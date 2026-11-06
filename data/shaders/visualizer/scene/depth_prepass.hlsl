// @avis-shader entry=pixel_main stage=ps permutations=ALPHA_MASK,POINTS,POINTS_ALPHA_MASK
//
// The pixel shader of the depth-only passes, the depth prepass and the shadow cascades (docs/design/11-renderer.md
// §2.3, §7, §9.1, docs/tasks/11-renderer.md §11.7, §11.10). Opaque triangles and lines have none: masked materials test
// their alpha, and points discard outside their circle (11 §9.1). Permutation parts, in this order:
//   POINTS      point topology
//   ALPHA_MASK  alpha_mode mask: the base colour's alpha (factor, texture and, with use_vertex_color, the vertex colour)
//               below alpha_cutoff is cut away
// The depth is the rasterised one, so the G-buffer's EQUAL test sees the same surface.

#include "bindless.hlsli"
#include "scene_material.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"
#include "vertex_pulling.hlsli"

// ---- permutation features ----
#if defined(POINTS) || defined(POINTS_ALPHA_MASK)
#    define AVIS_POINTS 1
#else
#    define AVIS_POINTS 0
#endif
#if defined(ALPHA_MASK) || defined(POINTS_ALPHA_MASK)
#    define AVIS_ALPHA_MASK 1
#else
#    define AVIS_ALPHA_MASK 0
#endif

AVIS_PASS_CONSTANTS(scene_pass_constants, scene_pass);

void pixel_main(scene_varyings input)
{
#if AVIS_POINTS
    if (point_outside_circle(input.quad))
    {
        discard;
    }
#endif
#if AVIS_ALPHA_MASK
    const material_data material = pull_material(scene_pass, input.material);
    if (material_alpha_clipped(material, material_base_color(material, input.uv, input.color).a))
    {
        discard;
    }
#endif
}
