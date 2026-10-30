#ifndef AVIS_TESTS_HELPERS_RENDER_GRAPH_FIXTURE_H
#define AVIS_TESTS_HELPERS_RENDER_GRAPH_FIXTURE_H

// A render graph on the test RHI (docs/tasks/09-render-graph.md, Open points): the build and compile tests need a
// device only because the graph's constructor takes RHI views; they record and submit nothing. The execute and frame
// tests begin frames on its scheduler; they need a runtime (runtime_fixture.h) for the parallel scheduler, declared
// before the fixture.

#include "graphics_fixture.h"
#include "rhi_fixture.h"

#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/gpu_profiler.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/transient_resource_pool.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"
#include "avis/threading/parallel_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace avis::tests
{
    struct render_graph_fixture_options
    {
        graphics::render_graph_configuration graph;
        std::uint32_t frames_in_flight = 2;
        std::uint32_t descriptors_per_frame = test_frame_descriptor_count; // per frame context
        bool profiler = false;
        bool gpu_based_validation = false;
    };

    // Members are destroyed in reverse order: the graph, the pool, the profiler, the scheduler, then the RHI
    class render_graph_fixture
    {
    public:
        explicit render_graph_fixture(const render_graph_fixture_options& options) :
            hardware_interface{ rhi_configuration(options) },
            frames{ make_frames(hardware_interface, options) },
            profiler{ options.profiler ? make_test_gpu_profiler(hardware_interface, options.frames_in_flight) :
                                         nullptr },
            pool{ rhi::memory{ *hardware_interface },
                  rhi::resources{ *hardware_interface },
                  frames->frames_in_flight() },
            graph{ rhi::resources{ *hardware_interface },
                   rhi::command_lists{ *hardware_interface },
                   rhi::queue<rhi::queue_type::graphics>{ *hardware_interface },
                   rhi::queue<rhi::queue_type::compute>{ *hardware_interface },
                   rhi::queue<rhi::queue_type::copy>{ *hardware_interface },
                   pool,
                   *frames,
                   profiler.get(),
                   options.graph }
        {}

        explicit render_graph_fixture(
            const graphics::render_graph_configuration& configuration = {},
            const std::uint32_t descriptors_per_frame = test_frame_descriptor_count) :
            render_graph_fixture{ render_graph_fixture_options{ .graph = configuration,
                                                                .descriptors_per_frame = descriptors_per_frame } }
        {}

        // A frame whose context is free: requires begin_frame to complete inline, so the frame F before it must be
        // complete (wait() on its submissions). Prepares the profiler for it.
        graphics::frame_token begin_frame()
        {
            graphics::frame_token frame = begin_frame_inline(*frames);
            if (profiler != nullptr)
            {
                profiler->prepare_frame(frame);
            }
            return frame;
        }

        // Executes the compiled graph and waits for the sender (not for the GPU)
        result<graphics::submitted_frame> execute(const graphics::frame_token& frame)
        {
            return avis::threading::sync_wait_result(graph.execute(frame));
        }

        // Waits for every submission of a frame on the GPU and polls completions
        void wait(const graphics::submitted_frame& submitted)
        {
            for (const rhi::synchronization_point point : submitted.last_submissions())
            {
                hardware_interface.wait_for(point);
            }
        }

        test_rhi hardware_interface;
        std::unique_ptr<graphics::frame_scheduler> frames;
        std::unique_ptr<graphics::gpu_profiler> profiler;
        graphics::transient_resource_pool pool;
        graphics::render_graph graph;

    private:
        static rhi::render_hardware_interface_configuration
            rhi_configuration(const render_graph_fixture_options& options)
        {
            rhi::render_hardware_interface_configuration configuration;
            configuration.debug.gpu_based_validation = options.gpu_based_validation;
            return configuration;
        }

        static std::unique_ptr<graphics::frame_scheduler>
            make_frames(test_rhi& hardware_interface, const render_graph_fixture_options& options)
        {
            graphics::frame_scheduler_configuration configuration =
                test_frame_scheduler_configuration(options.frames_in_flight);
            configuration.descriptors_per_frame = options.descriptors_per_frame;
            auto scheduler = graphics::frame_scheduler::create(
                rhi::synchronization{ *hardware_interface },
                rhi::transient_objects{ *hardware_interface },
                configuration);
            REQUIRE(scheduler.has_value());
            return std::move(*scheduler);
        }
    };
} // namespace avis::tests

#endif
