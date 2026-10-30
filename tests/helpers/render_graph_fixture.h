#ifndef AVIS_TESTS_HELPERS_RENDER_GRAPH_FIXTURE_H
#define AVIS_TESTS_HELPERS_RENDER_GRAPH_FIXTURE_H

// A render graph on the test RHI (docs/tasks/09-render-graph.md, Open points): the build and compile tests need a
// device only because the graph's constructor takes RHI views; they record and submit nothing. The materialise tests
// begin frames on its scheduler.

#include "graphics_fixture.h"
#include "rhi_fixture.h"

#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/transient_resource_pool.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace avis::tests
{
    // Members are destroyed in reverse order: the graph, the pool, the scheduler, then the RHI
    class render_graph_fixture
    {
    public:
        // descriptors_per_frame: the scheduler's descriptor range per frame context
        explicit render_graph_fixture(
            const graphics::render_graph_configuration& configuration = {},
            const std::uint32_t descriptors_per_frame = test_frame_descriptor_count) :
            frames{ make_frames(hardware_interface, descriptors_per_frame) },
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
                   nullptr,
                   configuration }
        {}

        test_rhi hardware_interface;
        std::unique_ptr<graphics::frame_scheduler> frames;
        graphics::transient_resource_pool pool;
        graphics::render_graph graph;

    private:
        static std::unique_ptr<graphics::frame_scheduler>
            make_frames(test_rhi& hardware_interface, const std::uint32_t descriptors_per_frame)
        {
            graphics::frame_scheduler_configuration configuration = test_frame_scheduler_configuration();
            configuration.descriptors_per_frame = descriptors_per_frame;
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
