// @avis-shader entry=pixel_main stage=ps permutations=PICKING,ALPHA_MASK,ALPHA_MASK_PICKING,LINES,LINES_PICKING,LINES_ALPHA_MASK,LINES_ALPHA_MASK_PICKING,POINTS,POINTS_PICKING,POINTS_ALPHA_MASK,POINTS_ALPHA_MASK_PICKING
//
// The overlay pixel shader (docs/design/11-renderer.md §3.4, §10.5, docs/tasks/11-renderer.md §11.7, §11.17): the
// material's unlit colour into ldr_color after the tonemap, so it is neither exposed nor tonemapped. ldr_color holds
// sRGB-encoded values, so the linear base colour is encoded with the exact transfer function, then premultiplied by
// its alpha (times a line's coverage) for the blend states it shares with the forward pass. Plus the picking id on
// click frames (11.19). Permutation parts, in this order:
//   LINES, POINTS  the topology; none for triangles
//   ALPHA_MASK     alpha_mode mask
//   PICKING        also writes picking_id (SV_Target1)

#include "bindless.hlsli"
#include "packing.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"
#include "scene_material.hlsli"
#include "vertex_pulling.hlsli"

AVIS_PASS_CONSTANTS(scene_pass_constants, scene_pass);
AVIS_VIEW_CONSTANTS(view_constants, view);

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
#if AVIS_POINTS
    if (point_outside_circle(input.quad))
    {
        discard;
    }
#endif
    const material_data material = pull_material(scene_pass, input.material);
    float coverage = 1.0;
#if AVIS_LINES
    // The analytic edge falloff of 11 §9.2: quad.y is the distance from the centre line in pixels
    coverage = line_coverage(input.quad.y, material.line_width * view.device_pixel_ratio);
#endif
    const float4 base_color = material_base_color(material, input.uv, input.color);
#if AVIS_ALPHA_MASK
    if (material_alpha_clipped(material, base_color.a))
    {
        discard;
    }
#endif
    // Opaque and mask materials cover the pixel; blended ones by their alpha
    const float alpha = (material.alpha_mode == alpha_mode_blend ? saturate(base_color.a) : 1.0) * coverage;
    overlay_output output = (overlay_output)0;
    // Premultiplied, like the forward pass: they share the blend states (pipeline_variant_registry.cpp)
    output.color = float4(linear_to_srgb(base_color.rgb) * alpha, alpha);
#if AVIS_PICKING
    const instance_data instance = load_instance(scene_pass, input.instance);
    output.picking = uint2(instance.object_id_low, instance.object_id_high);
#endif
    return output;
}
