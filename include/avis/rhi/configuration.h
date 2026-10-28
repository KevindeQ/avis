#ifndef AVIS_RHI_CONFIGURATION_H
#define AVIS_RHI_CONFIGURATION_H

// Configuration of render_hardware_interface::create, one struct per concern (docs/design/09-rhi.md §4.1, D-19).
// create fails with error_kind::invalid_argument for values outside their range.

#include "avis/rhi/types.h"

#include <cstdint>
#include <optional>

namespace avis::rhi
{
    struct adapter_configuration
    {
        std::optional<adapter_luid> adapter; // the Visualizer passes Qt's adapter LUID
        bool allow_software_adapter = false; // WARP (tests)
    };

    struct debug_configuration
    {
        bool debug_layer = false;
        bool gpu_based_validation = false; // implies debug_layer
        bool device_removed_extended_data = true;
        bool break_on_error = false;
        bool object_names = true; // SetName on every object (off saves the string work in release)
    };

    struct descriptor_heap_configuration
    {
        std::uint32_t persistent_descriptor_count = 900'000;
        std::uint32_t transient_descriptor_count = 100'000; // region that descriptor ranges are carved from (§6)
        std::uint32_t sampler_count = 2'048;
        std::uint32_t render_target_view_count = 4'096;
        std::uint32_t depth_stencil_view_count = 1'024;
    };

    struct render_hardware_interface_configuration
    {
        adapter_configuration adapter;
        debug_configuration debug;
        descriptor_heap_configuration descriptor_heaps;
    };
} // namespace avis::rhi

#endif
