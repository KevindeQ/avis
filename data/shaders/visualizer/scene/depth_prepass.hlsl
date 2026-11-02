// @avis-shader entry=pixel_main stage=ps permutations=ALPHA_MASK,POINTS,POINTS_ALPHA_MASK
//
// The pixel shader of the depth-only passes, the depth prepass and the shadow cascades (docs/design/11-renderer.md
// §2.3, §7, docs/tasks/11-renderer.md §11.7). Opaque triangles and lines have none: masked materials test their
// alpha, and points discard outside their circle (11 §9.1). Permutation parts, in this order:
//   POINTS      point topology
//   ALPHA_MASK  alpha_mode mask
// Depth prepass: 11.10; shadows: 11.12.
//
// The body is a placeholder: the pass's task fills it in.

#include "bindless.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"

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

void pixel_main(scene_varyings input)
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
}
