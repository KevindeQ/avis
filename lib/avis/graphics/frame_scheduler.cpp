#include "avis/graphics/frame_scheduler.h"

#include "avis/detail/check.h"
#include "avis/detail/safe_error.h"
#include "avis/graphics/error.h"
#include "avis/log.h"
#include "avis/rhi/error.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <string>
#include <utility>

namespace avis::graphics
{
    namespace detail
    {
        // One frame context (17 §3): the per-frame objects frame N uses, and what the frame that used it left behind
        struct frame_context
        {
            frame_context(
                rhi::linear_upload_allocator upload_allocator, rhi::descriptor_range descriptor_range) noexcept :
                allocator{ std::move(upload_allocator) },
                descriptors{ std::move(descriptor_range) }
            {}

            std::span<const rhi::synchronization_point> last() const noexcept
            {
                return { last_submissions.data(), last_count };
            }

            std::span<const rhi::synchronization_point> recycled() const noexcept
            {
                return { recycled_submissions.data(), recycled_count };
            }

            rhi::linear_upload_allocator allocator;
            rhi::descriptor_range descriptors;

            // end_frame's points for the frame that last held the context: at most one per queue
            std::array<rhi::synchronization_point, 3> last_submissions{};
            std::uint32_t last_count = 0;

            // The points the last recycle reset with, for frame_token::recycled_submissions()
            std::array<rhi::synchronization_point, 3> recycled_submissions{};
            std::uint32_t recycled_count = 0;

            std::uint64_t frame_index = 0; // the frame that last held the context; 0 before its first frame

            // Set by the first exhaustion warning of the frame, cleared by the recycle: one warning per frame each
            // for upload memory and for descriptors
            std::atomic<bool> memory_exhaustion_logged{ false };
            std::atomic<bool> descriptor_exhaustion_logged{ false };
        };
    } // namespace detail

    namespace
    {
        constexpr std::string_view logger_name = "graphics";
        constexpr std::uint32_t maximum_frames_in_flight = 3;

        avis::error invalid(std::string message)
        {
            return make_error(error_kind::invalid_argument, std::move(message));
        }
    } // namespace

    // ---- frame_token ----

    frame_token::frame_token(
        frame_scheduler& owner,
        const std::uint64_t index,
        const std::uint32_t context_index,
        const std::uint64_t completed_index) noexcept :
        owner_{ &owner },
        index_{ index },
        context_index_{ context_index },
        completed_index_{ completed_index }
    {}

    frame_token::frame_token(frame_token&& other) noexcept :
        owner_{ std::exchange(other.owner_, nullptr) },
        index_{ other.index_ },
        context_index_{ other.context_index_ },
        completed_index_{ other.completed_index_ }
    {}

    frame_token& frame_token::operator=(frame_token&& other) noexcept
    {
        if (this != &other)
        {
            end_abandoned();
            owner_ = std::exchange(other.owner_, nullptr);
            index_ = other.index_;
            context_index_ = other.context_index_;
            completed_index_ = other.completed_index_;
        }
        return *this;
    }

    frame_token::~frame_token()
    {
        end_abandoned();
    }

    std::span<const rhi::synchronization_point> frame_token::recycled_submissions() const noexcept
    {
        return owner_ != nullptr ? owner_->recycled_submissions(*this) : std::span<const rhi::synchronization_point>{};
    }

    void frame_token::end_abandoned() noexcept
    {
        if (owner_ == nullptr)
        {
            return;
        }
        try
        {
            log::get(logger_name)->debug("frame {} ended by its token without end_frame (no submissions)", index_);
        }
        catch (...)
        {}
        try
        {
            owner_->end_frame(std::move(*this), {});
        }
        catch (...)
        {}
        owner_ = nullptr; // also when end_frame refused it: never end twice
    }

    // ---- frame_scheduler ----

    result<std::unique_ptr<frame_scheduler>> frame_scheduler::create(
        const rhi::synchronization synchronization,
        rhi::transient_objects transient_objects,
        const frame_scheduler_configuration& configuration)
    {
        if (configuration.frames_in_flight == 0 || configuration.frames_in_flight > maximum_frames_in_flight)
        {
            return std::unexpected(invalid(
                std::format("frame_scheduler: frames_in_flight is {}, expected 1..3", configuration.frames_in_flight)));
        }
        if (configuration.upload_memory_per_frame == 0)
        {
            return std::unexpected(invalid("frame_scheduler: upload_memory_per_frame is 0"));
        }
        if (configuration.descriptors_per_frame == 0)
        {
            return std::unexpected(invalid("frame_scheduler: descriptors_per_frame is 0"));
        }

        std::vector<std::unique_ptr<detail::frame_context>> contexts;
        contexts.reserve(configuration.frames_in_flight);
        for (std::uint32_t index = 0; index < configuration.frames_in_flight; ++index)
        {
            auto allocator = transient_objects.create_linear_upload_allocator(
                configuration.upload_memory_per_frame, std::format("frame context {} upload", index));
            if (!allocator)
            {
                return std::unexpected(std::move(allocator.error()));
            }
            auto range = transient_objects.create_descriptor_range(
                configuration.descriptors_per_frame, std::format("frame context {} descriptors", index));
            if (!range)
            {
                return std::unexpected(std::move(range.error()));
            }
            contexts.push_back(std::make_unique<detail::frame_context>(std::move(*allocator), std::move(*range)));
        }

        return std::unique_ptr<frame_scheduler>{ new frame_scheduler{
            synchronization, configuration, std::move(contexts) } };
    }

    frame_scheduler::frame_scheduler(
        const rhi::synchronization synchronization,
        const frame_scheduler_configuration& configuration,
        std::vector<std::unique_ptr<detail::frame_context>> contexts) noexcept :
        synchronization_{ synchronization },
        configuration_{ configuration },
        contexts_{ std::move(contexts) }
    {}

    frame_scheduler::~frame_scheduler()
    {
        AVIS_CHECK(
            logger_name,
            pacing_.load(std::memory_order_acquire) == pacing::idle,
            "frame_scheduler destroyed with a frame outstanding (a begin_frame or an un-ended token)");
    }

    frame_sender frame_scheduler::begin_frame() noexcept
    {
        return frame_sender{ *this };
    }

    std::optional<frame_token> frame_scheduler::try_begin_frame()
    {
        if (!try_enter_waiting())
        {
            return std::nullopt;
        }
        try
        {
            if (!next_context_is_free())
            {
                leave_waiting();
                return std::nullopt;
            }
            return recycle_and_deliver();
        }
        catch (...)
        {
            leave_waiting();
            throw;
        }
    }

    void frame_scheduler::end_frame(
        frame_token&& frame, const std::span<const rhi::synchronization_point> last_submissions)
    {
        if (!AVIS_CHECK(logger_name, frame.owner_ == this, "end_frame: the token isn't a frame of this scheduler"))
        {
            return;
        }
        if (!AVIS_CHECK(
                logger_name,
                pacing_.load(std::memory_order_acquire) == pacing::recording &&
                    current_index_.load(std::memory_order_relaxed) == frame.index_,
                "end_frame: the token isn't the frame being recorded"))
        {
            return;
        }

        // The latest point per queue; points with value 0 wait for nothing
        std::array<rhi::synchronization_point, 3> per_queue{};
        for (const rhi::synchronization_point point : last_submissions)
        {
            if (point.value == 0)
            {
                continue;
            }
            auto& latest = per_queue[static_cast<std::size_t>(point.queue)];
            latest = latest.value == 0 ? point : rhi::latest(latest, point);
        }

        detail::frame_context& context = *contexts_[frame.context_index_];
        context.last_count = 0;
        for (const rhi::synchronization_point point : per_queue)
        {
            if (point.value != 0)
            {
                context.last_submissions[context.last_count++] = point;
            }
        }

        frame.owner_ = nullptr;
        current_index_.store(0, std::memory_order_relaxed);
        pacing_.store(pacing::idle, std::memory_order_release);
    }

    std::uint32_t frame_scheduler::frames_in_flight() const noexcept
    {
        return configuration_.frames_in_flight;
    }

    bool frame_scheduler::try_enter_waiting() noexcept
    {
        pacing expected = pacing::idle;
        const bool entered = pacing_.compare_exchange_strong(
            expected, pacing::waiting, std::memory_order_acq_rel, std::memory_order_acquire);
        return AVIS_CHECK(
            logger_name,
            entered,
            "begin_frame while a frame is outstanding: end the current frame (or drop its token) first");
    }

    void frame_scheduler::leave_waiting() noexcept
    {
        pacing_.store(pacing::idle, std::memory_order_release);
    }

    detail::frame_context& frame_scheduler::next_context() noexcept
    {
        return *contexts_[(delivered_index_ + 1) % configuration_.frames_in_flight];
    }

    bool frame_scheduler::next_context_is_free() const
    {
        const detail::frame_context& context = *contexts_[(delivered_index_ + 1) % configuration_.frames_in_flight];
        return std::ranges::all_of(
            context.last(),
            [this](const rhi::synchronization_point point) { return synchronization_.is_complete(point); });
    }

    frame_token frame_scheduler::recycle_and_deliver()
    {
        const std::uint64_t index = delivered_index_ + 1;
        const auto context_index = static_cast<std::uint32_t>(index % configuration_.frames_in_flight);
        detail::frame_context& context = *contexts_[context_index];

        // 1. The completed index: the frame this context last held (N − F, or 0 for the first F frames), or a newer
        // one whose context's submissions are already complete
        std::uint64_t completed = std::max(completed_index_, context.frame_index);
        for (const auto& other : contexts_)
        {
            if (other.get() != &context && other->frame_index > completed &&
                std::ranges::all_of(
                    other->last(),
                    [this](const rhi::synchronization_point point) { return synchronization_.is_complete(point); }))
            {
                completed = other->frame_index;
            }
        }
        completed_index_ = completed;

        // 2 and 3. Reset with the points the GPU just finished, kept for recycled_submissions()
        context.recycled_submissions = context.last_submissions;
        context.recycled_count = context.last_count;
        context.allocator.reset(context.recycled());
        context.descriptors.reset(context.recycled());
        context.last_count = 0;
        context.frame_index = index;
        context.memory_exhaustion_logged.store(false, std::memory_order_relaxed);
        context.descriptor_exhaustion_logged.store(false, std::memory_order_relaxed);

        delivered_index_ = index;
        current_index_.store(index, std::memory_order_relaxed);
        pacing_.store(pacing::recording, std::memory_order_release);
        return frame_token{ *this, index, context_index, completed };
    }

    std::span<const rhi::synchronization_point>
        frame_scheduler::recycled_submissions(const frame_token& frame) const noexcept
    {
        return contexts_[frame.context_index_]->recycled();
    }

    // ---- per-frame services ----

    result<rhi::upload_allocation> frame_scheduler::allocate_frame_memory(
        const frame_token& frame, const std::size_t size, const std::size_t alignment)
    {
        auto context = recording_context(frame, "allocate_frame_memory");
        if (!context)
        {
            return std::unexpected(std::move(context.error()));
        }
        auto allocation = (*context)->allocator.allocate(size, alignment);
        if (!allocation && rhi::is_error(allocation.error(), rhi::error_kind::transient_memory_exhausted))
        {
            warn_memory_exhausted(**context, frame);
        }
        return allocation;
    }

    result<rhi::bindless_index> frame_scheduler::create_frame_srv(
        const frame_token& frame, const rhi::texture_handle texture, const rhi::subresource_range range)
    {
        auto context = recording_context(frame, "create_frame_srv");
        if (!context)
        {
            return std::unexpected(std::move(context.error()));
        }
        return checked_view(**context, frame, (*context)->descriptors.create_srv(texture, range));
    }

    result<rhi::bindless_index> frame_scheduler::create_frame_uav(
        const frame_token& frame, const rhi::texture_handle texture, const std::uint32_t mip)
    {
        auto context = recording_context(frame, "create_frame_uav");
        if (!context)
        {
            return std::unexpected(std::move(context.error()));
        }
        return checked_view(**context, frame, (*context)->descriptors.create_uav(texture, mip));
    }

    result<rhi::bindless_index>
        frame_scheduler::create_frame_srv(const frame_token& frame, const rhi::buffer_handle buffer)
    {
        auto context = recording_context(frame, "create_frame_srv");
        if (!context)
        {
            return std::unexpected(std::move(context.error()));
        }
        return checked_view(**context, frame, (*context)->descriptors.create_srv(buffer));
    }

    result<rhi::bindless_index>
        frame_scheduler::create_frame_uav(const frame_token& frame, const rhi::buffer_handle buffer)
    {
        auto context = recording_context(frame, "create_frame_uav");
        if (!context)
        {
            return std::unexpected(std::move(context.error()));
        }
        return checked_view(**context, frame, (*context)->descriptors.create_uav(buffer));
    }

    result<detail::frame_context*>
        frame_scheduler::recording_context(const frame_token& frame, const char* const service) noexcept
    {
        // Lock-free: the token is only current between its delivery and end_frame, which the caller orders before
        // and after its per-frame calls
        const bool recording =
            frame.owner_ == this && frame.index_ != 0 && current_index_.load(std::memory_order_relaxed) == frame.index_;
        if (!AVIS_CHECK(
                logger_name, recording, "per-frame service called with a token that isn't the frame being recorded"))
        {
            return std::unexpected(
                avis::detail::safe_error(
                    error_domain::graphics, static_cast<std::int32_t>(error_kind::invalid_argument), service));
        }
        return contexts_[frame.context_index_].get();
    }

    void frame_scheduler::warn_memory_exhausted(detail::frame_context& context, const frame_token& frame) const noexcept
    {
        if (context.memory_exhaustion_logged.exchange(true, std::memory_order_relaxed))
        {
            return;
        }
        try
        {
            log::get(logger_name)
                ->warn(
                    "frame {}: frame upload memory is exhausted (high-water mark {} of {} bytes); raise "
                    "frame_scheduler_configuration::upload_memory_per_frame",
                    frame.index_,
                    context.allocator.high_water_mark(),
                    context.allocator.capacity());
        }
        catch (...)
        {}
    }

    void frame_scheduler::warn_descriptors_exhausted(
        detail::frame_context& context, const frame_token& frame) const noexcept
    {
        if (context.descriptor_exhaustion_logged.exchange(true, std::memory_order_relaxed))
        {
            return;
        }
        try
        {
            log::get(logger_name)
                ->warn(
                    "frame {}: frame descriptors are exhausted ({} views); raise "
                    "frame_scheduler_configuration::descriptors_per_frame",
                    frame.index_,
                    context.descriptors.capacity());
        }
        catch (...)
        {}
    }

    result<rhi::bindless_index> frame_scheduler::checked_view(
        detail::frame_context& context, const frame_token& frame, result<rhi::bindless_index> view) const noexcept
    {
        if (!view && rhi::is_error(view.error(), rhi::error_kind::transient_memory_exhausted))
        {
            warn_descriptors_exhausted(context, frame);
        }
        return view;
    }

    // ---- detail::frame_operation ----

    namespace detail
    {
        result<void> frame_operation::begin() noexcept
        {
            if (!scheduler_->try_enter_waiting())
            {
                return std::unexpected(
                    avis::detail::safe_error(
                        error_domain::generic,
                        static_cast<std::int32_t>(generic_error_kind::logic_error),
                        "begin_frame while a frame is outstanding"));
            }
            return {};
        }

        result<rhi::gpu_wait_sender> frame_operation::wait_for_context() noexcept
        {
            try
            {
                return scheduler_->synchronization_.when_complete(scheduler_->next_context().last());
            }
            catch (...)
            {
                scheduler_->leave_waiting();
                return std::unexpected(avis::detail::current_exception_error());
            }
        }

        result<frame_token> frame_operation::deliver() noexcept
        {
            try
            {
                return scheduler_->recycle_and_deliver();
            }
            catch (...)
            {
                scheduler_->leave_waiting();
                return std::unexpected(avis::detail::current_exception_error());
            }
        }

        void frame_operation::abandon() noexcept
        {
            scheduler_->leave_waiting();
        }
    } // namespace detail
} // namespace avis::graphics
