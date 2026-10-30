#ifndef AVIS_TESTS_HELPERS_RENDER_GRAPH_FIXTURE_H
#define AVIS_TESTS_HELPERS_RENDER_GRAPH_FIXTURE_H

// A render graph on the test RHI (docs/tasks/09-render-graph.md, Open points): the build and compile tests need a
// device only because the graph's constructor takes RHI views. They record and submit nothing.

#include "graphics_fixture.h"
#include "rhi_fixture.h"

#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/transient_resource_pool.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"

#include <memory>

namespace avis::tests
{
    // Members are destroyed in reverse order: the graph, the pool, the scheduler, then the RHI
    class render_graph_fixture
    {
    public:
        explicit render_graph_fixture(const graphics::render_graph_configuration& configuration = {}) :
            frames{ make_test_frame_scheduler(hardware_interface) },
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
    };
} // namespace avis::tests

#endif
