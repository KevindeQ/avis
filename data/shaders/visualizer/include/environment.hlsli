#ifndef AVIS_ENVIRONMENT_HLSLI
#define AVIS_ENVIRONMENT_HLSLI

// The baked environment (docs/design/11-renderer.md §6.4, docs/tasks/11-renderer.md §11.11): cubemap texel
// directions, the SH9 irradiance basis, and the image-based lighting that deferred lighting (and later the forward
// pass) adds: irradiance(N) · albedo · diffuse weight + prefiltered(R, roughness) · (F0·A + B).

#include "bindless.hlsli"
#include "brdf.hlsli"
#include "shared_types.hlsli"

// The direction through texel `texel` of cube face `face` (D3D order +X, -X, +Y, -Y, +Z, -Z) of a `size`² face,
// at the texel's centre; unit length
float3 cube_direction(uint2 texel, uint face, uint size)
{
    const float2 uv = (float2(texel) + 0.5) / float(size) * 2.0 - 1.0; // u right, v down
    float3 direction;
    switch (face)
    {
    case 0:
        direction = float3(1.0, -uv.y, -uv.x);
        break;
    case 1:
        direction = float3(-1.0, -uv.y, uv.x);
        break;
    case 2:
        direction = float3(uv.x, 1.0, uv.y);
        break;
    case 3:
        direction = float3(uv.x, -1.0, -uv.y);
        break;
    case 4:
        direction = float3(uv.x, -uv.y, 1.0);
        break;
    default:
        direction = float3(-uv.x, -uv.y, -1.0);
        break;
    }
    return normalize(direction);
}

// The solid angle of a cube texel at face coordinates uv ∈ [-1, 1]², relative to its texel area
float cube_texel_weight(float2 uv)
{
    const float r2 = 1.0 + dot(uv, uv);
    return 1.0 / (r2 * sqrt(r2));
}

// The nine real SH basis functions at unit direction n (bands 0 to 2)
void sh9_basis(float3 n, out float basis[9])
{
    basis[0] = 0.282095;
    basis[1] = 0.488603 * n.y;
    basis[2] = 0.488603 * n.z;
    basis[3] = 0.488603 * n.x;
    basis[4] = 1.092548 * n.x * n.y;
    basis[5] = 1.092548 * n.y * n.z;
    basis[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    basis[7] = 1.092548 * n.x * n.z;
    basis[8] = 0.546274 * (n.x * n.x - n.y * n.y);
}

// The cosine lobe's convolution factor per band: the coefficients the bake stores are the radiance's times these, so
// their sum is the irradiance
static const float sh9_cosine_band[3] = { pi, 2.0 * pi / 3.0, pi / 4.0 };

// The irradiance (lux) arriving at a surface of normal `n` from the baked SH9 coefficients
float3 sh9_irradiance(StructuredBuffer<float4> coefficients, float3 n)
{
    float basis[9];
    sh9_basis(n, basis);
    float3 irradiance = float3(0.0, 0.0, 0.0);
    [unroll] for (uint index = 0; index < 9; ++index)
    {
        irradiance += coefficients[index].rgb * basis[index];
    }
    return max(irradiance, 0.0);
}

// What the image-based lighting needs of the baked environment
struct environment_lighting
{
    uint irradiance; // StructuredBuffer<float4>
    uint prefiltered; // TextureCube
    uint brdf_lut; // Texture2D, RG16F: (A, B) of the split sum at (NoV, roughness)
};

// The ambient radiance a surface reflects towards `to_view`, before occlusion: the diffuse and the specular parts
void evaluate_image_lighting(environment_lighting environment, surface s, float3 to_view, out float3 diffuse,
                             out float3 specular)
{
    StructuredBuffer<float4> irradiance = ResourceDescriptorHeap[environment.irradiance];
    TextureCube<float4> prefiltered = ResourceDescriptorHeap[environment.prefiltered];
    Texture2D<float2> brdf_lut = ResourceDescriptorHeap[environment.brdf_lut];

    const float n_dot_v = saturate(dot(s.normal, to_view));
    const float2 split_sum = brdf_lut.SampleLevel(linear_clamp_sampler, float2(n_dot_v, s.roughness), 0.0);
    const float3 specular_albedo = s.f0 * split_sum.x + split_sum.y;

    const float3 reflected = reflect(-to_view, s.normal);
    const float lod = s.roughness * float(environment_prefiltered_mips - 1);
    specular = prefiltered.SampleLevel(linear_clamp_sampler, reflected, lod).rgb * specular_albedo;
    diffuse = sh9_irradiance(irradiance, s.normal) / pi * s.base_color * (1.0 - specular_albedo) * (1.0 - s.metallic);
}

#endif
