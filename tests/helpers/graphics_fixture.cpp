#include "graphics_fixture.h"

#include "avis/rhi/command_list.h"

#include <catch2/catch_test_macros.hpp>

#include <span>

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
} // namespace avis::tests
