#ifndef AVIS_RHI_D3D12_RESOURCE_RECORDS_H
#define AVIS_RHI_D3D12_RESOURCE_RECORDS_H

// What the slot maps behind resource handles hold (docs/design/09-rhi.md §21.3). Records release what they own when
// destroyed, so the deferred release frees a record by destroying it (D-91). Every record is movable: no mutex or
// atomic directly, and no string_view into itself.

#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/descriptor_heaps.h"
#include "avis/rhi/d3d12/unique_handle.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/root_signature.h"
#include "avis/rhi/types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace avis::rhi::d3d12
{
    // The memory of a heap (§8.5), shared by the heap record and every resource placed in it. Freed when the last
    // owner is released, so the order in which deferred release frees a heap and its placed resources doesn't matter.
    struct placed_heap
    {
        com_ptr<D3D12MA::Allocation> allocation;
        // Placed resources whose handle is alive; destroy(heap) asserts 0
        std::atomic<std::uint32_t> live_resources{ 0 };
    };

    struct texture_record
    {
        std::shared_ptr<placed_heap> heap; // set for placed resources; declared first, so released last
        com_ptr<ID3D12Resource> resource;
        com_ptr<D3D12MA::Allocation> allocation; // null for placed resources
        texture_description description; // debug_name cleared at creation; see name
        std::string name;
        bool placed = false; // no persistent descriptors (§8.4)
        descriptor_lease srv; // persistent region; empty (index 0) without shader_resource usage
        std::vector<descriptor_lease> uav; // per mip; the staging copy for clears is at the same index (§9.1)
        // Lazy RTV/DSV (§22.2.3); on the heap because the cache holds a mutex. Null without render_target or
        // depth_stencil usage.
        std::unique_ptr<view_cache> views;
        unique_handle shared_handle; // export_shared_handle (6.11); closes on release (§21.5)
    };

    struct buffer_record
    {
        std::shared_ptr<placed_heap> heap; // as in texture_record
        com_ptr<ID3D12Resource> resource;
        com_ptr<D3D12MA::Allocation> allocation;
        buffer_description description; // debug_name cleared at creation, as for textures
        std::string name;
        bool placed = false;
        gpu_address address;
        std::span<std::byte> mapped; // upload / readback memory, mapped for the record's lifetime
        descriptor_lease srv;
        descriptor_lease uav;
    };

    struct sampler_record
    {
        descriptor_lease index; // sampler heap
    };

    struct heap_record
    {
        std::shared_ptr<placed_heap> heap;
        heap_description description; // debug_name cleared
        std::string name;
    };

    // Filled by the pipeline factory (§22.3–22.4). Root signatures and pipelines live as long as the RHI (D-87), so a
    // pipeline record's pointer to its signature's record never dangles (slot map records never move, §21.2).
    struct root_signature_record
    {
        com_ptr<ID3D12RootSignature> native;
        std::vector<root_parameter> parameters; // copy of the description's, for the root call checks (§24.3)
        std::string name;
    };

    struct graphics_pipeline_record
    {
        com_ptr<ID3D12PipelineState> state;
        const root_signature_record* root_signature = nullptr;
        std::string name;
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; // IASetPrimitiveTopology in set_pipeline
    };

    struct compute_pipeline_record
    {
        com_ptr<ID3D12PipelineState> state;
        const root_signature_record* root_signature = nullptr;
        std::string name;
    };

    static_assert(std::is_nothrow_move_constructible_v<texture_record>);
    static_assert(std::is_nothrow_move_constructible_v<buffer_record>);
    static_assert(std::is_nothrow_move_constructible_v<sampler_record>);
    static_assert(std::is_nothrow_move_constructible_v<heap_record>);
} // namespace avis::rhi::d3d12

#endif
