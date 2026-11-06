// @avis-shader entry=sky_main stage=cs entry=prefilter_main stage=cs entry=irradiance_main stage=cs entry=brdf_lut_main stage=cs
//
// The environment bake (docs/design/11-renderer.md §6.4, docs/tasks/11-renderer.md §11.11), run by
// environment_baker.cpp when the snapshot's environment changes:
//   sky_main         the analytic sky (sky.hlsli) into the 256² sky cubemap, one face per dispatch z
//   prefilter_main   one mip of the GGX-prefiltered cubemap from the sky cubemap: mip m is roughness m / 5, with N = V = R
//   irradiance_main  the sky cubemap projected onto SH9 and convolved with the cosine lobe, in one group of 256 threads
// and, once at startup:
//   brdf_lut_main    the split-sum BRDF LUT (Karis 2013) with the height-correlated Smith term: (A, B) such that the
//                    directional specular albedo is F0·A + B, at NoV = (i + 0.5) / 128, roughness = (j + 0.5) / 128

#include "bindless.hlsli"
#include "brdf.hlsli"
#include "environment.hlsli"
#include "shared_types.hlsli"
#include "sky.hlsli"

// ---- sky ----

AVIS_PASS_CONSTANTS(sky_constants, sky);

// The draw constants of every entry point (environment_baker.cpp's bake_constants)
struct bake_constants
{
    uint source; // prefilter, irradiance: SRV of the sky cubemap
    uint destination; // sky, prefilter: UAV of a cubemap mip as a 6-slice array; irradiance: RWStructuredBuffer<float4>
                      // of environment_irradiance_coefficients; BRDF LUT: UAV, RG16F
    uint size; // texels per face (per side for the LUT); irradiance: samples per face side
    uint sample_count; // prefilter, BRDF LUT
    float roughness; // prefilter: the mip's roughness
    uint padding0;
    uint padding1;
    uint padding2;
};

AVIS_DRAW_CONSTANTS(bake_constants, bake);

[numthreads(8, 8, 1)] void sky_main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= bake.size))
    {
        return;
    }
    RWTexture2DArray<float4> destination = ResourceDescriptorHeap[bake.destination];
    const float3 direction = cube_direction(thread.xy, thread.z, bake.size);
    destination[thread] = float4(sky_radiance(sky, direction), 1.0);
}

// ---- prefiltered specular ----

[numthreads(8, 8, 1)] void prefilter_main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= bake.size))
    {
        return;
    }
    TextureCube<float4> source = ResourceDescriptorHeap[bake.source];
    RWTexture2DArray<float4> destination = ResourceDescriptorHeap[bake.destination];

    const float3 normal = cube_direction(thread.xy, thread.z, bake.size);
    if (bake.roughness <= 0.0)
    {
        destination[thread] = float4(source.SampleLevel(linear_clamp_sampler, normal, 0.0).rgb, 1.0);
        return;
    }

    const float alpha = bake.roughness * bake.roughness;
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    for (uint index = 0; index < bake.sample_count; ++index)
    {
        const float3 half_vector = importance_sample_ggx(hammersley(index, bake.sample_count), alpha, normal);
        const float3 to_light = 2.0 * dot(normal, half_vector) * half_vector - normal;
        const float n_dot_l = dot(normal, to_light);
        if (n_dot_l > 0.0)
        {
            sum += source.SampleLevel(linear_clamp_sampler, to_light, 0.0).rgb * n_dot_l;
            weight += n_dot_l;
        }
    }
    destination[thread] = float4(sum / max(weight, 1e-6), 1.0);
}

// ---- SH9 irradiance ----

static const uint irradiance_threads = 256;
groupshared float4 partial_sums[irradiance_threads];

[numthreads(irradiance_threads, 1, 1)] void irradiance_main(uint3 thread : SV_GroupThreadID)
{
    TextureCube<float4> source = ResourceDescriptorHeap[bake.source];
    RWStructuredBuffer<float4> destination = ResourceDescriptorHeap[bake.destination];

    const uint side = bake.size;
    const uint per_face = side * side;
    float3 coefficients[9];
    [unroll] for (uint clear = 0; clear < 9; ++clear)
    {
        coefficients[clear] = float3(0.0, 0.0, 0.0);
    }
    float weight = 0.0;
    for (uint sample_index = thread.x; sample_index < 6 * per_face; sample_index += irradiance_threads)
    {
        const uint face = sample_index / per_face;
        const uint2 texel = uint2(sample_index % side, (sample_index % per_face) / side);
        const float2 uv = (float2(texel) + 0.5) / float(side) * 2.0 - 1.0;
        const float3 direction = cube_direction(texel, face, side);
        const float texel_weight = cube_texel_weight(uv);
        const float3 radiance = source.SampleLevel(linear_clamp_sampler, direction, 0.0).rgb;
        float basis[9];
        sh9_basis(direction, basis);
        [unroll] for (uint index = 0; index < 9; ++index)
        {
            coefficients[index] += radiance * (basis[index] * texel_weight);
        }
        weight += texel_weight;
    }

    // One coefficient at a time through group memory: the w channel carries the total weight
    for (uint coefficient = 0; coefficient < 9; ++coefficient)
    {
        partial_sums[thread.x] = float4(coefficients[coefficient], weight);
        GroupMemoryBarrierWithGroupSync();
        for (uint stride = irradiance_threads / 2; stride > 0; stride /= 2)
        {
            if (thread.x < stride)
            {
                partial_sums[thread.x] += partial_sums[thread.x + stride];
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (thread.x == 0)
        {
            // The weights sum to the sphere's 4π: normalising by them makes a uniform sky exact
            const float4 total = partial_sums[0];
            const uint band = coefficient == 0 ? 0 : (coefficient < 4 ? 1 : 2);
            destination[coefficient] = float4(total.rgb * (4.0 * pi / total.w) * sh9_cosine_band[band], 0.0);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// ---- BRDF LUT ----

[numthreads(8, 8, 1)] void brdf_lut_main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= bake.size))
    {
        return;
    }
    RWTexture2D<float2> destination = ResourceDescriptorHeap[bake.destination];

    const float n_dot_v = (float(thread.x) + 0.5) / float(bake.size);
    const float roughness = clamp_roughness((float(thread.y) + 0.5) / float(bake.size));
    const float alpha = roughness * roughness;
    const float3 to_view = float3(sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v);
    const float3 normal = float3(0.0, 0.0, 1.0);

    float2 sum = float2(0.0, 0.0);
    for (uint index = 0; index < bake.sample_count; ++index)
    {
        const float3 half_vector = importance_sample_ggx(hammersley(index, bake.sample_count), alpha, normal);
        const float3 to_light = 2.0 * dot(to_view, half_vector) * half_vector - to_view;
        const float n_dot_l = saturate(to_light.z);
        const float n_dot_h = saturate(half_vector.z);
        const float v_dot_h = saturate(dot(to_view, half_vector));
        if (n_dot_l > 0.0 && n_dot_h > 0.0)
        {
            // The sample's weight once the GGX pdf D·NoH / (4 VoH) cancels: V · 4 · NoL · VoH / NoH
            const float weighted = visibility_smith_ggx_correlated(n_dot_v, n_dot_l, alpha) * 4.0 * n_dot_l * v_dot_h /
                                   n_dot_h;
            const float fresnel = pow(1.0 - v_dot_h, 5.0);
            sum += float2((1.0 - fresnel) * weighted, fresnel * weighted);
        }
    }
    destination[thread.xy] = sum / float(bake.sample_count);
}
