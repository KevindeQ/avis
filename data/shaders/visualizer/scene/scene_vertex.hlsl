// @avis-shader entry=vertex_main stage=vs permutations=COLOR,UV,UV_COLOR,LINES,LINES_COLOR,LINES_UV,LINES_UV_COLOR,POINTS,POINTS_COLOR,POINTS_UV,POINTS_UV_COLOR,SCREEN_ALIGNED,SCREEN_ALIGNED_COLOR,SCREEN_ALIGNED_UV,SCREEN_ALIGNED_UV_COLOR,SCREEN_ALIGNED_LINES,SCREEN_ALIGNED_LINES_COLOR,SCREEN_ALIGNED_LINES_UV,SCREEN_ALIGNED_LINES_UV_COLOR,SCREEN_ALIGNED_POINTS,SCREEN_ALIGNED_POINTS_COLOR,SCREEN_ALIGNED_POINTS_UV,SCREEN_ALIGNED_POINTS_UV_COLOR
//
// The vertex shader of every scene pass (docs/design/11-renderer.md §3.2, §4.4, §9, docs/tasks/11-renderer.md §11.7,
// §11.10): one permutation per vertex format of a pipeline set. The permutation name is built from these parts, in this
// order:
//   SCREEN_ALIGNED  the instance is in pixels and faces the camera (overlay layers, 11 §10.5)
//   LINES, POINTS   the topology; none for triangles. Lines and points are expanded into quads (11 §9)
//   UV, COLOR       the mesh has uv0, color0
// pipeline_variant_registry builds the same names (rendering/pipeline_variant_registry.cpp). Normals and tangents are
// read by the mesh's stream offsets at run time, so they don't multiply the permutations.
//
// Every vertex is pulled from the geometry arena (vertex_pulling.hlsli) and its instance through instance_indices. The
// rasterised position uses the jittered view_projection; current_clip and previous_clip are unjittered, from this and
// the previous rendered frame's camera and world matrix, for velocity. The shadow passes use the same shader with each
// cascade's view constants (11.12).
//
// Points and lines draw six vertices per point or segment, without an index buffer: a line mesh's indices, if any, are
// read here. Their sizes come from the material: point_size metres (pixels when screen-aligned), at least one pixel,
// and line_width pixels. Both scale with the device pixel ratio where they are in pixels.

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
AVIS_PASS_CONSTANTS(scene_pass_constants, scene_pass);
AVIS_VIEW_CONSTANTS(view_constants, view);

// A vertex as the mesh stores it, in object space (or in pixels from the anchor, screen-aligned)
struct mesh_vertex
{
    float3 position;
    float3 normal;
    float4 tangent; // w = 0: no tangent
    float2 uv;
    float4 color; // linear
};

mesh_vertex pull_vertex(uint vertex)
{
    mesh_vertex result;
    result.position = pull_position(scene_pass, draw, vertex);
    result.normal = pull_normal(scene_pass, draw, vertex);
    result.tangent = pull_tangent(scene_pass, draw, vertex);
#if AVIS_HAS_UV0
    result.uv = pull_uv0(scene_pass, draw, vertex);
#else
    result.uv = float2(0.0, 0.0);
#endif
#if AVIS_HAS_COLOR0
    result.color = pull_color(scene_pass, draw, vertex);
#else
    result.color = float4(1.0, 1.0, 1.0, 1.0);
#endif
    return result;
}

mesh_vertex lerp_vertex(mesh_vertex a, mesh_vertex b, float t)
{
    mesh_vertex result;
    result.position = lerp(a.position, b.position, t);
    result.normal = lerp(a.normal, b.normal, t);
    result.tangent = float4(lerp(a.tangent.xyz, b.tangent.xyz, t), a.tangent.w);
    result.uv = lerp(a.uv, b.uv, t);
    result.color = lerp(a.color, b.color, t);
    return result;
}

// A position in the three clip spaces the passes need, and in world space
struct projected
{
    float4 clip; // jittered: what is rasterised
    float4 current; // unjittered, for velocity
    float4 previous; // unjittered, of the previous rendered frame's camera and world matrix
    float3 world;
};

projected project(instance_data instance, float3 position)
{
    projected result;
#if AVIS_SCREEN_ALIGNED
    // The anchor is the origin of the world matrix; the position is an offset in pixels at its depth, scaled by the
    // device pixel ratio (11 §10.5). The mesh keeps its size on screen and faces the camera.
    const float4 world = mul(instance.world, float4(0.0, 0.0, 0.0, 1.0));
    const float4 world_previous = mul(instance.world_previous, float4(0.0, 0.0, 0.0, 1.0));
    result.clip = mul(view.view_projection, world);
    result.current = mul(view.view_projection_unjittered, world);
    result.previous = mul(view.view_projection_previous, world_previous);
    const float2 offset = position.xy * 2.0 * view.inverse_size * view.device_pixel_ratio;
    result.clip.xy += offset * result.clip.w;
    result.current.xy += offset * result.current.w;
    result.previous.xy += offset * result.previous.w;
    result.world = world.xyz;
#else
    const float4 world = mul(instance.world, float4(position, 1.0));
    const float4 world_previous = mul(instance.world_previous, float4(position, 1.0));
    result.clip = mul(view.view_projection, world);
    result.current = mul(view.view_projection_unjittered, world);
    result.previous = mul(view.view_projection_previous, world_previous);
    result.world = world.xyz;
#endif
    return result;
}

projected lerp_projected(projected a, projected b, float t)
{
    projected result;
    result.clip = lerp(a.clip, b.clip, t);
    result.current = lerp(a.current, b.current, t);
    result.previous = lerp(a.previous, b.previous, t);
    result.world = lerp(a.world, b.world, t);
    return result;
}

// Moves all three clip positions by `offset` in NDC units, so the offset stays constant in pixels
void offset_projected(inout projected value, float2 offset)
{
    value.clip.xy += offset * value.clip.w;
    value.current.xy += offset * value.current.w;
    value.previous.xy += offset * value.previous.w;
}

// A vertex the rasteriser drops: outside the clip volume (reverse-Z keeps 0 <= z <= w)
static const float4 culled_position = float4(0.0, 0.0, 2.0, 1.0);

// The six corners of a quad as two counter-clockwise triangles (front-facing): x along, y across, both in {-1, 1}
static const float2 quad_corners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0)
};

scene_varyings make_varyings(uint instance_index, instance_data instance, projected position, mesh_vertex vertex)
{
    scene_varyings output = (scene_varyings)0;
    output.position = position.clip;
    output.world_position = position.world;
    output.current_clip = position.current;
    output.previous_clip = position.previous;
    output.uv = vertex.uv;
    output.color = vertex.color;
    output.normal = normalize(mul((float3x3)instance.normal_from_object, vertex.normal));
    output.tangent = float4(0.0, 0.0, 0.0, 0.0);
    if (vertex.tangent.w != 0.0)
    {
        output.tangent = float4(normalize(mul((float3x3)instance.world, vertex.tangent.xyz)), vertex.tangent.w);
    }
    output.instance = instance_index;
    output.material = draw.material;
    return output;
}

// Lines, points and normal-less meshes facing the camera: the normal points from the surface to the camera
float3 facing_normal(float3 world_position)
{
    return normalize(view.camera_position - world_position);
}

scene_varyings vertex_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    const uint instance_index = pull_instance_index(scene_pass, draw, instance_id);
    const instance_data instance = load_instance(scene_pass, instance_index);
    const bool has_normals = (draw.attributes & vertex_attribute_normal) != 0;

#if AVIS_POINTS
    // Six vertices per point (11 §9.1): a quad parallel to the image plane, point_size metres wide at the point's
    // depth and at least one pixel. The pixel shaders discard outside its inscribed circle; the rasteriser writes the
    // quad's depth, which is the point's.
    const uint corner_index = vertex_id % 6;
    const mesh_vertex vertex = pull_vertex(vertex_id / 6);
    projected position = project(instance, vertex.position);
    const material_data material = pull_material(scene_pass, draw.material);
    const float2 corner = quad_corners[corner_index];
    scene_varyings output;
    if (position.clip.w <= view.near_plane * 0.5)
    {
        output = (scene_varyings)0;
        output.position = culled_position;
        return output;
    }
    const float2 projection_scale = float2(view.projection_unjittered[0][0], view.projection_unjittered[1][1]);
#    if AVIS_SCREEN_ALIGNED
    const float2 half_size = max(0.5 * material.point_size * view.device_pixel_ratio, 0.5) * 2.0 * view.inverse_size;
#    else
    const float2 half_size = max(0.5 * material.point_size * projection_scale / position.clip.w, view.inverse_size);
#    endif
    offset_projected(position, corner * half_size);
    output = make_varyings(instance_index, instance, position, vertex);
    if (!has_normals)
    {
        output.normal = facing_normal(position.world);
    }
    output.quad = corner;
    return output;

#elif AVIS_LINES
    // Six vertices per segment (11 §9.2): a screen-space quad line_width pixels wide (scaled by the device pixel
    // ratio), plus the pass's line_margin on each side for the edge falloff of the blended passes. A segment crossing
    // the near plane is clipped to it first.
    const uint segment = vertex_id / 6;
    const float2 corner = quad_corners[vertex_id % 6];
    uint first = 2 * segment;
    uint second = 2 * segment + 1;
    if (draw.index_count > 0)
    {
        first = pull_index(scene_pass, draw, first);
        second = pull_index(scene_pass, draw, second);
    }
    mesh_vertex start = pull_vertex(first);
    mesh_vertex end = pull_vertex(second);
    projected start_position = project(instance, start.position);
    projected end_position = project(instance, end.position);
    scene_varyings output;
    const float near_w = view.near_plane;
    if (start_position.clip.w < near_w && end_position.clip.w < near_w)
    {
        output = (scene_varyings)0;
        output.position = culled_position;
        return output;
    }
    if (start_position.clip.w < near_w || end_position.clip.w < near_w)
    {
        const float t = (near_w - start_position.clip.w) / (end_position.clip.w - start_position.clip.w);
        const projected clipped = lerp_projected(start_position, end_position, t);
        const mesh_vertex clipped_vertex = lerp_vertex(start, end, t);
        if (start_position.clip.w < near_w)
        {
            start_position = clipped;
            start = clipped_vertex;
        }
        else
        {
            end_position = clipped;
            end = clipped_vertex;
        }
    }

    const float2 start_ndc = start_position.clip.xy / start_position.clip.w;
    const float2 end_ndc = end_position.clip.xy / end_position.clip.w;
    float2 direction = (end_ndc - start_ndc) * view.size * 0.5; // pixels, y up
    direction = dot(direction, direction) > 1e-12 ? normalize(direction) : float2(1.0, 0.0);
    const float2 across = float2(-direction.y, direction.x); // to the left of the direction

    const material_data material = pull_material(scene_pass, draw.material);
    const float half_width = 0.5 * max(material.line_width, 0.0) * view.device_pixel_ratio + scene_pass.line_margin;
    const bool at_end = corner.x > 0.0;
    projected position = start_position;
    mesh_vertex vertex = start;
    if (at_end)
    {
        position = end_position;
        vertex = end;
    }
    offset_projected(position, across * (corner.y * half_width) * 2.0 * view.inverse_size);
    output = make_varyings(instance_index, instance, position, vertex);
    if (!has_normals)
    {
        output.normal = facing_normal(position.world);
    }
    output.quad = float2(corner.x, corner.y * half_width);
    return output;

#else
    const mesh_vertex vertex = pull_vertex(vertex_id); // relative to the primitive's first vertex, indexed or not
    const projected position = project(instance, vertex.position);
    scene_varyings output = make_varyings(instance_index, instance, position, vertex);
#    if AVIS_SCREEN_ALIGNED
    if (!has_normals)
    {
        output.normal = facing_normal(position.world);
    }
#    endif
    return output;
#endif
}
