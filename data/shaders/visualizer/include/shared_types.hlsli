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

    struct int2
    {
        int x, y;
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

// "No light": deferred_lighting_constants::sun when the frame has no sun
AVIS_SHARED_CONSTANT(uint, no_light, 0xFFFFFFFFu);

// The light cluster grid (11 §6.3): 16 × 9 screen tiles × 24 exponential depth slices from cluster_near to
// cluster_far (metres of linear depth). Cluster (x, y, z) is index x + 16·(y + 9·z). Each cluster holds at most
// cluster_maximum_lights lights, at cluster_maximum_lights · index in the light index list.
AVIS_SHARED_CONSTANT(uint, cluster_count_x, 16);
AVIS_SHARED_CONSTANT(uint, cluster_count_y, 9);
AVIS_SHARED_CONSTANT(uint, cluster_count_z, 24);
AVIS_SHARED_CONSTANT(uint, cluster_count, 16 * 9 * 24);
AVIS_SHARED_CONSTANT(uint, cluster_maximum_lights, 256);
AVIS_SHARED_CONSTANT(float, cluster_near, 0.1f);
AVIS_SHARED_CONSTANT(float, cluster_far, 2000.0f);

// sky_constants::mode
AVIS_SHARED_CONSTANT(uint, sky_mode_model, 0); // the analytic sky of the environment
AVIS_SHARED_CONSTANT(uint, sky_mode_uniform, 1); // uniform_radiance in every direction (tests, tools)

// The baked environment (11 §6.4)
AVIS_SHARED_CONSTANT(uint, environment_sky_size, 256); // texels per face of the sky cubemap
AVIS_SHARED_CONSTANT(uint, environment_prefiltered_size, 128); // texels per face of the prefiltered cubemap's mip 0
AVIS_SHARED_CONSTANT(uint, environment_prefiltered_mips, 6); // mip m is for roughness m / 5
AVIS_SHARED_CONSTANT(uint, environment_irradiance_coefficients, 9); // SH9, one float4 (RGB) each
AVIS_SHARED_CONSTANT(uint, brdf_lut_size, 128); // texel (i, j) is NoV = (i + 0.5) / 128, roughness = (j + 0.5) / 128

// The view's exposure buffer (11 §10.3, view_history), a raw buffer: a 16-byte header, then the histogram's 128 bin
// counts and 128 sums. A pixel's position in the histogram is (log2(pre-exposed luminance / key) − minimum) · bins per
// stop, clamped to [0, 128): its bin is the integer part, and its sum adds the fraction in 1/256ths.
AVIS_SHARED_CONSTANT(uint, exposure_offset_current, 0); // float: the pre-exposure of the frame being rendered
AVIS_SHARED_CONSTANT(uint, exposure_offset_previous, 4); // float: the previous frame's, which the TAA history has
AVIS_SHARED_CONSTANT(uint, exposure_offset_luminance, 8); // float: the luminance the last adaptation measured; 0: none
AVIS_SHARED_CONSTANT(uint, exposure_offset_counts, 16); // uint per bin
AVIS_SHARED_CONSTANT(uint, exposure_offset_sums, 16 + 128 * 4); // uint per bin
AVIS_SHARED_CONSTANT(uint, exposure_buffer_bytes, 16 + 2 * 128 * 4);
AVIS_SHARED_CONSTANT(uint, exposure_histogram_bins, 128);
AVIS_SHARED_CONSTANT(float, exposure_histogram_minimum, -8.0f); // stops below the key of bin 0's start
AVIS_SHARED_CONSTANT(float, exposure_histogram_bins_per_stop, 8.0f); // so the bins span 16 stops around the key
AVIS_SHARED_CONSTANT(uint, exposure_histogram_fraction_steps, 256);

// exposure_constants::mode
AVIS_SHARED_CONSTANT(uint, exposure_mode_automatic, 0); // adapt towards the histogram's average
AVIS_SHARED_CONSTANT(uint, exposure_mode_manual, 1); // the next exposure is manual_exposure
AVIS_SHARED_CONSTANT(uint, exposure_mode_reset, 2); // both header exposures become manual_exposure, the bins 0

// tonemap_constants::tonemapper
AVIS_SHARED_CONSTANT(uint, tonemapper_aces, 0);
AVIS_SHARED_CONSTANT(uint, tonemapper_agx, 1);

// The tonemap's dither (11 §10.4): a blue-noise texture of this many texels per side, tiled over the view
AVIS_SHARED_CONSTANT(uint, blue_noise_size, 64);

// The selection outline (11 §10.5): pixels within this many pixels of a selected one (a disc) are outlined
AVIS_SHARED_CONSTANT(uint, selection_outline_radius, 2);

// Picking (11 §5.1): the texels read back around a click, per side, and the coverage below which a pixel isn't picked
// (a blend material's alpha times a line's edge coverage)
AVIS_SHARED_CONSTANT(uint, pick_window_size, 9);
AVIS_SHARED_CONSTANT(float, picking_minimum_coverage, 0.1f);

// The debug blit (11 §11): debug_blit_constants::source_kind, how the source is read
AVIS_SHARED_CONSTANT(uint, debug_source_float, 0); // Texture2D<float4> (Texture2DArray<float4> when is_array)
AVIS_SHARED_CONSTANT(uint, debug_source_uint, 1); // Texture2D<uint4> (Texture2DArray<uint4>): an integer format
AVIS_SHARED_CONSTANT(uint, debug_source_depth, 2); // a depth format, through its R32_FLOAT SRV, as debug_source_float
AVIS_SHARED_CONSTANT(uint, debug_source_clusters, 3); // StructuredBuffer<cluster_lights>, with linear_depth

// debug_blit_constants::mode: the values of rendering::debug_blit_mode
AVIS_SHARED_CONSTANT(uint, debug_mode_raw, 0);
AVIS_SHARED_CONSTANT(uint, debug_mode_depth_linear, 1);
AVIS_SHARED_CONSTANT(uint, debug_mode_normal_octahedral, 2);
AVIS_SHARED_CONSTANT(uint, debug_mode_velocity, 3);
AVIS_SHARED_CONSTANT(uint, debug_mode_ao, 4);
AVIS_SHARED_CONSTANT(uint, debug_mode_cascade_index, 5);
AVIS_SHARED_CONSTANT(uint, debug_mode_cluster_light_count, 6);
AVIS_SHARED_CONSTANT(uint, debug_mode_luminance_heat, 7);
AVIS_SHARED_CONSTANT(uint, debug_mode_alpha_only, 8);

// The pixel inspector's texel (11 §11): float4 value at 0, uint4 bits at 16
AVIS_SHARED_CONSTANT(uint, debug_inspector_bytes, 32);

// A view's cluster statistics (11 §6.3), a uint4: the light references dropped past cluster_maximum_lights, the clusters
// that dropped any, the most lights a cluster touched (dropped ones included), and the light references kept
AVIS_SHARED_CONSTANT(uint, cluster_statistics_bytes, 16);

// The sun's shadow cascades (11 §7): at most shadow_maximum_cascades slices of the shadow map. cascade_index (the
// debug output of deferred_lighting) is no_cascade for a pixel no cascade covers.
AVIS_SHARED_CONSTANT(uint, shadow_maximum_cascades, 4);
AVIS_SHARED_CONSTANT(uint, no_cascade, 255);

// GTAO (11 §8): gtao_denoise runs gtao_denoise_passes times over the raw AO
AVIS_SHARED_CONSTANT(uint, gtao_denoise_passes, 2);

// Custom screenshots (11 §12.1): the sub-frames accumulated into the screenshot view's RGBA32F accumulation texture
AVIS_SHARED_CONSTANT(uint, screenshot_subframes, 8);

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

// One cluster's view-space bounds (11 §6.3), written by cluster_build and kept in the view's history
struct cluster_bounds
{
    float3 minimum;
    float padding0;
    float3 maximum;
    float padding1;
};

// One cluster's lights, written by light_cull: indices into the frame's light_data[] at offset .. offset + count - 1
// of the light index list; `dropped` lights touched the cluster past cluster_maximum_lights
struct cluster_lights
{
    uint offset;
    uint count;
    uint dropped;
    uint padding0;
};

// The analytic sky (11 §6.4), evaluated by sky.hlsli. The model is Preetham et al. 1999 (the task's Open point
// "Hosek-Wilkie data"): each of luminance Y and chromaticity x, y is zenith · F(θ, γ) / F(0, θs) with the Perez function
// F(θ, γ) = (1 + A·exp(B / cos θ)) · (1 + C·exp(D·γ) + E·cos²γ). zenith holds zenith / F(0, θs) per channel; Y is in
// cd/m². Below the horizon the radiance is ground_radiance. World space is Z up.
struct sky_constants
{
    float3 perez_a; // per channel Y, x, y
    uint mode; // sky_mode_*
    float3 perez_b;
    float luminance_scale; // multiplies Y: fades the sky out as the sun sets
    float3 perez_c;
    float padding0;
    float3 perez_d;
    float padding1;
    float3 perez_e;
    float padding2;
    float3 zenith; // Y (cd/m²), x, y, each divided by F(0, θs)
    float padding3;
    float3 sun_position; // unit vector towards the sun
    float padding4;
    float3 ground_radiance; // linear RGB, cd/m²
    float padding5;
    float3 uniform_radiance; // sky_mode_uniform: linear RGB, cd/m²
    float padding6;
};

// The pass constants (b1) of cluster_build: the cluster bounds of one projection
struct cluster_build_constants
{
    float4x4 projection_inverse; // view from clip, of the unjittered projection
    uint bounds; // UAV, RWStructuredBuffer<cluster_bounds>
    uint padding0;
    uint2 size; // the view's size, pixels
    uint2 tile_size; // pixels per cluster tile
    uint2 padding1;
};

// One shadow cascade, as the receiver reads it (11 §7)
struct shadow_cascade_data
{
    float4x4 shadow_from_world; // clip of the cascade: x, y in [-1, 1], z the reverse-Z depth (1 towards the sun)
    float split_far; // the view depth the cascade ends at
    float blend_start; // the view depth its blend into the next cascade (or into "lit") starts at
    float texel_size; // metres per shadow-map texel
    float padding0;
};

// The sun's shadow (11 §7): the cascades and the receiver's filter. The cascades are members rather than an array
// (the layout rules above); shadows.hlsli indexes them.
struct shadow_constants
{
    shadow_cascade_data cascade0;
    shadow_cascade_data cascade1;
    shadow_cascade_data cascade2;
    shadow_cascade_data cascade3;
    uint shadow_map; // SRV of the D32F Texture2DArray; 0: no shadows, everything is lit
    uint cascade_count;
    float resolution; // texels per side of a slice
    float normal_offset; // texels of the receiver's cascade along its normal, times sin of the normal-sun angle
    float filter_radius; // the Poisson disk's radius, texels
    uint padding0;
    uint padding1;
    uint padding2;
};

// The pass constants (b1) of deferred_lighting (11 §6.5); bindless indices of the G-buffer, the clusters and the baked
// environment
struct deferred_lighting_constants
{
    uint albedo; // gbuffer_albedo SRV
    uint normal; // gbuffer_normal SRV
    uint material; // gbuffer_material SRV
    uint emissive; // gbuffer_emissive SRV
    uint depth; // SRV of the depth buffer, read as R32F
    uint linear_depth; // SRV, R32F
    uint hdr_color; // UAV, RGBA16F
    uint lights; // StructuredBuffer<light_data>
    uint cluster_lights; // StructuredBuffer<cluster_lights>
    uint light_indices; // StructuredBuffer<uint>
    uint sun; // index of the sun in lights, or no_light
    uint ambient_occlusion; // SRV of GTAO's denoised AO (R8); 0: no occlusion
    uint irradiance; // StructuredBuffer<float4>, environment_irradiance_coefficients SH9 coefficients
    uint prefiltered; // TextureCube SRV
    uint brdf_lut; // Texture2D SRV, RG16F
    uint cascade_index; // UAV of the cascade_index debug output (R8_UINT); 0: not written
    uint2 size; // pixels
    uint2 tile_size; // pixels per cluster tile
    uint exposure_buffer; // ByteAddressBuffer SRV, the view's exposure: hdr_color = radiance · its current exposure
    float ambient_intensity; // scales the image-based lighting
    uint ambient_occlusion_scale; // pixels per AO texel along each axis: 1, or 2 at half resolution (upsampled)
    uint padding0;
    shadow_constants shadow; // the sun's
};

// The pass constants (b1) of the sky pass (11 §2.3 pass 9)
struct sky_pass_constants
{
    sky_constants sky;
    uint exposure_buffer; // ByteAddressBuffer SRV, the view's exposure
    uint padding0;
    uint padding1;
    uint padding2;
};

// The pass constants (b1) of gtao (11 §8, §2.3 pass 5). AO texel p is centred on pixel p · scale.
struct gtao_constants
{
    uint depth_mips; // SRV, RG32F: min and max linear depth (metres) per texel, every mip
    uint normal; // gbuffer_normal SRV
    uint ao; // UAV, R8_UNORM: the raw AO
    uint edges; // UAV, R8_UNORM: the depth edges, 2 bits each for left, right, top and bottom
    uint2 size; // texels of ao and edges
    uint scale; // pixels per AO texel along each axis: 1, or 2 at half resolution
    uint depth_mip_count;
    uint slice_count;
    uint steps_per_side;
    float radius; // metres
    float falloff; // the fraction of the radius over which occluders fade out
    float intensity; // the power curve applied to the visibility
    uint padding0;
    uint padding1;
    uint padding2;
};

// The draw constants (b0) of gtao_denoise (11 §8, §2.3 pass 6)
struct gtao_denoise_constants
{
    uint source; // SRV, R8_UNORM
    uint edges; // SRV, R8_UNORM, gtao's
    uint destination; // UAV, R8_UNORM
    uint padding0;
    uint2 size; // texels
    uint padding1;
    uint padding2;
};

// What forward_transparent shades with (11 §10.1): the lights, clusters, shadows and baked environment of deferred
// lighting, without GTAO. The pass constants (b1) are forward_pass_constants: scene_pass_constants first, so the
// scene vertex shader reads them as its own.
struct forward_lighting_constants
{
    uint lights; // StructuredBuffer<light_data>
    uint cluster_lights; // StructuredBuffer<cluster_lights>
    uint light_indices; // StructuredBuffer<uint>
    uint sun; // index of the sun in lights, or no_light
    uint irradiance; // StructuredBuffer<float4>, SH9
    uint prefiltered; // TextureCube SRV
    uint brdf_lut; // Texture2D SRV, RG16F
    uint padding0;
    uint2 tile_size; // pixels per cluster tile
    uint exposure_buffer; // ByteAddressBuffer SRV, the view's exposure: hdr_color = radiance · its current exposure
    float ambient_intensity; // scales the image-based lighting
    shadow_constants shadow; // the sun's
};

struct forward_pass_constants
{
    scene_pass_constants scene;
    forward_lighting_constants lighting;
};

// The pass constants (b1) of taa_resolve (11 §10.2, §2.3 pass 11). The view constants (b2) are the frame's camera.
struct taa_constants
{
    uint hdr_color; // Texture2D<float4> SRV, this frame's pre-exposed colour
    uint velocity; // Texture2D<float2> SRV, current minus previous unjittered UV
    uint linear_depth; // Texture2D<float> SRV, view-space depth, +infinity where nothing was drawn
    uint reactive_mask; // Texture2D<float> SRV
    uint history; // Texture2D<float4> SRV, the previous frame's taa_output; unread without history
    uint depth_history; // Texture2D<float> SRV, the previous frame's linear depth; unread without history
    uint output; // RWTexture2D<float4> UAV, taa_output: this frame's history slot
    uint depth_output; // RWTexture2D<float> UAV, this frame's depth history slot
    uint has_history; // 0: the output is this frame's colour, unaccumulated
    uint padding0;
    uint2 size; // pixels
    float blend; // the base weight of this frame's colour
    float gamma; // the variance box's half-size, in standard deviations
    uint exposure_buffer; // ByteAddressBuffer SRV: the history is rescaled by the current over the previous exposure
    float disocclusion_tolerance; // the relative depth difference beyond which the history is rejected
};

// The pass constants (b1) of exposure_histogram and exposure_adapt (11 §10.3, §2.3 pass 12)
struct exposure_constants
{
    uint exposure_buffer; // RWByteAddressBuffer UAV, the view's exposure
    uint color; // Texture2D<float4> SRV, taa_output (pre-exposed); histogram only
    uint2 size; // pixels of color; histogram only
    uint mode; // adapt: exposure_mode_*
    float key; // the pre-exposed luminance the average is brought to
    float manual_exposure; // adapt: the exposure of exposure_mode_manual and exposure_mode_reset
    float frame_time; // adapt: seconds since the view's previous adaptation
    float brighten_rate; // adapt: per second, while the exposure rises (the image brightens)
    float darken_rate; // adapt: per second, while it falls
    float minimum_exposure; // adapt: the range the exposure is kept in
    float maximum_exposure;
};

// The pass constants (b1) of the pick window (11 §5.1)
struct pick_window_constants
{
    uint picking_id; // Texture2D<uint2> SRV, R32G32_UINT
    uint window; // RWByteAddressBuffer UAV: pick_window_size² texels of 8 bytes, row by row
    int2 origin; // the window's top-left texel; may be outside the view
    uint2 size; // the view's size, pixels
    uint2 padding0;
};

// The pass constants (b1) of the selection outline (11 §10.5)
struct selection_outline_constants
{
    uint selection_mask; // Texture2D<float> SRV, R8_UNORM: 1 where a selected instance is visible
    uint ldr_color; // RWTexture2D<float4> UAV, RGBA8_UNORM
    uint2 size; // pixels
    float4 color; // the outline's, sRGB-encoded, alpha 1
};

// The pass constants (b1) of the tonemap (11 §10.4, §2.3 pass 13)
struct tonemap_constants
{
    uint color; // Texture2D<float4> SRV, taa_output
    uint ldr_color; // RWTexture2D<float4> UAV, RGBA8_UNORM: sRGB-encoded
    uint exposure_buffer; // ByteAddressBuffer SRV: color is rescaled by the current over the previous exposure
    uint blue_noise; // Texture2D<float> SRV, R8_UNORM, blue_noise_size²: the dither, a rank in 1/256ths
    uint2 size; // pixels
    uint tonemapper; // tonemapper_*
    uint padding0;
};

// The pass constants (b1) of a debug blit and of the pixel inspector (11 §11). The view constants (b2) are the camera's,
// for linearizing a depth format.
struct debug_blit_constants
{
    uint source; // SRV, as source_kind says
    uint source_kind; // debug_source_*
    uint is_array; // 1: a Texture2DArray, read at `slice`
    uint mode; // debug_mode_*
    uint mip;
    uint slice;
    uint channels; // debug_mode_raw: bit 0 R … bit 3 A
    uint linear_depth; // debug_source_clusters: Texture2D<float> SRV, the view's linear depth
    int2 origin; // the blit's rectangle in ldr_color, pixels
    uint2 extent;
    uint2 source_size; // texels of the mip; the view's size for debug_source_clusters
    uint2 tile_size; // debug_source_clusters: pixels per cluster tile
    float range_minimum;
    float range_maximum;
    uint2 texel; // the inspector's texel in the mip
    uint output; // the inspector's RWByteAddressBuffer UAV, debug_inspector_bytes
    uint padding0;
    uint padding1;
    uint padding2;
};

// The pass constants (b1) of a view's cluster statistics (11 §6.3)
struct cluster_statistics_constants
{
    uint cluster_lights; // StructuredBuffer<cluster_lights> SRV, cluster_count entries
    uint output; // RWByteAddressBuffer UAV, cluster_statistics_bytes
    uint padding0;
    uint padding1;
};

// The pass constants (b1) of screenshot.hlsl (11 §12.1): accumulate_main adds hdr_color, divided by the exposure it was
// pre-exposed with, into the accumulation texture (the first sub-frame replaces it); resolve_main writes the average,
// pre-exposed again with the same buffer's current exposure, into the resolved texture
struct screenshot_constants
{
    uint2 size; // pixels
    uint color; // accumulate: Texture2D<float4> SRV, hdr_color
    uint accumulation; // RWTexture2D<float4> UAV (accumulate), Texture2D<float4> SRV (resolve)
    uint exposure_buffer; // ByteAddressBuffer SRV, the screenshot view's exposure
    uint first; // accumulate: 1 for the first sub-frame
    uint resolved; // resolve: RWTexture2D<float4> UAV
    float weight; // resolve: 1 / the sub-frames accumulated
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

static_assert(sizeof(cluster_bounds) == 32);
static_assert(offsetof(cluster_bounds, minimum) == 0);
static_assert(offsetof(cluster_bounds, maximum) == 16);

static_assert(sizeof(cluster_lights) == 16);
static_assert(offsetof(cluster_lights, offset) == 0);
static_assert(offsetof(cluster_lights, count) == 4);
static_assert(offsetof(cluster_lights, dropped) == 8);

static_assert(sizeof(sky_constants) == 144);
static_assert(offsetof(sky_constants, perez_a) == 0);
static_assert(offsetof(sky_constants, mode) == 12);
static_assert(offsetof(sky_constants, perez_b) == 16);
static_assert(offsetof(sky_constants, luminance_scale) == 28);
static_assert(offsetof(sky_constants, perez_c) == 32);
static_assert(offsetof(sky_constants, perez_d) == 48);
static_assert(offsetof(sky_constants, perez_e) == 64);
static_assert(offsetof(sky_constants, zenith) == 80);
static_assert(offsetof(sky_constants, sun_position) == 96);
static_assert(offsetof(sky_constants, ground_radiance) == 112);
static_assert(offsetof(sky_constants, uniform_radiance) == 128);

static_assert(sizeof(cluster_build_constants) == 96);
static_assert(offsetof(cluster_build_constants, projection_inverse) == 0);
static_assert(offsetof(cluster_build_constants, bounds) == 64);
static_assert(offsetof(cluster_build_constants, size) == 72);
static_assert(offsetof(cluster_build_constants, tile_size) == 80);

static_assert(sizeof(shadow_cascade_data) == 80);
static_assert(offsetof(shadow_cascade_data, shadow_from_world) == 0);
static_assert(offsetof(shadow_cascade_data, split_far) == 64);
static_assert(offsetof(shadow_cascade_data, blend_start) == 68);
static_assert(offsetof(shadow_cascade_data, texel_size) == 72);

static_assert(sizeof(shadow_constants) == 352);
static_assert(offsetof(shadow_constants, cascade0) == 0);
static_assert(offsetof(shadow_constants, cascade1) == 80);
static_assert(offsetof(shadow_constants, cascade2) == 160);
static_assert(offsetof(shadow_constants, cascade3) == 240);
static_assert(offsetof(shadow_constants, shadow_map) == 320);
static_assert(offsetof(shadow_constants, cascade_count) == 324);
static_assert(offsetof(shadow_constants, resolution) == 328);
static_assert(offsetof(shadow_constants, normal_offset) == 332);
static_assert(offsetof(shadow_constants, filter_radius) == 336);

static_assert(sizeof(deferred_lighting_constants) == 448);
static_assert(offsetof(deferred_lighting_constants, albedo) == 0);
static_assert(offsetof(deferred_lighting_constants, depth) == 16);
static_assert(offsetof(deferred_lighting_constants, cluster_lights) == 32);
static_assert(offsetof(deferred_lighting_constants, irradiance) == 48);
static_assert(offsetof(deferred_lighting_constants, cascade_index) == 60);
static_assert(offsetof(deferred_lighting_constants, shadow) == 96);
static_assert(offsetof(deferred_lighting_constants, size) == 64);
static_assert(offsetof(deferred_lighting_constants, tile_size) == 72);
static_assert(offsetof(deferred_lighting_constants, exposure_buffer) == 80);
static_assert(offsetof(deferred_lighting_constants, ambient_intensity) == 84);
static_assert(offsetof(deferred_lighting_constants, ambient_occlusion_scale) == 88);

static_assert(sizeof(sky_pass_constants) == 160);
static_assert(offsetof(sky_pass_constants, sky) == 0);
static_assert(offsetof(sky_pass_constants, exposure_buffer) == 144);

static_assert(sizeof(gtao_constants) == 64);
static_assert(offsetof(gtao_constants, size) == 16);
static_assert(offsetof(gtao_constants, scale) == 24);
static_assert(offsetof(gtao_constants, slice_count) == 32);
static_assert(offsetof(gtao_constants, radius) == 40);
static_assert(offsetof(gtao_constants, intensity) == 48);

static_assert(sizeof(gtao_denoise_constants) == 32);
static_assert(offsetof(gtao_denoise_constants, size) == 16);

static_assert(sizeof(forward_lighting_constants) == 400);
static_assert(offsetof(forward_lighting_constants, irradiance) == 16);
static_assert(offsetof(forward_lighting_constants, tile_size) == 32);
static_assert(offsetof(forward_lighting_constants, exposure_buffer) == 40);
static_assert(offsetof(forward_lighting_constants, ambient_intensity) == 44);
static_assert(offsetof(forward_lighting_constants, shadow) == 48);

static_assert(sizeof(forward_pass_constants) == 432);
static_assert(offsetof(forward_pass_constants, scene) == 0);
static_assert(offsetof(forward_pass_constants, lighting) == 32);

static_assert(sizeof(taa_constants) == 64);
static_assert(offsetof(taa_constants, size) == 40);
static_assert(offsetof(taa_constants, blend) == 48);
static_assert(offsetof(taa_constants, exposure_buffer) == 56);
static_assert(offsetof(taa_constants, disocclusion_tolerance) == 60);

static_assert(sizeof(exposure_constants) == 48);
static_assert(offsetof(exposure_constants, size) == 8);
static_assert(offsetof(exposure_constants, mode) == 16);
static_assert(offsetof(exposure_constants, frame_time) == 28);
static_assert(offsetof(exposure_constants, maximum_exposure) == 44);

static_assert(sizeof(tonemap_constants) == 32);
static_assert(sizeof(selection_outline_constants) == 32);
static_assert(sizeof(pick_window_constants) == 32);
static_assert(offsetof(tonemap_constants, size) == 16);
static_assert(offsetof(tonemap_constants, tonemapper) == 24);

static_assert(sizeof(debug_blit_constants) == 96);
static_assert(offsetof(debug_blit_constants, origin) == 32);
static_assert(offsetof(debug_blit_constants, extent) == 40);
static_assert(offsetof(debug_blit_constants, source_size) == 48);
static_assert(offsetof(debug_blit_constants, tile_size) == 56);
static_assert(offsetof(debug_blit_constants, range_minimum) == 64);
static_assert(offsetof(debug_blit_constants, texel) == 72);
static_assert(offsetof(debug_blit_constants, output) == 80);
static_assert(sizeof(cluster_statistics_constants) == 16);
static_assert(sizeof(screenshot_constants) == 32);
static_assert(offsetof(screenshot_constants, color) == 8);
static_assert(offsetof(screenshot_constants, weight) == 28);

} // namespace avis::visualizer::rendering::gpu

#endif

#undef AVIS_SHARED_CONSTANT

#endif
