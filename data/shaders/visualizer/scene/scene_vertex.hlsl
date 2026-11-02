// @avis-shader entry=vertex_main stage=vs permutations=COLOR,UV,UV_COLOR,LINES,LINES_COLOR,LINES_UV,LINES_UV_COLOR,POINTS,POINTS_COLOR,POINTS_UV,POINTS_UV_COLOR,SCREEN_ALIGNED,SCREEN_ALIGNED_COLOR,SCREEN_ALIGNED_UV,SCREEN_ALIGNED_UV_COLOR,SCREEN_ALIGNED_LINES,SCREEN_ALIGNED_LINES_COLOR,SCREEN_ALIGNED_LINES_UV,SCREEN_ALIGNED_LINES_UV_COLOR,SCREEN_ALIGNED_POINTS,SCREEN_ALIGNED_POINTS_COLOR,SCREEN_ALIGNED_POINTS_UV,SCREEN_ALIGNED_POINTS_UV_COLOR
//
// The vertex shader of every scene pass (docs/design/11-renderer.md §3.2, §4.4, docs/tasks/11-renderer.md §11.7): one
// permutation per vertex format of a pipeline set. The permutation name is built from these parts, in this order:
//   SCREEN_ALIGNED  the instance is in pixels and faces the camera (overlay layers, 11 §10.5)
//   LINES, POINTS   the topology; none for triangles. Lines and points are expanded into quads (11 §9)
//   UV, COLOR       the mesh has uv0, color0
// pipeline_variant_registry builds the same names (rendering/pipeline_variant_registry.cpp). Normals and tangents are
// read by the mesh's attribute mask at run time, so they don't multiply the permutations.
//
// The shadow passes use it too, with each cascade's view constants (11.12).
//
// The body is a placeholder: the depth prepass and G-buffer task (11.10) pulls the vertices from the geometry arena.

#include "bindless.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"
#include "vertex_pulling.hlsli"

// ---- permutation features ----
#if defined(SCREEN_ALIGNED) || defined(SCREEN_ALIGNED_COLOR) || defined(SCREEN_ALIGNED_UV) || defined(SCREEN_ALIGNED_UV_COLOR) || defined(SCREEN_ALIGNED_LINES) || defined(SCREEN_ALIGNED_LINES_COLOR) || defined(SCREEN_ALIGNED_LINES_UV) || defined(SCREEN_ALIGNED_LINES_UV_COLOR) || defined(SCREEN_ALIGNED_POINTS) || defined(SCREEN_ALIGNED_POINTS_COLOR) || defined(SCREEN_ALIGNED_POINTS_UV) || defined(SCREEN_ALIGNED_POINTS_UV_COLOR)
#    define AVIS_SCREEN_ALIGNED 1
#else
#    define AVIS_SCREEN_ALIGNED 0
#endif
#if defined(LINES) || defined(LINES_COLOR) || defined(LINES_UV) || defined(LINES_UV_COLOR) || defined(SCREEN_ALIGNED_LINES) || defined(SCREEN_ALIGNED_LINES_COLOR) || defined(SCREEN_ALIGNED_LINES_UV) || defined(SCREEN_ALIGNED_LINES_UV_COLOR)
#    define AVIS_LINES 1
#else
#    define AVIS_LINES 0
#endif
#if defined(POINTS) || defined(POINTS_COLOR) || defined(POINTS_UV) || defined(POINTS_UV_COLOR) || defined(SCREEN_ALIGNED_POINTS) || defined(SCREEN_ALIGNED_POINTS_COLOR) || defined(SCREEN_ALIGNED_POINTS_UV) || defined(SCREEN_ALIGNED_POINTS_UV_COLOR)
#    define AVIS_POINTS 1
#else
#    define AVIS_POINTS 0
#endif
#if defined(UV) || defined(UV_COLOR) || defined(LINES_UV) || defined(LINES_UV_COLOR) || defined(POINTS_UV) || defined(POINTS_UV_COLOR) || defined(SCREEN_ALIGNED_UV) || defined(SCREEN_ALIGNED_UV_COLOR) || defined(SCREEN_ALIGNED_LINES_UV) || defined(SCREEN_ALIGNED_LINES_UV_COLOR) || defined(SCREEN_ALIGNED_POINTS_UV) || defined(SCREEN_ALIGNED_POINTS_UV_COLOR)
#    define AVIS_HAS_UV0 1
#else
#    define AVIS_HAS_UV0 0
#endif
#if defined(COLOR) || defined(UV_COLOR) || defined(LINES_COLOR) || defined(LINES_UV_COLOR) || defined(POINTS_COLOR) || defined(POINTS_UV_COLOR) || defined(SCREEN_ALIGNED_COLOR) || defined(SCREEN_ALIGNED_UV_COLOR) || defined(SCREEN_ALIGNED_LINES_COLOR) || defined(SCREEN_ALIGNED_LINES_UV_COLOR) || defined(SCREEN_ALIGNED_POINTS_COLOR) || defined(SCREEN_ALIGNED_POINTS_UV_COLOR)
#    define AVIS_HAS_COLOR0 1
#else
#    define AVIS_HAS_COLOR0 0
#endif

AVIS_DRAW_CONSTANTS(scene_draw_constants, draw);
AVIS_VIEW_CONSTANTS(view_constants, view);

scene_varyings vertex_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    scene_varyings output = (scene_varyings)0;
    const float4 world_position = float4(0.0, 0.0, 0.0, 1.0);
    output.position = mul(view.view_projection, world_position);
    output.world_position = world_position.xyz;
    output.normal = float3(0.0, 0.0, 1.0);
    output.tangent = float4(1.0, 0.0, 0.0, 1.0);
    output.color = float4(1.0, 1.0, 1.0, 1.0);
    output.current_clip = mul(view.view_projection_unjittered, world_position);
    output.previous_clip = mul(view.view_projection_previous, world_position);
#if AVIS_SCREEN_ALIGNED
    // A pixel offset at the anchor's depth
    output.position.xy += float2(vertex_id & 1, vertex_id >> 1) * 2.0 * view.inverse_size * view.device_pixel_ratio *
                          output.position.w;
#endif
#if AVIS_LINES || AVIS_POINTS
    output.quad = float2((vertex_id & 1) ? 1.0 : -1.0, (vertex_id & 2) ? 1.0 : -1.0);
#endif
#if AVIS_HAS_UV0
    output.uv = float2(0.0, 0.0);
#endif
#if AVIS_HAS_COLOR0
    output.color = float4(1.0, 1.0, 1.0, 1.0);
#endif
    output.instance = draw.instance_index_offset + instance_id;
    output.material = draw.material;
    return output;
}
