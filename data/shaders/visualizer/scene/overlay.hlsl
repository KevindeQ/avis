// @avis-shader entry=pixel_main stage=ps permutations=PICKING,ALPHA_MASK,ALPHA_MASK_PICKING,LINES,LINES_PICKING,LINES_ALPHA_MASK,LINES_ALPHA_MASK_PICKING,POINTS,POINTS_PICKING,POINTS_ALPHA_MASK,POINTS_ALPHA_MASK_PICKING
//
// The overlay pixel shader (docs/design/11-renderer.md §10.5, docs/tasks/11-renderer.md §11.7): unlit colour into
// ldr_color after tonemapping, premultiplied, plus the picking id on click frames. Permutation parts, in this
// order:
//   LINES, POINTS  the topology; none for triangles
//   ALPHA_MASK     alpha_mode mask
//   PICKING        also writes picking_id (SV_Target1)
// Overlay: 11.17; picking: 11.19.
//
// The body is a placeholder: the pass's task fills it in.

#include "bindless.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"

// ---- permutation features ----
#if defined(LINES) || defined(LINES_PICKING) || defined(LINES_ALPHA_MASK) || defined(LINES_ALPHA_MASK_PICKING)
#    define AVIS_LINES 1
#else
#    define AVIS_LINES 0
#endif
#if defined(POINTS) || defined(POINTS_PICKING) || defined(POINTS_ALPHA_MASK) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_POINTS 1
#else
#    define AVIS_POINTS 0
#endif
#if defined(ALPHA_MASK) || defined(ALPHA_MASK_PICKING) || defined(LINES_ALPHA_MASK) || defined(LINES_ALPHA_MASK_PICKING) || defined(POINTS_ALPHA_MASK) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_ALPHA_MASK 1
#else
#    define AVIS_ALPHA_MASK 0
#endif
#if defined(PICKING) || defined(ALPHA_MASK_PICKING) || defined(LINES_PICKING) || defined(LINES_ALPHA_MASK_PICKING) || defined(POINTS_PICKING) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_PICKING 1
#else
#    define AVIS_PICKING 0
#endif

struct overlay_output
{
    float4 color : SV_Target0; // RGBA8_UNORM, sRGB-encoded
#if AVIS_PICKING
    uint2 picking : SV_Target1; // R32G32_UINT
#endif
};

overlay_output pixel_main(scene_varyings input)
{
    float coverage = 1.0;
#if AVIS_POINTS
    if (dot(input.quad, input.quad) > 1.0)
    {
        discard;
    }
#endif
#if AVIS_LINES
    coverage = saturate(1.0 - abs(input.quad.y));
#endif
#if AVIS_ALPHA_MASK
    if (input.color.a < 0.5)
    {
        discard;
    }
#endif
    overlay_output output = (overlay_output)0;
    // Premultiplied, like the forward pass: they share the blend states (pipeline_variant_registry.cpp)
    const float alpha = input.color.a * coverage;
    output.color = float4(input.color.rgb * alpha, alpha);
#if AVIS_PICKING
    output.picking = uint2(input.instance, 0);
#endif
    return output;
}
