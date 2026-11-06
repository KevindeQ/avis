// @avis-shader entry=pixel_main stage=ps permutations=ALPHA_MASK,LINES,LINES_ALPHA_MASK,POINTS,POINTS_ALPHA_MASK
//
// Picking (docs/design/11-renderer.md §5.1, docs/tasks/11-renderer.md §11.19): the object id of the pickable instance
// on top into picking_id (R32G32_UINT, low word in R, high word in G; 0 is nothing). On a click frame, the picking
// passes walk the camera's world and overlay groups in key order, then overlay_on_top, so what is drawn later wins:
// overlays over transparents over opaque surfaces, transparents back to front. The depth test is the overlay's
// (pipeline_variant_registry.cpp) against the scene depth, so hidden surfaces aren't picked. Permutation parts, in this
// order:
//   LINES, POINTS  the topology; none for triangles
//   ALPHA_MASK     alpha_mode mask: the cut-away pixels aren't picked
// Discarded: instances without instance_flag_pickable (so what is behind them shows through), and pixels whose
// coverage is below picking_minimum_coverage (a blend material's alpha, times a line's edge coverage).

#include "bindless.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"
#include "scene_material.hlsli"
#include "vertex_pulling.hlsli"

AVIS_PASS_CONSTANTS(scene_pass_constants, scene_pass);
AVIS_VIEW_CONSTANTS(view_constants, view);

// ---- permutation features ----
#if defined(LINES) || defined(LINES_ALPHA_MASK)
#    define AVIS_LINES 1
#else
#    define AVIS_LINES 0
#endif
#if defined(POINTS) || defined(POINTS_ALPHA_MASK)
#    define AVIS_POINTS 1
#else
#    define AVIS_POINTS 0
#endif
#if defined(ALPHA_MASK) || defined(LINES_ALPHA_MASK) || defined(POINTS_ALPHA_MASK)
#    define AVIS_ALPHA_MASK 1
#else
#    define AVIS_ALPHA_MASK 0
#endif

uint2 pixel_main(scene_varyings input) : SV_Target0
{
    const instance_data instance = load_instance(scene_pass, input.instance);
    if ((instance.flags & instance_flag_pickable) == 0)
    {
        discard;
    }
#if AVIS_POINTS
    if (point_outside_circle(input.quad))
    {
        discard;
    }
#endif
    const material_data material = pull_material(scene_pass, input.material);
    const float alpha = material_base_color(material, input.uv, input.color).a;
#if AVIS_ALPHA_MASK
    if (material_alpha_clipped(material, alpha))
    {
        discard;
    }
#endif
    float coverage = material.alpha_mode == alpha_mode_blend ? alpha : 1.0;
#if AVIS_LINES
    coverage *= line_coverage(input.quad.y, material.line_width * view.device_pixel_ratio);
#endif
    if (coverage < picking_minimum_coverage)
    {
        discard;
    }
    return uint2(instance.object_id_low, instance.object_id_high);
}
