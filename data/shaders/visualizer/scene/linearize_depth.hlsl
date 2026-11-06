// @avis-shader entry=main stage=cs
//
// Linearize depth (docs/design/11-renderer.md §2.3 pass 2, docs/tasks/11-renderer.md §11.10): the reverse-Z depth of
// the depth prepass to view-space linear depth (metres in front of the camera, -z_view), into linear_depth (R32F) and
// mip 0 of the min/max chain depth_mips (RG32F, min in R, max in G; the task's Open point "depth_mips layout"). A
// pixel nothing was drawn into (depth 0, the far plane or infinity) is +infinity, so it never lowers a minimum and
// marks empty tiles. depth_mips.hlsl reduces the rest of the chain.

#include "bindless.hlsli"
#include "shared_types.hlsli"

struct linearize_constants
{
    uint depth; // SRV of the depth buffer (D32F, read as R32F)
    uint linear_depth; // UAV, R32F
    uint depth_mips; // UAV of depth_mips mip 0, RG32F
    uint padding0;
    uint2 size; // pixels
};

AVIS_DRAW_CONSTANTS(linearize_constants, constants);
AVIS_VIEW_CONSTANTS(view_constants, view);

static const float infinity = asfloat(0x7F800000u);

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float> depth = ResourceDescriptorHeap[constants.depth];
    RWTexture2D<float> linear_depth = ResourceDescriptorHeap[constants.linear_depth];
    RWTexture2D<float2> depth_mips = ResourceDescriptorHeap[constants.depth_mips];

    const float device_depth = depth.Load(int3(thread.xy, 0));
    float value = infinity;
    if (device_depth > 0.0)
    {
        // The view-space position of the pixel centre at that depth, through the jittered projection it was drawn with
        const float2 ndc = ((float2(thread.xy) + 0.5) * view.inverse_size * 2.0 - 1.0) * float2(1.0, -1.0);
        const float4 position = mul(view.projection_inverse, float4(ndc, device_depth, 1.0));
        value = -position.z / position.w;
    }
    linear_depth[thread.xy] = value;
    depth_mips[thread.xy] = float2(value, value);
}
