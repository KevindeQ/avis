// @avis-shader entry=pixel_main stage=ps permutations=PICKING,UNLIT,UNLIT_PICKING,LINES,LINES_PICKING,LINES_UNLIT,LINES_UNLIT_PICKING,POINTS,POINTS_PICKING,POINTS_UNLIT,POINTS_UNLIT_PICKING
//
// The forward pixel shader of translucent world geometry (docs/design/11-renderer.md §10.1,
// docs/tasks/11-renderer.md §11.7): premultiplied colour into hdr_color and the TAA reactive mask, plus the
// picking id on click frames. Permutation parts, in this order:
//   LINES, POINTS  the topology (line coverage falloff, point circles); none for triangles
//   UNLIT          shading_model unlit
//   PICKING        also writes picking_id (SV_Target2)
// Forward: 11.14; picking: 11.19.
//
// The body is a placeholder: the pass's task fills it in.

#include "bindless.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"

// ---- permutation features ----
#if defined(LINES) || defined(LINES_PICKING) || defined(LINES_UNLIT) || defined(LINES_UNLIT_PICKING)
#    define AVIS_LINES 1
#else
#    define AVIS_LINES 0
#endif
#if defined(POINTS) || defined(POINTS_PICKING) || defined(POINTS_UNLIT) || defined(POINTS_UNLIT_PICKING)
#    define AVIS_POINTS 1
#else
#    define AVIS_POINTS 0
#endif
#if defined(UNLIT) || defined(UNLIT_PICKING) || defined(LINES_UNLIT) || defined(LINES_UNLIT_PICKING) || defined(POINTS_UNLIT) || defined(POINTS_UNLIT_PICKING)
#    define AVIS_UNLIT 1
#else
#    define AVIS_UNLIT 0
#endif
#if defined(PICKING) || defined(UNLIT_PICKING) || defined(LINES_PICKING) || defined(LINES_UNLIT_PICKING) || defined(POINTS_PICKING) || defined(POINTS_UNLIT_PICKING)
#    define AVIS_PICKING 1
#else
#    define AVIS_PICKING 0
#endif

struct forward_output
{
    float4 color : SV_Target0; // RGBA16_FLOAT, premultiplied
    float reactive : SV_Target1; // R8_UNORM
#if AVIS_PICKING
    uint2 picking : SV_Target2; // R32G32_UINT
#endif
};

forward_output pixel_main(scene_varyings input)
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
    float4 color = input.color;
#if !AVIS_UNLIT
    color.rgb *= saturate(input.normal.z);
#endif
    forward_output output = (forward_output)0;
    output.color = float4(color.rgb * color.a * coverage, color.a * coverage);
    output.reactive = color.a * coverage;
#if AVIS_PICKING
    output.picking = uint2(input.instance, 0);
#endif
    return output;
}
