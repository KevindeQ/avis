#ifndef AVIS_GRAPHICS_DETAIL_RENDER_GRAPH_EXECUTE_SENDER_H
#define AVIS_GRAPHICS_DETAIL_RENDER_GRAPH_EXECUTE_SENDER_H

// The sender behind render_graph::execute (docs/design/10-render-graph.md §7). The operation hands itself to the
// graph's executor (lib/avis/graphics/render_graph_executor.cpp), which records the passes in a bulk on the parallel
// scheduler and submits them; the number of recording groups is only known at runtime and D-66 rules out a
// type-erased sender. Include render_graph.h.

#include "avis/error.h"
#include "avis/rhi/queue.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace avis::graphics
{
    class frame_token;
    class render_graph;

    // The last submission of each queue the graph submitted to in one frame (at most one per queue), for end_frame. A
    // graphics point alone wouldn't cover async compute work that no later graphics batch waits on.
    struct submitted_frame
    {
        std::array<rhi::synchronization_point, 3> points{};
        std::uint32_t count = 0;

        std::span<const rhi::synchronization_point> last_submissions() const noexcept
        {
            return { points.data(), count };
        }
    };

    namespace detail
    {
        // Non-template part of an execute operation
        class render_graph_execute_operation
        {
        public:
            // Called once: inline from start() when execution fails before recording, otherwise on a parallel worker
            virtual void complete(result<submitted_frame> outcome) noexcept = 0;
            virtual void complete_stopped() noexcept = 0;
            // The receiver's stop request; the executor checks it once recording finished
            virtual bool stop_requested() const noexcept = 0;

        protected:
            render_graph_execute_operation(render_graph& graph, const frame_token& frame) noexcept :
                graph_{ &graph },
                frame_{ &frame }
            {}

            ~render_graph_execute_operation() = default;
            render_graph_execute_operation(const render_graph_execute_operation&) = delete;
            render_graph_execute_operation& operator=(const render_graph_execute_operation&) = delete;

            // Materialises, starts the recording, and completes later. The operation may complete (and be destroyed)
            // before begin() returns.
            void begin() noexcept;

        private:
            render_graph* graph_;
            const frame_token* frame_;
        };

        template<class Receiver>
        class render_graph_execute_operation_state final : public render_graph_execute_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            render_graph_execute_operation_state(Receiver receiver, render_graph& graph, const frame_token& frame) :
                render_graph_execute_operation{ graph, frame },
                receiver_{ std::move(receiver) }
            {}

            void start() & noexcept
            {
                if (stop_requested())
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                begin();
            }

            void complete(result<submitted_frame> outcome) noexcept override
            {
                if (outcome)
                {
                    stdexec::set_value(std::move(receiver_), *outcome);
                }
                else
                {
                    stdexec::set_error(std::move(receiver_), std::move(outcome.error()));
                }
            }

            void complete_stopped() noexcept override
            {
                stdexec::set_stopped(std::move(receiver_));
            }

            bool stop_requested() const noexcept override
            {
                return stdexec::get_stop_token(stdexec::get_env(receiver_)).stop_requested();
            }

        private:
            Receiver receiver_;
        };

        // Completes with set_value(submitted_frame), set_error(avis::error) or set_stopped(). Single-shot; the graph
        // and the frame token must outlive the operation.
        class render_graph_execute_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = avis::completions<submitted_frame>;

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) && -> render_graph_execute_operation_state<Receiver>
            {
                return { std::move(receiver), *graph_, *frame_ };
            }

        private:
            friend class graphics::render_graph;

            render_graph_execute_sender(render_graph& graph, const frame_token& frame) noexcept :
                graph_{ &graph },
                frame_{ &frame }
            {}

            render_graph* graph_;
            const frame_token* frame_;
        };

        static_assert(avis::sender_of<render_graph_execute_sender, submitted_frame>);
    } // namespace detail

    using detail::render_graph_execute_sender;
} // namespace avis::graphics

#endif
