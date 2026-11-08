// @avis-shader entry=main stage=cs
//
// Deferred lighting (docs/design/11-renderer.md §6.5, §2.3 pass 8, docs/tasks/11-renderer.md §11.11): one pixel per
// thread in 8 × 8 groups. It reconstructs the world position from the depth buffer, then sums
//   - the sun, with its shadow from the cascaded shadow maps (shadows.hlsli, 11.12);
//   - the point and spot lights of the pixel's cluster;
//   - the image-based ambient light of the baked environment, with ambient occlusion (the lower of the material's and
//     GTAO's, 11.13) and Lagarde's specular occlusion from it;
//   - the emissive radiance.
// Unlit pixels pass their base colour through. The result is pre-exposed: hdr_color = radiance · exposure, with the
// frame's exposure from the view's exposure buffer (exposure.hlsli, 11.16). Pixels with
// nothing drawn (depth 0) are left at 0 for the sky pass. When constants.cascade_index names a UAV, each pixel's shadow
// cascade goes there (no_cascade where none applies), for the debug blit (11.20) and the tests. A half-resolution GTAO
// result is upsampled here, with bilinear weights scaled by depth similarity (11 §8).

#include "bindless.hlsli"
#include "brdf.hlsli"
#include "environment.hlsli"
#include "exposure.hlsli"
#include "lighting.hlsli"
#include "packing.hlsli"
#include "shadows.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(deferred_lighting_constants, constants);
AVIS_VIEW_CONSTANTS(view_constants, view);

void write_cascade_index(uint2 pixel, uint index)
{
    if (avis_has_descriptor(constants.cascade_index))
    {
        RWTexture2D<uint> cascade_index = ResourceDescriptorHeap[constants.cascade_index];
        cascade_index[pixel] = index;
    }
}

// GTAO's ambient occlusion at `pixel`, whose linear depth is `view_depth`. At half resolution, AO texel t is centred
// on pixel t · scale: the four texels around the pixel are weighted bilinearly and by how close their depth is to the
// pixel's, so occlusion doesn't bleed across depth edges. Without any similar texel, the closest in depth wins.
float gtao_occlusion(uint2 pixel, float view_depth)
{
    Texture2D<float> gtao = ResourceDescriptorHeap[constants.ambient_occlusion];
    const uint scale = constants.ambient_occlusion_scale;
    if (scale <= 1)
    {
        return gtao.Load(int3(pixel, 0));
    }
    Texture2D<float> linear_depth = ResourceDescriptorHeap[constants.linear_depth];
    // The AO texture's size, rounded up as gtao_extent does. Not from GetDimensions: WARP faults on it in the -O3 build
    // (an access violation in its shader code, then a removed device).
    const uint2 ao_size = (constants.size + scale - 1) / scale;
    const int2 last = int2(ao_size) - 1;
    const int2 last_pixel = int2(constants.size) - 1;
    const float2 position = float2(pixel) / float(scale);
    const int2 base = int2(floor(position));
    const float2 fraction = position - float2(base);

    float sum = 0.0;
    float total = 0.0;
    float closest = 0.0;
    float closest_difference = 1.0e30;
    [unroll] for (uint corner = 0; corner < 4; ++corner)
    {
        const int2 offset = int2(corner & 1, corner >> 1);
        const int2 texel = min(base + offset, last);
        const float bilinear =
            (offset.x != 0 ? fraction.x : 1.0 - fraction.x) * (offset.y != 0 ? fraction.y : 1.0 - fraction.y);
        const float depth = linear_depth.Load(int3(min(texel * int(scale), last_pixel), 0));
        const float difference = abs(depth - view_depth);
        const float value = gtao.Load(int3(texel, 0));
        const float weight = bilinear * saturate(1.0 - difference / (0.05 * view_depth));
        sum += weight * value;
        total += weight;
        if (difference < closest_difference)
        {
            closest_difference = difference;
            closest = value;
        }
    }
    return total > 1.0e-4 ? sum / total : closest;
}

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
        write_cascade_index(thread.xy, no_cascade);
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
        hdr_color[thread.xy] = float4(albedo.rgb * load_current_exposure(constants.exposure_buffer), 1.0);
        write_cascade_index(thread.xy, no_cascade);
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
    const float view_depth = linear_depth.Load(texel);

    StructuredBuffer<light_data> lights = ResourceDescriptorHeap[constants.lights];
    uint cascade = no_cascade;
    if (constants.sun != no_light)
    {
        const light_data sun = lights[constants.sun];
        const float n_dot_l = dot(normal, -sun.direction);
        float shadow = 1.0;
        if (sun.casts_shadows != 0 && n_dot_l > 0.0)
        {
            shadow = sun_shadow(
                constants.shadow, view_depth, position, normal, n_dot_l, float2(thread.xy), view.frame_index, cascade);
        }
        radiance += evaluate_directional_light(sun, s, to_view, shadow);
    }
    write_cascade_index(thread.xy, cascade);

    StructuredBuffer<cluster_lights> clusters = ResourceDescriptorHeap[constants.cluster_lights];
    StructuredBuffer<uint> light_indices = ResourceDescriptorHeap[constants.light_indices];
    const cluster_lights cluster = clusters[cluster_index(pixel_cluster(thread.xy, constants.tile_size, view_depth))];
    for (uint index = 0; index < cluster.count; ++index)
    {
        radiance += evaluate_punctual_light(lights[light_indices[cluster.offset + index]], s, to_view);
    }

    // Image-based ambient light: diffuse AO is the lower of the material's and GTAO's (11 §6.4)
    float ambient_occlusion = albedo.a;
    if (avis_has_descriptor(constants.ambient_occlusion))
    {
        ambient_occlusion = min(ambient_occlusion, gtao_occlusion(thread.xy, view_depth));
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

    hdr_color[thread.xy] = float4(radiance * load_current_exposure(constants.exposure_buffer), 1.0);
}
