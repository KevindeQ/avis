// @avis-shader entry=pixel_main stage=ps permutations=PICKING,ALPHA_MASK,ALPHA_MASK_PICKING,POINTS,POINTS_PICKING,POINTS_ALPHA_MASK,POINTS_ALPHA_MASK_PICKING
//
// The G-buffer pixel shader (docs/design/11-renderer.md §5, docs/tasks/11-renderer.md §11.7, §11.10): albedo, normal,
// material, emissive and velocity, plus the picking id on click frames (11 §5.1, task 11.19). Permutation parts, in
// this order:
//   POINTS      point topology: discards outside the point's circle
//   ALPHA_MASK  alpha_mode mask: cuts away alpha below alpha_cutoff, as the depth prepass did
//   PICKING     also writes picking_id (SV_Target5)
//
// The targets (11 §5):
//   albedo    RGBA8_UNORM_SRGB  linear base colour (the target encodes it); A the material's ambient occlusion
//   normal    RG16_UNORM        world-space normal, octahedral; normal-mapped when the mesh has tangents
//   material  RGBA8_UNORM       R metallic, G roughness (widened by the Toksvig term where a normal map is filtered,
//                               11 §6.1), B shading model (0 lit, 1 unlit), A material flags / 255
//   emissive  R11G11B10_FLOAT   emissive radiance, before exposure
//   velocity  RG16_FLOAT        current minus previous unjittered UV (y down)

#include "bindless.hlsli"
#include "brdf.hlsli"
#include "packing.hlsli"
#include "scene_material.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"
#include "vertex_pulling.hlsli"

// ---- permutation features ----
#if defined(POINTS) || defined(POINTS_PICKING) || defined(POINTS_ALPHA_MASK) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_POINTS 1
#else
#    define AVIS_POINTS 0
#endif
#if defined(ALPHA_MASK) || defined(ALPHA_MASK_PICKING) || defined(POINTS_ALPHA_MASK) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_ALPHA_MASK 1
#else
#    define AVIS_ALPHA_MASK 0
#endif
#if defined(PICKING) || defined(ALPHA_MASK_PICKING) || defined(POINTS_PICKING) || defined(POINTS_ALPHA_MASK_PICKING)
#    define AVIS_PICKING 1
#else
#    define AVIS_PICKING 0
#endif

AVIS_PASS_CONSTANTS(scene_pass_constants, scene_pass);

struct gbuffer_output
{
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 material : SV_Target2;
    float3 emissive : SV_Target3;
    float2 velocity : SV_Target4;
#if AVIS_PICKING
    uint2 picking : SV_Target5; // R32G32_UINT, the object id
#endif
};

// The screen position of an unjittered clip position, in UV (y down)
float2 clip_to_uv(float4 clip)
{
    return clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
}

gbuffer_output pixel_main(scene_varyings input, bool front_face : SV_IsFrontFace)
{
#if AVIS_POINTS
    if (point_outside_circle(input.quad))
    {
        discard;
    }
#endif
    const material_data material = pull_material(scene_pass, input.material);
    const float4 base_color = material_base_color(material, input.uv, input.color);
#if AVIS_ALPHA_MASK
    if (material_alpha_clipped(material, base_color.a))
    {
        discard;
    }
#endif

    // glTF: metalness in B, roughness in G; occlusion in R, applied with occlusion_strength
    const float4 metallic_roughness = sample_material_texture(
        material.metallic_roughness_texture, material.sampler_index, input.uv, float4(1.0, 1.0, 1.0, 1.0));
    const float occlusion =
        sample_material_texture(material.occlusion_texture, material.sampler_index, input.uv, float4(1.0, 1.0, 1.0, 1.0))
            .r;
    const float3 emissive =
        material.emissive_factor *
        sample_material_texture(material.emissive_texture, material.sampler_index, input.uv, float4(1.0, 1.0, 1.0, 1.0)).rgb;

    float filtered_length = 1.0;
    const float3 normal = shading_normal(input, material, front_face, filtered_length);
    const float roughness = saturate(material.roughness_factor * metallic_roughness.g);

    gbuffer_output output;
    output.albedo = float4(base_color.rgb, 1.0 + material.occlusion_strength * (occlusion - 1.0));
    output.normal = octahedral_encode(normal);
    output.material = float4(saturate(material.metallic_factor * metallic_roughness.b),
                             filtered_length < 1.0 ? toksvig_roughness(roughness, filtered_length) : roughness,
                             material.shading_model == shading_model_unlit ? 1.0 : 0.0,
                             (material.flags & 0xFFu) / 255.0);
    output.emissive = emissive;
    output.velocity = clip_to_uv(input.current_clip) - clip_to_uv(input.previous_clip);
#if AVIS_PICKING
    const instance_data instance = load_instance(scene_pass, input.instance);
    output.picking = uint2(instance.object_id_low, instance.object_id_high);
#endif
    return output;
}
