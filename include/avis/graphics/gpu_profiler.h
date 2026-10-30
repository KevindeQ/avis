#ifndef AVIS_GRAPHICS_GPU_PROFILER_H
#define AVIS_GRAPHICS_GPU_PROFILER_H

// Named per-frame GPU timings from timestamp queries (docs/design/17-graphics-frames-and-transfers.md §6, D-97).
// Follows the frame scheduler's contexts through the frame token; the scheduler doesn't know about it. Free of
// <windows.h> and D3D12.

#include "avis/error.h"
#include "avis/graphics/frame_token.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/transient.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace avis::graphics
{
    namespace detail
    {
        struct profiler_context; // lib/avis/graphics/gpu_profiler.cpp
    } // namespace detail

    struct gpu_profiler_configuration
    {
        std::uint32_t timestamps_per_frame = 1'024; // per queue type (2 per scope)
    };

    // A scope begin_timing opened. The default value is invalid: a dropped scope, which end_timing ignores.
    struct gpu_scope_id
    {
        std::uint32_t value = 0;

        constexpr bool is_valid() const noexcept
        {
            return value != 0;
        }
    };

    struct gpu_timing
    {
        std::string_view name; // valid as long as the span returned by completed_gpu_timings
        rhi::queue_type queue = rhi::queue_type::graphics;
        std::uint32_t depth = 0; // nesting depth of begin_timing scopes
        double start_milliseconds = 0.0; // relative to the frame's first timestamp on that queue
        double duration_milliseconds = 0.0;
    };

    class gpu_profiler
    {
    public:
        // One query set per queue type for each of the scheduler's frames_in_flight() contexts. A copy-queue set the
        // device can't create (unsupported_hardware) is skipped, and copy-queue scopes become no-ops. Fails with
        // invalid_argument for frames_in_flight outside 1..3 or fewer than 2 timestamps per frame.
        // Views (D-96): the query sets; each queue's timestamp_frequency()
        static result<std::unique_ptr<gpu_profiler>> create(
            rhi::transient_objects transient_objects,
            rhi::queue<rhi::queue_type::graphics> graphics_queue,
            rhi::queue<rhi::queue_type::compute> compute_queue,
            rhi::queue<rhi::queue_type::copy> copy_queue,
            std::uint32_t frames_in_flight,
            const gpu_profiler_configuration& configuration);

        // No frame may be recording; destroy before the RHI
        ~gpu_profiler();

        gpu_profiler(const gpu_profiler&) = delete;
        gpu_profiler& operator=(const gpu_profiler&) = delete;

        // Once per frame token, before any begin_timing for it: converts the results of frame N − F (dropped when
        // that frame wasn't prepared) and resets the context's query sets with frame.recycled_submissions().
        // Serialised with completed_gpu_timings by the frame chain.
        void prepare_frame(const frame_token& frame);

        // Opens a scope on recorder.submission_queue(). Scopes nest per thread: a scope begins and ends on one thread,
        // which records its list. Different threads may time different lists concurrently. Running out of
        // timestamps drops the scope (one warning per frame).
        template<rhi::queue_type Queue>
        gpu_scope_id
            begin_timing(const frame_token& frame, rhi::command_recorder<Queue>& recorder, std::string_view name)
        {
            const scope_slot slot = open_scope(frame, recorder.submission_queue(), name);
            if (slot.set != nullptr)
            {
                recorder.write_timestamp(*slot.set, slot.index);
            }
            return slot.scope;
        }

        template<rhi::queue_type Queue>
        void end_timing(const frame_token& frame, rhi::command_recorder<Queue>& recorder, gpu_scope_id scope)
        {
            const scope_slot slot = close_scope(frame, scope);
            if (slot.set != nullptr)
            {
                recorder.write_timestamp(*slot.set, slot.index);
            }
        }

        // Frame N − F's timings, ordered by queue, then by the order their scopes began; empty for the first F frames
        // and after a skipped prepare_frame. Valid until the next prepare_frame.
        std::span<const gpu_timing> completed_gpu_timings(const frame_token& frame) const;

    private:
        struct scope_slot
        {
            rhi::timestamp_query_set* set = nullptr; // null: write nothing
            std::uint32_t index = 0;
            gpu_scope_id scope;
        };

        gpu_profiler(
            std::uint32_t frames_in_flight,
            std::uint32_t scopes_per_queue,
            std::array<std::uint64_t, 3> frequencies,
            std::vector<std::unique_ptr<detail::profiler_context>> contexts) noexcept;

        scope_slot open_scope(const frame_token& frame, rhi::queue_type queue, std::string_view name) noexcept;
        scope_slot close_scope(const frame_token& frame, gpu_scope_id scope) noexcept;
        bool is_prepared(const frame_token& frame, const char* message) const noexcept;
        void convert_results(detail::profiler_context& context);

        std::uint32_t frames_in_flight_;
        std::uint32_t scopes_per_queue_;
        std::array<std::uint64_t, 3> frequencies_; // ticks per second, by queue_type
        std::vector<std::unique_ptr<detail::profiler_context>> contexts_;
        std::atomic<std::uint64_t> prepared_index_{ 0 }; // the last prepared frame; read by recording threads
    };
} // namespace avis::graphics

#endif
