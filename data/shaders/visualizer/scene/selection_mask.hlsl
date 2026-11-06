// @avis-shader entry=pixel_main stage=ps permutations=ALPHA_MASK,LINES,LINES_ALPHA_MASK,POINTS,POINTS_ALPHA_MASK
//
// The selection mask (docs/design/11-renderer.md §10.5, docs/tasks/11-renderer.md §11.17): 1 into selection_mask (R8)
// where a selected instance is visible. The selection_mask passes walk only the runs that hold a selected instance;
// a run can also hold unselected instances of the same mesh and material, which are discarded here. The depth test
// is the overlay's (pipeline_variant_registry.cpp), so hidden parts aren't in the mask. Permutation parts, in this
// order:
//   LINES, POINTS  the topology; none for triangles
//   ALPHA_MASK     alpha_mode mask: the cut-away pixels aren't selected

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

float pixel_main(scene_varyings input) : SV_Target0
{
    const instance_data instance = load_instance(scene_pass, input.instance);
    if ((instance.flags & instance_flag_selected) == 0)
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
#if AVIS_LINES
    // The pixels the line covers at least half (11 §9.2)
    if (line_coverage(input.quad.y, material.line_width * view.device_pixel_ratio) < 0.5)
    {
        discard;
    }
#endif
#if AVIS_ALPHA_MASK
    if (material_alpha_clipped(material, material_base_color(material, input.uv, input.color).a))
    {
        discard;
    }
#endif
    return 1.0;
}
