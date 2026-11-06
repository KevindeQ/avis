// @avis-shader entry=accumulate_main stage=cs entry=resolve_main stage=cs
//
// Custom screenshots (docs/design/11-renderer.md §12.1, docs/tasks/11-renderer.md §11.21), one pixel per thread in
// 8 × 8 groups. The screenshot view renders screenshot_subframes jittered sub-frames without TAA:
// - accumulate_main adds each sub-frame's hdr_color into the RGBA32F accumulation texture, divided by the exposure it
//   was pre-exposed with, so a sub-frame whose exposure differs still adds the same radiance. The first sub-frame
//   replaces what the texture held.
// - resolve_main, after the last sub-frame's accumulation, writes the average pre-exposed again with that exposure:
//   what the tonemap reads in place of taa_output.

#include "bindless.hlsli"
#include "exposure.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(screenshot_constants, constants);

[numthreads(8, 8, 1)] void accumulate_main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float4> color = ResourceDescriptorHeap[constants.color];
    RWTexture2D<float4> accumulation = ResourceDescriptorHeap[constants.accumulation];

    const float4 radiance = max(color.Load(int3(thread.xy, 0)), 0.0) / load_current_exposure(constants.exposure_buffer);
    accumulation[thread.xy] = constants.first != 0 ? radiance : accumulation[thread.xy] + radiance;
}

[numthreads(8, 8, 1)] void resolve_main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float4> accumulation = ResourceDescriptorHeap[constants.accumulation];
    RWTexture2D<float4> resolved = ResourceDescriptorHeap[constants.resolved];

    const float exposure = load_current_exposure(constants.exposure_buffer);
    resolved[thread.xy] = accumulation.Load(int3(thread.xy, 0)) * (constants.weight * exposure);
}
