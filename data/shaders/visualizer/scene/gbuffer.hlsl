// @avis-shader entry=pixel_main stage=ps permutations=PICKING,ALPHA_MASK,ALPHA_MASK_PICKING,POINTS,POINTS_PICKING,POINTS_ALPHA_MASK,POINTS_ALPHA_MASK_PICKING
//
// The G-buffer pixel shader (docs/design/11-renderer.md §5, docs/tasks/11-renderer.md §11.7): albedo, normal,
// material, emissive and velocity, plus the picking id on click frames (11 §5.1). Permutation parts, in this order:
//   POINTS      point topology
//   ALPHA_MASK  alpha_mode mask
//   PICKING     also writes picking_id (SV_Target5)
// G-buffer: 11.10; picking: 11.19.
//
// The body is a placeholder: the pass's task fills it in.

#include "bindless.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"

// ---- permutation features ----
#if defined(POINTS) || defined(POINTS_PICKING) || defined(POINTS_ALPHA_MASK) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_POINTS 1
#else
#    define AVIS_POINTS 0
#endif
#if defined(ALPHA_MASK) || defined(ALPHA_MASK_PICKING) || defined(POINTS_ALPHA_MASK) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_ALPHA_MASK 1
#else
#    define AVIS_ALPHA_MASK 0
#endif
#if defined(PICKING) || defined(ALPHA_MASK_PICKING) || defined(POINTS_PICKING) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_PICKING 1
#else
#    define AVIS_PICKING 0
#endif

struct gbuffer_output
{
    float4 albedo : SV_Target0; // RGBA8_UNORM_SRGB
    float2 normal : SV_Target1; // RG16_UNORM, octahedral
    float4 material : SV_Target2; // RGBA8_UNORM
    float3 emissive : SV_Target3; // R11G11B10_FLOAT
    float2 velocity : SV_Target4; // RG16_FLOAT
#if AVIS_PICKING
    uint2 picking : SV_Target5; // R32G32_UINT, the object id
#endif
};

gbuffer_output pixel_main(scene_varyings input)
{
#if AVIS_POINTS
    if (dot(input.quad, input.quad) > 1.0)
    {
        discard;
    }
#endif
#if AVIS_ALPHA_MASK
    if (input.color.a < 0.5)
    {
        discard;
    }
#endif
    gbuffer_output output = (gbuffer_output)0;
    output.albedo = input.color;
    output.normal = input.normal.xy * 0.5 + 0.5;
    output.velocity = input.current_clip.xy / input.current_clip.w - input.previous_clip.xy / input.previous_clip.w;
#if AVIS_PICKING
    output.picking = uint2(input.instance, 0);
#endif
    return output;
}
