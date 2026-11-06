#ifndef AVIS_SHARED_TYPES_HLSLI
#define AVIS_SHARED_TYPES_HLSLI

// Structures and constants shared by the scene renderer's C++ code and its shaders (docs/design/11-renderer.md §13,
// docs/tasks/11-renderer.md conventions). Written once, in the common subset of HLSL and C++: in C++ a small prelude
// maps the HLSL vector and matrix types onto plain structs in avis::visualizer::rendering::gpu, and the epilogue
// static_asserts every size and offset.
//
// Layout rules, so a struct has the same layout as a C++ struct, an HLSL structured buffer element and an HLSL constant
// buffer:
// - members are 4-byte scalars, vectors and float4x4 only (no bool, no half, no arrays);
// - no vector crosses a 16-byte boundary, and every struct's size is a multiple of 16 bytes;
// - matrices are column-major in memory, as HLSL's default packing and Eigen's default storage: an Eigen::Matrix4f's
//   data() copies straight into a float4x4, and mul(matrix, vector) applies it.
//
// Some members are placeholders until the pass that reads them exists; their task is named next to them.

#ifdef __cplusplus

#    include <cstddef>
#    include <cstdint>

namespace avis::visualizer::rendering::gpu
{
    using uint = std::uint32_t;

    struct float2
    {
        float x, y;
    };

    struct float3
    {
        float x, y, z;
    };

    struct float4
    {
        float x, y, z, w;
    };

    struct uint2
    {
        uint x, y;
    };

    struct uint4
    {
        uint x, y, z, w;
    };

    // Column-major: columns[c] is column c, as Eigen::Matrix4f stores it
    struct float4x4
    {
        float4 columns[4];
    };

#    define AVIS_SHARED_CONSTANT(type, name, value) inline constexpr type name = value

#else

#    define AVIS_SHARED_CONSTANT(type, name, value) static const type name = value

#endif

// ---- Constants ----

// material_data::shading_model
AVIS_SHARED_CONSTANT(uint, shading_model_lit, 0);
AVIS_SHARED_CONSTANT(uint, shading_model_unlit, 1);

// material_data::alpha_mode
AVIS_SHARED_CONSTANT(uint, alpha_mode_opaque, 0);
AVIS_SHARED_CONSTANT(uint, alpha_mode_mask, 1);
AVIS_SHARED_CONSTANT(uint, alpha_mode_blend, 2);

// material_data::blend_operation (alpha_mode_blend only)
AVIS_SHARED_CONSTANT(uint, blend_operation_normal, 0);
AVIS_SHARED_CONSTANT(uint, blend_operation_additive, 1);
AVIS_SHARED_CONSTANT(uint, blend_operation_subtractive, 2);

// material_data::flags
AVIS_SHARED_CONSTANT(uint, material_flag_double_sided, 1u << 0);
AVIS_SHARED_CONSTANT(uint, material_flag_use_vertex_color, 1u << 1);

// Vertex attribute mask (11 §3.2); the position is always present
AVIS_SHARED_CONSTANT(uint, vertex_attribute_position, 1u << 0); // f32x3
AVIS_SHARED_CONSTANT(uint, vertex_attribute_normal, 1u << 1); // octahedral u16x2
AVIS_SHARED_CONSTANT(uint, vertex_attribute_tangent, 1u << 2); // octahedral u16x2 + sign
AVIS_SHARED_CONSTANT(uint, vertex_attribute_uv0, 1u << 3); // f16x2
AVIS_SHARED_CONSTANT(uint, vertex_attribute_color0, 1u << 4); // u8x4, sRGB

// Mesh topology (11 §3.2)
AVIS_SHARED_CONSTANT(uint, topology_triangles, 0);
AVIS_SHARED_CONSTANT(uint, topology_lines, 1);
AVIS_SHARED_CONSTANT(uint, topology_points, 2);

// instance_data::flags (11 §3.1)
AVIS_SHARED_CONSTANT(uint, instance_flag_visible, 1u << 0);
AVIS_SHARED_CONSTANT(uint, instance_flag_casts_shadows, 1u << 1);
AVIS_SHARED_CONSTANT(uint, instance_flag_selected, 1u << 2);
AVIS_SHARED_CONSTANT(uint, instance_flag_pickable, 1u << 3);
AVIS_SHARED_CONSTANT(uint, instance_flag_screen_aligned, 1u << 4);
AVIS_SHARED_CONSTANT(uint, instance_flag_history_invalid, 1u << 5);

// light_data::kind (11 §6.2)
AVIS_SHARED_CONSTANT(uint, light_kind_directional, 0);
AVIS_SHARED_CONSTANT(uint, light_kind_point, 1);
AVIS_SHARED_CONSTANT(uint, light_kind_spot, 2);

// ---- Structures ----

// One material slot of the renderer's material buffer (11 §3.3): glTF 2.0 metallic-roughness plus the generic extensions
// for unlit, blended, line and point content. Texture fields are bindless SRV indices; 0 means "none".
struct material_data
{
    float4 base_color_factor;
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    float occlusion_strength;
    float3 emissive_factor;
    float alpha_cutoff;
    uint base_color_texture;
    uint metallic_roughness_texture;
    uint normal_texture;
    uint occlusion_texture;
    uint emissive_texture;
    uint sampler_index; // a binding_layout::static_sampler: linear_wrap or linear_clamp
    uint shading_model; // shading_model_*
    uint alpha_mode; // alpha_mode_*
    uint blend_operation; // blend_operation_*
    uint flags; // material_flag_*
    float point_size; // metres, for point topology (at least 1 px)
    float line_width; // pixels, for line topology
};

// One visible instance of the frame (11 §3.5), read by the vertex shaders through instance_indices (11 §4.8)
struct instance_data
{
    float4x4 world; // interpolated world-from-object at t_render
    float4x4 world_previous; // the world matrix of the previous rendered frame, for motion vectors
    float4x4 normal_from_object; // inverse transpose of world's upper 3x3, in a float4x4 for the packing rules
    uint mesh; // geometry arena entry (11.3)
    uint material; // material slot (11.4)
    uint flags; // instance_flag_*
    uint layer_priority; // ordering within the overlay layers
    uint object_id_low; // the opaque 64-bit object id, for picking (11 §5.1)
    uint object_id_high;
    uint padding0;
    uint padding1;
};

// One light (11 §6.2). Directions point from the light into the scene.
struct light_data
{
    float3 position; // world space; point and spot
    float range; // metres; point and spot
    float3 direction; // world space, unit length; directional and spot
    uint kind; // light_kind_*
    float3 color; // linear RGB, the light's colour; intensity scales it
    float intensity; // directional: illuminance (lux); point and spot: luminous intensity (cd)
    float spot_cos_inner; // cosine of the inner cone angle
    float spot_cos_outer; // cosine of the outer cone angle
    uint casts_shadows; // 0 or 1; only the sun casts shadows in iteration 1
    uint padding0;
};

// The view constants, a root CBV at b2 (11.10). Projections are reverse-Z; view space looks down -Z, so a point's
// linear depth is -z_view (the clip w of a perspective projection).
struct view_constants
{
    float4x4 view; // view-from-world
    float4x4 view_inverse; // world-from-view
    float4x4 projection; // jittered when TAA is on
    float4x4 projection_inverse; // of the jittered projection
    float4x4 projection_unjittered;
    float4x4 view_projection; // projection * view, jittered
    float4x4 view_projection_unjittered;
    float4x4 view_projection_previous; // unjittered, of the previous rendered frame (this frame's without one)
    float4x4 view_projection_inverse; // world-from-clip, of the jittered view_projection
    float2 jitter; // in clip space units: added to the unjittered projection's x and y offsets
    float2 size; // pixels
    float2 inverse_size;
    float near_plane; // metres
    float device_pixel_ratio;
    float3 camera_position; // world space
    uint frame_index;
};

// The root constants of one scene draw (11 §4.8, 11.9), pushed by the range walker with
// binding_layout::push_draw_constants. The vertex shader reads instance_indices[instance_index_offset + SV_InstanceID]
// and pulls the vertices from the geometry arena by the stream offsets.
struct scene_draw_constants
{
    uint instance_index_offset; // the draw's first entry in instance_indices[]
    uint material; // material slot
    uint mesh; // index into the frame's mesh table (instance_data::mesh)
    uint attributes; // vertex_attribute_* of the mesh
    uint vertex_first; // the primitive's first vertex: indices, and SV_VertexID of non-indexed draws, are relative to it
    uint vertex_count; // the primitive's vertex count
    uint index_first; // the primitive's first index in the arena's index buffer, in indices
    uint index_count; // 0 for a non-indexed primitive
    uint position_offset; // byte offsets of the streams in the arena's vertex buffer; vertex_stream_absent when absent
    uint normal_offset;
    uint tangent_offset;
    uint uv0_offset;
    uint color0_offset;
    uint padding0;
    uint padding1;
    uint padding2;
};

// scene_draw_constants' offset of a stream the mesh doesn't have
AVIS_SHARED_CONSTANT(uint, vertex_stream_absent, 0xFFFFFFFFu);

// The pass constants (b1) of the scene passes (11.9): the bindless SRVs of the frame's arrays and the geometry arena
struct scene_pass_constants
{
    uint instances; // StructuredBuffer<instance_data>
    uint instance_indices; // StructuredBuffer<uint>
    uint vertices; // ByteAddressBuffer, the arena's vertex buffer
    uint indices; // ByteAddressBuffer, the arena's index buffer (u32)
    uint materials; // StructuredBuffer<material_data>
    float line_margin; // pixels a line's quad extends past its width on each side, for the edge falloff: 0 in the
                       // opaque passes, so the depth and the G-buffer cover line_width pixels exactly (11 §9.2)
    uint padding1;
    uint padding2;
};

#ifdef __cplusplus

static_assert(sizeof(float2) == 8 && sizeof(float3) == 12 && sizeof(float4) == 16);
static_assert(sizeof(uint2) == 8 && sizeof(uint4) == 16 && sizeof(float4x4) == 64);

static_assert(sizeof(material_data) == 96);
static_assert(offsetof(material_data, base_color_factor) == 0);
static_assert(offsetof(material_data, metallic_factor) == 16);
static_assert(offsetof(material_data, roughness_factor) == 20);
static_assert(offsetof(material_data, normal_scale) == 24);
static_assert(offsetof(material_data, occlusion_strength) == 28);
static_assert(offsetof(material_data, emissive_factor) == 32);
static_assert(offsetof(material_data, alpha_cutoff) == 44);
static_assert(offsetof(material_data, base_color_texture) == 48);
static_assert(offsetof(material_data, metallic_roughness_texture) == 52);
static_assert(offsetof(material_data, normal_texture) == 56);
static_assert(offsetof(material_data, occlusion_texture) == 60);
static_assert(offsetof(material_data, emissive_texture) == 64);
static_assert(offsetof(material_data, sampler_index) == 68);
static_assert(offsetof(material_data, shading_model) == 72);
static_assert(offsetof(material_data, alpha_mode) == 76);
static_assert(offsetof(material_data, blend_operation) == 80);
static_assert(offsetof(material_data, flags) == 84);
static_assert(offsetof(material_data, point_size) == 88);
static_assert(offsetof(material_data, line_width) == 92);

static_assert(sizeof(instance_data) == 224);
static_assert(offsetof(instance_data, world) == 0);
static_assert(offsetof(instance_data, world_previous) == 64);
static_assert(offsetof(instance_data, normal_from_object) == 128);
static_assert(offsetof(instance_data, mesh) == 192);
static_assert(offsetof(instance_data, material) == 196);
static_assert(offsetof(instance_data, flags) == 200);
static_assert(offsetof(instance_data, layer_priority) == 204);
static_assert(offsetof(instance_data, object_id_low) == 208);
static_assert(offsetof(instance_data, object_id_high) == 212);
static_assert(offsetof(instance_data, padding0) == 216);
static_assert(offsetof(instance_data, padding1) == 220);

static_assert(sizeof(light_data) == 64);
static_assert(offsetof(light_data, position) == 0);
static_assert(offsetof(light_data, range) == 12);
static_assert(offsetof(light_data, direction) == 16);
static_assert(offsetof(light_data, kind) == 28);
static_assert(offsetof(light_data, color) == 32);
static_assert(offsetof(light_data, intensity) == 44);
static_assert(offsetof(light_data, spot_cos_inner) == 48);
static_assert(offsetof(light_data, spot_cos_outer) == 52);
static_assert(offsetof(light_data, casts_shadows) == 56);
static_assert(offsetof(light_data, padding0) == 60);

static_assert(sizeof(view_constants) == 624);
static_assert(offsetof(view_constants, view) == 0);
static_assert(offsetof(view_constants, view_inverse) == 64);
static_assert(offsetof(view_constants, projection) == 128);
static_assert(offsetof(view_constants, projection_inverse) == 192);
static_assert(offsetof(view_constants, projection_unjittered) == 256);
static_assert(offsetof(view_constants, view_projection) == 320);
static_assert(offsetof(view_constants, view_projection_unjittered) == 384);
static_assert(offsetof(view_constants, view_projection_previous) == 448);
static_assert(offsetof(view_constants, view_projection_inverse) == 512);
static_assert(offsetof(view_constants, jitter) == 576);
static_assert(offsetof(view_constants, size) == 584);
static_assert(offsetof(view_constants, inverse_size) == 592);
static_assert(offsetof(view_constants, near_plane) == 600);
static_assert(offsetof(view_constants, device_pixel_ratio) == 604);
static_assert(offsetof(view_constants, camera_position) == 608);
static_assert(offsetof(view_constants, frame_index) == 620);

static_assert(sizeof(scene_draw_constants) == 64); // binding_layout::draw_constant_count 32-bit values
static_assert(offsetof(scene_draw_constants, instance_index_offset) == 0);
static_assert(offsetof(scene_draw_constants, material) == 4);
static_assert(offsetof(scene_draw_constants, mesh) == 8);
static_assert(offsetof(scene_draw_constants, attributes) == 12);
static_assert(offsetof(scene_draw_constants, vertex_first) == 16);
static_assert(offsetof(scene_draw_constants, vertex_count) == 20);
static_assert(offsetof(scene_draw_constants, index_first) == 24);
static_assert(offsetof(scene_draw_constants, index_count) == 28);
static_assert(offsetof(scene_draw_constants, position_offset) == 32);
static_assert(offsetof(scene_draw_constants, normal_offset) == 36);
static_assert(offsetof(scene_draw_constants, tangent_offset) == 40);
static_assert(offsetof(scene_draw_constants, uv0_offset) == 44);
static_assert(offsetof(scene_draw_constants, color0_offset) == 48);

static_assert(sizeof(scene_pass_constants) == 32);
static_assert(offsetof(scene_pass_constants, instances) == 0);
static_assert(offsetof(scene_pass_constants, instance_indices) == 4);
static_assert(offsetof(scene_pass_constants, vertices) == 8);
static_assert(offsetof(scene_pass_constants, indices) == 12);
static_assert(offsetof(scene_pass_constants, materials) == 16);
static_assert(offsetof(scene_pass_constants, line_margin) == 20);

} // namespace avis::visualizer::rendering::gpu

#endif

#undef AVIS_SHARED_CONSTANT

#endif
