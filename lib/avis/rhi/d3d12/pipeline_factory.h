#ifndef AVIS_RHI_D3D12_PIPELINE_FACTORY_H
#define AVIS_RHI_D3D12_PIPELINE_FACTORY_H

// Root signatures, command signatures, pipeline records and PSO creation (docs/design/09-rhi.md §12, §22.4). What the
// public pipelines view forwards to. No cache and no lock of its own: PSO creation is thread-safe, and the slot maps
// guard their inserts.

#include "avis/error.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/resource_records.h"
#include "avis/rhi/d3d12/root_signature.h"
#include "avis/rhi/d3d12/slot_map.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/root_signature.h"

#include <memory>

namespace avis::rhi::d3d12
{
    class device;

    class pipeline_factory
    {
    public:
        static result<std::unique_ptr<pipeline_factory>> create(device& device);

        pipeline_factory(const pipeline_factory&) = delete;
        pipeline_factory& operator=(const pipeline_factory&) = delete;

        result<root_signature_handle> create_root_signature(const root_signature_description& description);
        result<graphics_pipeline_handle> create_pipeline(const graphics_pipeline_description& description);
        result<compute_pipeline_handle> create_pipeline(const compute_pipeline_description& description);

        const root_signature_record* find(root_signature_handle signature) const noexcept;
        const graphics_pipeline_record* find(graphics_pipeline_handle pipeline) const noexcept;
        const compute_pipeline_record* find(compute_pipeline_handle pipeline) const noexcept;
        d3d12::command_signatures& command_signatures() noexcept;

        // Pipelines created so far: they're never destroyed (D-87), so a slot map's high-water mark is its count. For
        // white-box tests of callers that must not create one (the graphics layer's pipeline cache).
        std::uint32_t graphics_pipeline_count() const noexcept
        {
            return graphics_pipelines_.slots_used();
        }

        std::uint32_t compute_pipeline_count() const noexcept
        {
            return compute_pipelines_.slots_used();
        }

    private:
        pipeline_factory(device& device, d3d12::command_signatures signatures) noexcept;

        device& device_;
        d3d12::command_signatures command_signatures_;
        slot_map<root_signature_record> root_signatures_;
        slot_map<graphics_pipeline_record> graphics_pipelines_;
        slot_map<compute_pipeline_record> compute_pipelines_;
    };

    // A pipeline state stream built without d3dx12.h: root signature, shaders, rasterizer, DEPTH_STENCIL1, blend,
    // render target and depth formats, topology type, sample description {1, 0}. Failures are pipeline_creation_failed
    // with the debug name and HRESULT.
    result<com_ptr<ID3D12PipelineState>> create_pipeline_state(
        device& device, const root_signature_record& root, const graphics_pipeline_description& description);
    result<com_ptr<ID3D12PipelineState>> create_pipeline_state(
        device& device, const root_signature_record& root, const compute_pipeline_description& description);
} // namespace avis::rhi::d3d12

#endif
