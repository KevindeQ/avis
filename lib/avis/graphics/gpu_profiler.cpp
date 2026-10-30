#include "avis/graphics/gpu_profiler.h"

#include "avis/detail/check.h"
#include "avis/graphics/error.h"
#include "avis/log.h"
#include "avis/rhi/error.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace avis::graphics
{
    namespace detail
    {
        // A fixed-capacity character buffer with an atomic bump offset, so concurrent begin_timing calls copy names
        // without a lock. A name that doesn't fit is truncated (empty once the arena is full).
        struct name_arena
        {
            explicit name_arena(const std::size_t size) : bytes{ std::make_unique<char[]>(size) }, capacity{ size } {}

            std::string_view copy(const std::string_view name) noexcept
            {
                if (name.empty())
                {
                    return {};
                }
                const std::size_t offset = used.fetch_add(name.size(), std::memory_order_relaxed);
                if (offset >= capacity)
                {
                    return {};
                }
                const std::size_t length = std::min(name.size(), capacity - offset);
                std::memcpy(bytes.get() + offset, name.data(), length);
                return { bytes.get() + offset, length };
            }

            void clear() noexcept
            {
                used.store(0, std::memory_order_relaxed);
            }

            std::unique_ptr<char[]> bytes;
            std::size_t capacity;
            std::atomic<std::size_t> used{ 0 };
        };

        struct scope_record
        {
            std::string_view name;
            std::uint32_t depth = 0;
            bool ended = false;
        };

        // The profiler's state for one frame context (17 §6)
        struct profiler_context
        {
            profiler_context(const std::uint32_t scopes_per_queue, const std::size_t arena_size) :
                arenas{ name_arena{ arena_size }, name_arena{ arena_size } }
            {
                for (auto& records : scopes)
                {
                    records.resize(scopes_per_queue);
                }
            }

            std::array<std::optional<rhi::timestamp_query_set>, 3> sets; // by queue_type; empty: no timestamps
            std::array<std::atomic<std::uint32_t>, 3> next_scope{}; // scopes taken this frame, by queue_type
            std::array<std::vector<scope_record>, 3> scopes; // scope i uses timestamps 2i and 2i + 1
            std::array<name_arena, 2> arenas; // one collects this frame's names, the other backs the results
            std::uint32_t collecting = 0;
            std::vector<gpu_timing> results; // frame N − F's
            std::uint64_t written_index = 0; // the frame last prepared on this context: the one that wrote it
            std::atomic<bool> overflow_logged{ false };
        };
    } // namespace detail

    namespace
    {
        constexpr std::string_view logger_name = "graphics";
        constexpr std::uint32_t maximum_frames_in_flight = 3;
        constexpr std::size_t name_bytes_per_scope = 64; // sizes the name arenas; longer names share the slack
        constexpr std::uint32_t scope_bits = 24; // gpu_scope_id: (queue + 1) << 24 | scope

        constexpr std::array<std::string_view, 3> queue_names{ "graphics", "compute", "copy" };

        std::string_view name_of(const rhi::queue_type queue) noexcept
        {
            return queue_names[static_cast<std::size_t>(queue)];
        }

        // Nesting depth per thread and profiler (task 7 open point "profiler nesting key", option a): a scope begins
        // and ends on the thread recording its list, so the thread stands in for the list
        struct thread_depth
        {
            const void* profiler;
            std::uint32_t depth;
        };

        thread_local std::vector<thread_depth> thread_depths;

        std::uint32_t* depth_of(const void* const profiler) noexcept
        {
            for (thread_depth& entry : thread_depths)
            {
                if (entry.profiler == profiler)
                {
                    return &entry.depth;
                }
            }
            try
            {
                return &thread_depths.emplace_back(thread_depth{ profiler, 0 }).depth;
            }
            catch (...)
            {
                return nullptr;
            }
        }
    } // namespace

    result<std::unique_ptr<gpu_profiler>> gpu_profiler::create(
        rhi::transient_objects transient_objects,
        const rhi::queue<rhi::queue_type::graphics> graphics_queue,
        const rhi::queue<rhi::queue_type::compute> compute_queue,
        const rhi::queue<rhi::queue_type::copy> copy_queue,
        const std::uint32_t frames_in_flight,
        const gpu_profiler_configuration& configuration)
    {
        if (frames_in_flight == 0 || frames_in_flight > maximum_frames_in_flight)
        {
            return std::unexpected(make_error(
                error_kind::invalid_argument,
                std::format("gpu_profiler: frames_in_flight is {}, expected 1..3", frames_in_flight)));
        }
        const std::uint32_t scopes_per_queue = std::min(configuration.timestamps_per_frame / 2, (1u << scope_bits) - 1);
        if (scopes_per_queue == 0)
        {
            return std::unexpected(make_error(
                error_kind::invalid_argument,
                std::format(
                    "gpu_profiler: timestamps_per_frame is {}, expected at least 2",
                    configuration.timestamps_per_frame)));
        }

        const std::array<std::uint64_t, 3> frequencies{ graphics_queue.timestamp_frequency(),
                                                        compute_queue.timestamp_frequency(),
                                                        copy_queue.timestamp_frequency() };

        std::vector<std::unique_ptr<detail::profiler_context>> contexts;
        contexts.reserve(frames_in_flight);
        for (std::uint32_t index = 0; index < frames_in_flight; ++index)
        {
            auto context = std::make_unique<detail::profiler_context>(
                scopes_per_queue, scopes_per_queue * 3 * name_bytes_per_scope);
            for (std::size_t queue = 0; queue < 3; ++queue)
            {
                const auto type = static_cast<rhi::queue_type>(queue);
                if (frequencies[queue] == 0)
                {
                    continue; // the queue has no timestamps
                }
                auto set = transient_objects.create_timestamp_query_set(
                    type, scopes_per_queue * 2, std::format("gpu profiler context {} {}", index, name_of(type)));
                if (!set)
                {
                    if (type == rhi::queue_type::copy &&
                        rhi::is_error(set.error(), rhi::error_kind::unsupported_hardware))
                    {
                        continue; // no copy-queue timestamps on this device: copy scopes are no-ops
                    }
                    return std::unexpected(std::move(set.error()));
                }
                context->sets[queue].emplace(std::move(*set));
            }
            contexts.push_back(std::move(context));
        }

        return std::unique_ptr<gpu_profiler>{ new gpu_profiler{
            frames_in_flight, scopes_per_queue, frequencies, std::move(contexts) } };
    }

    gpu_profiler::gpu_profiler(
        const std::uint32_t frames_in_flight,
        const std::uint32_t scopes_per_queue,
        const std::array<std::uint64_t, 3> frequencies,
        std::vector<std::unique_ptr<detail::profiler_context>> contexts) noexcept :
        frames_in_flight_{ frames_in_flight },
        scopes_per_queue_{ scopes_per_queue },
        frequencies_{ frequencies },
        contexts_{ std::move(contexts) }
    {}

    gpu_profiler::~gpu_profiler() = default;

    void gpu_profiler::prepare_frame(const frame_token& frame)
    {
        const std::uint64_t index = frame.index();
        if (!AVIS_CHECK(
                logger_name,
                index > prepared_index_.load(std::memory_order_relaxed),
                "prepare_frame: frame indices must increase, and each token is prepared once"))
        {
            return;
        }
        if (!AVIS_CHECK(
                logger_name,
                frame.context_index() < contexts_.size(),
                "prepare_frame: the token's context is outside the profiler's frames_in_flight"))
        {
            return;
        }
        detail::profiler_context& context = *contexts_[frame.context_index()];

        // Results: only when the frame that last wrote this context is N − F; older ticks are dropped
        if (context.written_index != 0 && context.written_index + frames_in_flight_ == index)
        {
            convert_results(context);
        }
        else
        {
            context.results.clear();
        }

        // The arena that collected frame N − F's names now backs its results; the other one collects frame N's
        context.collecting = 1 - context.collecting;
        context.arenas[context.collecting].clear();

        // The query sets are free: frame N − F is complete, and so is any older frame that last wrote them (17 §6)
        for (auto& set : context.sets)
        {
            if (set)
            {
                set->reset(frame.recycled_submissions());
            }
        }
        for (auto& next : context.next_scope)
        {
            next.store(0, std::memory_order_relaxed);
        }
        context.overflow_logged.store(false, std::memory_order_relaxed);
        context.written_index = index;
        prepared_index_.store(index, std::memory_order_release);
    }

    std::span<const gpu_timing> gpu_profiler::completed_gpu_timings(const frame_token& frame) const
    {
        if (!is_prepared(frame, "completed_gpu_timings for a frame that wasn't prepared"))
        {
            return {};
        }
        return contexts_[frame.context_index()]->results;
    }

    bool gpu_profiler::is_prepared(const frame_token& frame, const char* const message) const noexcept
    {
        return AVIS_CHECK(
            logger_name,
            frame.index() != 0 && frame.index() == prepared_index_.load(std::memory_order_acquire) &&
                frame.context_index() < contexts_.size(),
            message);
    }

    gpu_profiler::scope_slot gpu_profiler::open_scope(
        const frame_token& frame, const rhi::queue_type queue, const std::string_view name) noexcept
    {
        if (!is_prepared(frame, "begin_timing for a frame that wasn't prepared"))
        {
            return {};
        }
        detail::profiler_context& context = *contexts_[frame.context_index()];
        const auto queue_index = static_cast<std::size_t>(queue);
        if (!context.sets[queue_index])
        {
            return {}; // no timestamps on this queue
        }

        const std::uint32_t scope = context.next_scope[queue_index].fetch_add(1, std::memory_order_relaxed);
        if (scope >= scopes_per_queue_)
        {
            if (!context.overflow_logged.exchange(true, std::memory_order_relaxed))
            {
                try
                {
                    log::get(logger_name)
                        ->warn(
                            "frame {}: the GPU profiler ran out of timestamps on the {} queue ({} scopes); later "
                            "scopes "
                            "are dropped; raise gpu_profiler_configuration::timestamps_per_frame",
                            frame.index(),
                            name_of(queue),
                            scopes_per_queue_);
                }
                catch (...)
                {}
            }
            return {};
        }

        std::uint32_t* const depth = depth_of(this);
        detail::scope_record& record = context.scopes[queue_index][scope];
        record.name = context.arenas[context.collecting].copy(name);
        record.depth = depth != nullptr ? (*depth)++ : 0;
        record.ended = false;

        return { .set = &*context.sets[queue_index],
                 .index = scope * 2,
                 .scope = { (static_cast<std::uint32_t>(queue_index + 1) << scope_bits) | scope } };
    }

    gpu_profiler::scope_slot gpu_profiler::close_scope(const frame_token& frame, const gpu_scope_id scope) noexcept
    {
        if (!scope.is_valid())
        {
            return {};
        }
        if (!is_prepared(frame, "end_timing for a frame that wasn't prepared"))
        {
            return {};
        }
        detail::profiler_context& context = *contexts_[frame.context_index()];
        const std::size_t queue_index = (scope.value >> scope_bits) - 1;
        const std::uint32_t index = scope.value & ((1u << scope_bits) - 1);
        if (!AVIS_CHECK(
                logger_name,
                queue_index < 3 && context.sets[queue_index] && index < scopes_per_queue_,
                "end_timing: the scope id isn't one begin_timing returned"))
        {
            return {};
        }

        context.scopes[queue_index][index].ended = true;
        if (std::uint32_t* const depth = depth_of(this); depth != nullptr && *depth > 0)
        {
            --*depth;
        }
        return { .set = &*context.sets[queue_index], .index = index * 2 + 1, .scope = scope };
    }

    void gpu_profiler::convert_results(detail::profiler_context& context)
    {
        context.results.clear();
        for (std::size_t queue = 0; queue < 3; ++queue)
        {
            const std::uint32_t count =
                std::min(context.next_scope[queue].load(std::memory_order_relaxed), scopes_per_queue_);
            if (!context.sets[queue] || count == 0 || frequencies_[queue] == 0)
            {
                continue;
            }
            const std::span<const std::uint64_t> ticks = context.sets[queue]->ticks(0, count * 2);
            if (ticks.size() != count * 2)
            {
                continue;
            }
            const std::vector<detail::scope_record>& scopes = context.scopes[queue];

            std::uint64_t first = std::numeric_limits<std::uint64_t>::max();
            for (std::uint32_t scope = 0; scope < count; ++scope)
            {
                if (scopes[scope].ended)
                {
                    first = std::min(first, ticks[scope * 2]);
                }
            }

            const double milliseconds_per_tick = 1000.0 / static_cast<double>(frequencies_[queue]);
            for (std::uint32_t scope = 0; scope < count; ++scope)
            {
                if (!scopes[scope].ended)
                {
                    continue; // never ended: its second timestamp wasn't written
                }
                const std::uint64_t begin = ticks[scope * 2];
                const std::uint64_t end = ticks[scope * 2 + 1];
                context.results.push_back(
                    gpu_timing{
                        .name = scopes[scope].name,
                        .queue = static_cast<rhi::queue_type>(queue),
                        .depth = scopes[scope].depth,
                        .start_milliseconds = static_cast<double>(begin - first) * milliseconds_per_tick,
                        .duration_milliseconds =
                            end >= begin ? static_cast<double>(end - begin) * milliseconds_per_tick : 0.0,
                    });
            }
        }
    }
} // namespace avis::graphics
