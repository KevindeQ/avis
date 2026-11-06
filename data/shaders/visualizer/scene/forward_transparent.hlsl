// @avis-shader entry=pixel_main stage=ps permutations=PICKING,UNLIT,UNLIT_PICKING,LINES,LINES_PICKING,LINES_UNLIT,LINES_UNLIT_PICKING,POINTS,POINTS_PICKING,POINTS_UNLIT,POINTS_UNLIT_PICKING
//
// The forward pixel shader of translucent world geometry (docs/design/11-renderer.md §10.1, §2.3 pass 10,
// docs/tasks/11-renderer.md §11.7, §11.14). Permutation parts, in this order:
//   LINES, POINTS  the topology (line coverage falloff, point circles); none for triangles
//   UNLIT          shading_model unlit
//   PICKING        also writes picking_id (SV_Target2)
//
// Targets:
//   SV_Target0  hdr_color, RGBA16F: pre-exposed radiance, premultiplied by alpha (base colour alpha × line or point
//               coverage). The PSO blends it by the material's blend_operation: normal src + (1 − a)·dst, additive
//               dst + src, subtractive dst − src (pipeline_variant_registry.cpp).
//   SV_Target1  reactive_mask, R8: how much this surface changes the pixel, for TAA (11.15), kept as the maximum over
//               the pixel's transparents. Normal blending: its alpha, the fraction of the background it replaces (how
//               much the luminance changes would need the background, which a blended target can't read). Additive
//               and subtractive: the luminance it adds or takes away, pre-exposed, which is the change exactly.
//   SV_Target2  picking_id, R32G32_UINT, the object id (11 §5.1; 11.19)
//
// Lit materials are shaded like deferred_lighting: the sun with its cascaded shadow, the point and spot lights of the
// pixel's cluster, the image-based ambient light and the emissive radiance, with the material's own occlusion and
// Lagarde's specular occlusion from it; GTAO doesn't apply (11 §10.1). The whole radiance is scaled by alpha, so a
// transparent is a partially covering surface (glTF's blend mode).

#include "bindless.hlsli"
#include "brdf.hlsli"
#include "environment.hlsli"
#include "lighting.hlsli"
#include "scene_material.hlsli"
#include "scene_varyings.hlsli"
#include "shadows.hlsli"
#include "shared_types.hlsli"
#include "vertex_pulling.hlsli"

AVIS_PASS_CONSTANTS(forward_pass_constants, forward);
AVIS_VIEW_CONSTANTS(view_constants, view);

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

float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

#if !AVIS_UNLIT
// The radiance a lit transparent reflects towards the camera at `pixel`, before exposure and alpha
float3 shade(scene_varyings input, material_data material, float3 base_color, bool front_face, float2 pixel)
{
    const forward_lighting_constants lighting = forward.lighting;
    const float4 metallic_roughness = sample_material_texture(
        material.metallic_roughness_texture, material.sampler_index, input.uv, float4(1.0, 1.0, 1.0, 1.0));
    const float occlusion_texel =
        sample_material_texture(material.occlusion_texture, material.sampler_index, input.uv, float4(1.0, 1.0, 1.0, 1.0))
            .r;
    const float3 emissive =
        material.emissive_factor *
        sample_material_texture(material.emissive_texture, material.sampler_index, input.uv, float4(1.0, 1.0, 1.0, 1.0)).rgb;

    float filtered_length = 1.0;
    const float3 normal = shading_normal(input, material, front_face, filtered_length);
    const float base_roughness = saturate(material.roughness_factor * metallic_roughness.g);
    const float roughness =
        filtered_length < 1.0 ? toksvig_roughness(base_roughness, filtered_length) : base_roughness;
    const float metallic = saturate(material.metallic_factor * metallic_roughness.b);
    const float occlusion = 1.0 + material.occlusion_strength * (occlusion_texel - 1.0);

    const float3 position = input.world_position;
    const float3 to_view = normalize(view.camera_position - position);
    const surface s = make_surface(position, normal, base_color, metallic, roughness);
    const float view_depth = -mul(view.view, float4(position, 1.0)).z;

    float3 radiance = emissive;
    StructuredBuffer<light_data> lights = ResourceDescriptorHeap[lighting.lights];
    if (lighting.sun != no_light)
    {
        const light_data sun = lights[lighting.sun];
        const float n_dot_l = dot(normal, -sun.direction);
        float shadow = 1.0;
        if (sun.casts_shadows != 0 && n_dot_l > 0.0)
        {
            uint cascade = no_cascade;
            shadow = sun_shadow(lighting.shadow, view_depth, position, normal, n_dot_l, pixel, view.frame_index, cascade);
        }
        radiance += evaluate_directional_light(sun, s, to_view, shadow);
    }

    StructuredBuffer<cluster_lights> clusters = ResourceDescriptorHeap[lighting.cluster_lights];
    StructuredBuffer<uint> light_indices = ResourceDescriptorHeap[lighting.light_indices];
    const cluster_lights cluster = clusters[cluster_index(pixel_cluster(uint2(pixel), lighting.tile_size, view_depth))];
    for (uint index = 0; index < cluster.count; ++index)
    {
        radiance += evaluate_punctual_light(lights[light_indices[cluster.offset + index]], s, to_view);
    }

    environment_lighting environment;
    environment.irradiance = lighting.irradiance;
    environment.prefiltered = lighting.prefiltered;
    environment.brdf_lut = lighting.brdf_lut;
    float3 ambient_diffuse;
    float3 ambient_specular;
    evaluate_image_lighting(environment, s, to_view, ambient_diffuse, ambient_specular);
    const float specular_ao = specular_occlusion(saturate(dot(normal, to_view)), occlusion, s.roughness);
    radiance += lighting.ambient_intensity * (ambient_diffuse * occlusion + ambient_specular * specular_ao);
    return radiance;
}
#endif

forward_output pixel_main(scene_varyings input, bool front_face : SV_IsFrontFace)
{
#if AVIS_POINTS
    if (point_outside_circle(input.quad))
    {
        discard;
    }
#endif
    const material_data material = pull_material(forward.scene, input.material);
    float coverage = 1.0;
#if AVIS_LINES
    // The analytic edge falloff of 11 §9.2: quad.y is the distance from the centre line in pixels
    coverage = line_coverage(input.quad.y, material.line_width * view.device_pixel_ratio);
#endif
    const float4 base_color = material_base_color(material, input.uv, input.color);
    const float alpha = saturate(base_color.a) * coverage;
#if AVIS_UNLIT
    const float3 radiance = base_color.rgb;
#else
    const float3 radiance = shade(input, material, base_color.rgb, front_face, input.position.xy);
#endif

    forward_output output = (forward_output)0;
    output.color = float4(radiance * (forward.lighting.exposure * alpha), alpha);
    output.reactive =
        saturate(material.blend_operation == blend_operation_normal ? alpha : luminance(output.color.rgb));
#if AVIS_PICKING
    const instance_data instance = load_instance(forward.scene, input.instance);
    output.picking = uint2(instance.object_id_low, instance.object_id_high);
#endif
    return output;
}
