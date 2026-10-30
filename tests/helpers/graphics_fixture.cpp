#include "graphics_fixture.h"

#include "avis/rhi/command_list.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <utility>

namespace avis::tests
{
    template<rhi::queue_type Queue>
    rhi::synchronization_point submit_marker(test_rhi& hardware_interface)
    {
        auto list = rhi::command_lists{ *hardware_interface }.acquire<Queue>("marker");
        REQUIRE(list.has_value());
        auto point = rhi::queue<Queue>{ *hardware_interface }.submit(std::span{ &*list, 1 });
        REQUIRE(point.has_value());
        return *point;
    }

    template rhi::synchronization_point submit_marker<rhi::queue_type::graphics>(test_rhi&);
    template rhi::synchronization_point submit_marker<rhi::queue_type::compute>(test_rhi&);
    template rhi::synchronization_point submit_marker<rhi::queue_type::copy>(test_rhi&);

    graphics::frame_scheduler_configuration test_frame_scheduler_configuration(const std::uint32_t frames_in_flight)
    {
        return { .frames_in_flight = frames_in_flight,
                 .upload_memory_per_frame = test_frame_memory_size,
                 .descriptors_per_frame = test_frame_descriptor_count };
    }

    std::unique_ptr<graphics::frame_scheduler>
        make_test_frame_scheduler(test_rhi& hardware_interface, const std::uint32_t frames_in_flight)
    {
        auto scheduler = graphics::frame_scheduler::create(
            rhi::synchronization{ *hardware_interface },
            rhi::transient_objects{ *hardware_interface },
            test_frame_scheduler_configuration(frames_in_flight));
        REQUIRE(scheduler.has_value());
        return std::move(*scheduler);
    }

    std::unique_ptr<graphics::gpu_profiler> make_test_gpu_profiler(
        test_rhi& hardware_interface, const std::uint32_t frames_in_flight, const std::uint32_t timestamps_per_frame)
    {
        auto profiler = graphics::gpu_profiler::create(
            rhi::transient_objects{ *hardware_interface },
            rhi::queue<rhi::queue_type::graphics>{ *hardware_interface },
            rhi::queue<rhi::queue_type::compute>{ *hardware_interface },
            rhi::queue<rhi::queue_type::copy>{ *hardware_interface },
            frames_in_flight,
            { .timestamps_per_frame = timestamps_per_frame });
        REQUIRE(profiler.has_value());
        return std::move(*profiler);
    }

    std::unique_ptr<graphics::upload_queue>
        make_test_upload_queue(test_rhi& hardware_interface, const std::size_t ring_size)
    {
        auto queue = graphics::upload_queue::create(
            rhi::resources{ *hardware_interface },
            rhi::command_lists{ *hardware_interface },
            rhi::queue<rhi::queue_type::copy>{ *hardware_interface },
            rhi::synchronization{ *hardware_interface },
            { .staging_ring_size = ring_size });
        REQUIRE(queue.has_value());
        return std::move(*queue);
    }

    std::unique_ptr<graphics::readback_queue> make_test_readback_queue(test_rhi& hardware_interface)
    {
        auto queue = graphics::readback_queue::create(
            rhi::resources{ *hardware_interface },
            rhi::command_lists{ *hardware_interface },
            rhi::queue<rhi::queue_type::graphics>{ *hardware_interface },
            rhi::queue<rhi::queue_type::compute>{ *hardware_interface },
            rhi::queue<rhi::queue_type::copy>{ *hardware_interface },
            rhi::synchronization{ *hardware_interface });
        REQUIRE(queue.has_value());
        return std::move(*queue);
    }

    graphics::frame_token begin_frame_inline(graphics::frame_scheduler& scheduler)
    {
        started_frame frame{ scheduler.begin_frame() };
        frame.start();
        REQUIRE(frame.outcome.values == 1);
        return std::move(*frame.outcome.token);
    }
} // namespace avis::tests
