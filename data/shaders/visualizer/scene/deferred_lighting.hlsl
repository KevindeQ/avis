// @avis-shader entry=main stage=cs
//
// Deferred lighting (docs/design/11-renderer.md §6.5, §2.3 pass 8, docs/tasks/11-renderer.md §11.11): one pixel per
// thread in 8 × 8 groups. It reconstructs the world position from the depth buffer, then sums
//   - the sun, with its shadow (all lit until the cascades exist, 11.12);
//   - the point and spot lights of the pixel's cluster;
//   - the image-based ambient light of the baked environment, with ambient occlusion (the material's, and GTAO's once
//     it exists, 11.13) and Lagarde's specular occlusion;
//   - the emissive radiance.
// Unlit pixels pass their base colour through. The result is pre-exposed: hdr_color = radiance · exposure. Pixels with
// nothing drawn (depth 0) are left at 0 for the sky pass.

#include "bindless.hlsli"
#include "brdf.hlsli"
#include "environment.hlsli"
#include "lighting.hlsli"
#include "packing.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(deferred_lighting_constants, constants);
AVIS_VIEW_CONSTANTS(view_constants, view);

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float> depth = ResourceDescriptorHeap[constants.depth];
    RWTexture2D<float4> hdr_color = ResourceDescriptorHeap[constants.hdr_color];

    const int3 texel = int3(thread.xy, 0);
    const float device_depth = depth.Load(texel);
    if (device_depth <= 0.0)
    {
        hdr_color[thread.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    Texture2D<float4> albedo_texture = ResourceDescriptorHeap[constants.albedo];
    Texture2D<float2> normal_texture = ResourceDescriptorHeap[constants.normal];
    Texture2D<float4> material_texture = ResourceDescriptorHeap[constants.material];
    Texture2D<float3> emissive_texture = ResourceDescriptorHeap[constants.emissive];
    Texture2D<float> linear_depth = ResourceDescriptorHeap[constants.linear_depth];

    const float4 albedo = albedo_texture.Load(texel);
    const float4 material = material_texture.Load(texel);
    const float3 emissive = emissive_texture.Load(texel);
    if (material.b > 0.5)
    {
        // Unlit: the base colour, as it is
        hdr_color[thread.xy] = float4(albedo.rgb * constants.exposure, 1.0);
        return;
    }

    // The world position of the pixel centre, through the jittered matrices the depth was drawn with
    const float2 ndc = ((float2(thread.xy) + 0.5) * view.inverse_size * 2.0 - 1.0) * float2(1.0, -1.0);
    const float4 world = mul(view.view_projection_inverse, float4(ndc, device_depth, 1.0));
    const float3 position = world.xyz / world.w;
    const float3 normal = octahedral_decode(normal_texture.Load(texel));
    const float3 to_view = normalize(view.camera_position - position);
    const surface s = make_surface(position, normal, albedo.rgb, material.r, material.g);

    float3 radiance = emissive;

    StructuredBuffer<light_data> lights = ResourceDescriptorHeap[constants.lights];
    if (constants.sun != no_light)
    {
        const float shadow = 1.0; // the cascades come with 11.12
        radiance += evaluate_directional_light(lights[constants.sun], s, to_view, shadow);
    }

    StructuredBuffer<cluster_lights> clusters = ResourceDescriptorHeap[constants.cluster_lights];
    StructuredBuffer<uint> light_indices = ResourceDescriptorHeap[constants.light_indices];
    const cluster_lights cluster =
        clusters[cluster_index(pixel_cluster(thread.xy, constants.tile_size, linear_depth.Load(texel)))];
    for (uint index = 0; index < cluster.count; ++index)
    {
        radiance += evaluate_punctual_light(lights[light_indices[cluster.offset + index]], s, to_view);
    }

    // Image-based ambient light: diffuse AO is the lower of the material's and GTAO's (11 §6.4)
    float ambient_occlusion = albedo.a;
    if (avis_has_descriptor(constants.ambient_occlusion))
    {
        Texture2D<float> gtao = ResourceDescriptorHeap[constants.ambient_occlusion];
        ambient_occlusion = min(ambient_occlusion, gtao.Load(texel));
    }
    environment_lighting environment;
    environment.irradiance = constants.irradiance;
    environment.prefiltered = constants.prefiltered;
    environment.brdf_lut = constants.brdf_lut;
    float3 ambient_diffuse;
    float3 ambient_specular;
    evaluate_image_lighting(environment, s, to_view, ambient_diffuse, ambient_specular);
    const float specular_ao = specular_occlusion(saturate(dot(normal, to_view)), ambient_occlusion, s.roughness);
    radiance += constants.ambient_intensity * (ambient_diffuse * ambient_occlusion + ambient_specular * specular_ao);

    hdr_color[thread.xy] = float4(radiance * constants.exposure, 1.0);
}
