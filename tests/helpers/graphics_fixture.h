#ifndef AVIS_TESTS_HELPERS_GRAPHICS_FIXTURE_H
#define AVIS_TESTS_HELPERS_GRAPHICS_FIXTURE_H

// Helpers for the graphics-layer tests (docs/tasks/07-graphics-frames-and-transfers.md §7.1), on top of test_rhi:
// test-sized configurations for the graphics objects and an empty submission that yields a synchronization point.

#include "rhi_fixture.h"

#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>

namespace avis::tests
{
    // Test sizes: small enough to exhaust on purpose, large enough for the tests' normal use
    inline constexpr std::size_t test_staging_ring_size = std::size_t{ 1 } << 20; // 1 MiB
    inline constexpr std::size_t test_frame_memory_size = std::size_t{ 64 } << 10; // 64 KiB per frame context
    inline constexpr std::uint32_t test_frame_descriptor_count = 64; // per frame context

    // Acquires a list on `Queue`, records nothing, submits it and returns its point. Explicitly instantiated for the
    // three queue types in graphics_fixture.cpp.
    template<rhi::queue_type Queue>
    rhi::synchronization_point submit_marker(test_rhi& hardware_interface);
} // namespace avis::tests

#endif
