#ifndef AVIS_GRAPHICS_PASS_CONTEXT_H
#define AVIS_GRAPHICS_PASS_CONTEXT_H

// What an execute callback records with (docs/design/10-render-graph.md §3.1): a recorder of the pass's own type and
// the frame's services. The graph's executor creates the contexts; they are valid during the execute call only.

#include "avis/rhi/command_list.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace avis::graphics
{
    class frame_scheduler;
    class frame_token;

    namespace detail
    {
        // The part of a context that doesn't depend on the recorder type
        class pass_context_base
        {
        public:
            std::uint32_t render_width() const noexcept
            {
                return render_width_;
            }

            std::uint32_t render_height() const noexcept
            {
                return render_height_;
            }

        protected:
            pass_context_base(
                const frame_token* frame,
                frame_scheduler* frames,
                const std::uint32_t render_width,
                const std::uint32_t render_height) noexcept :
                frame_{ frame },
                frames_{ frames },
                render_width_{ render_width },
                render_height_{ render_height }
            {}

            // frame_scheduler::allocate_frame_memory, 256-byte aligned. Throws avis::runtime_error with the scheduler's
            // error (transient_memory_exhausted when the frame's memory is full, 10 §7), or invalid_argument after a
            // failed debug check when the context has no frame (outside execute).
            rhi::gpu_address allocate_constant_bytes(std::span<const std::byte> data) const;

        private:
            const frame_token* frame_;
            frame_scheduler* frames_;
            std::uint32_t render_width_;
            std::uint32_t render_height_;
        };
    } // namespace detail

    // The context of a pass whose recorder records Queue's operations: graphics, compute or copy passes, whichever
    // queue they land on (a compute or copy pass on the graphics queue gets a narrowed recorder, D-92)
    template<rhi::queue_type Queue>
    class pass_context : public detail::pass_context_base
    {
    public:
        using recorder_type = rhi::command_recorder<Queue>;

        // Made by the executor (9.9)
        pass_context(
            recorder_type& recorder,
            const frame_token* frame,
            frame_scheduler* frames,
            const std::uint32_t render_width,
            const std::uint32_t render_height) noexcept :
            pass_context_base{ frame, frames, render_width, render_height },
            recorder_{ &recorder }
        {}

        recorder_type& recorder() noexcept
        {
            return *recorder_;
        }

        // Frame memory for a root CBV (frame_scheduler::write_frame_constants, 17 §4). Not on copy contexts.
        rhi::gpu_address allocate_constants(const std::span<const std::byte> data)
            requires(Queue != rhi::queue_type::copy)
        {
            return allocate_constant_bytes(data);
        }

        template<typename T>
            requires(Queue != rhi::queue_type::copy && std::is_trivially_copyable_v<T>)
        rhi::gpu_address allocate_constants(const T& value)
        {
            return allocate_constant_bytes(std::as_bytes(std::span{ &value, 1 }));
        }

    private:
        recorder_type* recorder_;
    };

    using graphics_pass_context = pass_context<rhi::queue_type::graphics>;
    using compute_pass_context = pass_context<rhi::queue_type::compute>;
    using copy_pass_context = pass_context<rhi::queue_type::copy>;
} // namespace avis::graphics

#endif
