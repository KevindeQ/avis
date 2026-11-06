#ifndef AVIS_BRDF_HLSLI
#define AVIS_BRDF_HLSLI

// The scene renderer's BRDF (docs/design/11-renderer.md §6.1, docs/tasks/11-renderer.md §11.11): Cook-Torrance with
// GGX D, height-correlated Smith V and Schlick F, with F0 = lerp(0.04, base colour, metallic). Lambert diffuse,
// weighted by (1 - F)(1 - metallic) for lights and by (1 - specular albedo)(1 - metallic) for the image-based term, so
// a white surface under a uniform environment reflects exactly that environment. Roughness is perceptual
// (α = roughness²) and clamped to at least 0.045. The Toksvig term widens the roughness by the normal variance the
// normal map's mips filtered away. Also the importance sampling the environment bake uses.

static const float pi = 3.14159265358979;
static const float minimum_roughness = 0.045;

// The dielectric F0 of metallic-roughness materials
static const float dielectric_f0 = 0.04;

float clamp_roughness(float roughness)
{
    return clamp(roughness, minimum_roughness, 1.0);
}

// GGX / Trowbridge-Reitz normal distribution, for α = roughness²
float distribution_ggx(float n_dot_h, float alpha)
{
    const float alpha2 = alpha * alpha;
    const float f = (n_dot_h * alpha2 - n_dot_h) * n_dot_h + 1.0;
    return alpha2 / (pi * f * f);
}

// The height-correlated Smith visibility term V = G / (4 NoL NoV) (Heitz 2014)
float visibility_smith_ggx_correlated(float n_dot_v, float n_dot_l, float alpha)
{
    const float alpha2 = alpha * alpha;
    const float ggx_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    const float ggx_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    return 0.5 / max(ggx_v + ggx_l, 1e-7);
}

float3 fresnel_schlick(float3 f0, float v_dot_h)
{
    const float f = pow(1.0 - saturate(v_dot_h), 5.0);
    return f0 + (1.0 - f0) * f;
}

// A G-buffer surface, ready to shade
struct surface
{
    float3 position; // world space
    float3 normal; // world space, unit length
    float3 base_color; // linear
    float metallic;
    float roughness; // perceptual, clamped
    float3 f0;
};

surface make_surface(float3 position, float3 normal, float3 base_color, float metallic, float roughness)
{
    surface result;
    result.position = position;
    result.normal = normal;
    result.base_color = base_color;
    result.metallic = metallic;
    result.roughness = clamp_roughness(roughness);
    result.f0 = lerp(float3(dielectric_f0, dielectric_f0, dielectric_f0), base_color, metallic);
    return result;
}

// The BRDF times NoL for light arriving from `to_light` (unit length), seen from `to_view` (unit length): multiply by
// the light's illuminance at the surface (perpendicular to the light) to get the reflected radiance
float3 evaluate_brdf(surface s, float3 to_view, float3 to_light)
{
    const float n_dot_l = saturate(dot(s.normal, to_light));
    if (n_dot_l <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float n_dot_v = max(dot(s.normal, to_view), 1e-4);
    const float3 half_vector = normalize(to_view + to_light);
    const float n_dot_h = saturate(dot(s.normal, half_vector));
    const float v_dot_h = saturate(dot(to_view, half_vector));

    const float alpha = s.roughness * s.roughness;
    const float3 fresnel = fresnel_schlick(s.f0, v_dot_h);
    const float3 specular =
        distribution_ggx(n_dot_h, alpha) * visibility_smith_ggx_correlated(n_dot_v, n_dot_l, alpha) * fresnel;
    const float3 diffuse = (1.0 - fresnel) * (1.0 - s.metallic) * s.base_color / pi;
    return (diffuse + specular) * n_dot_l;
}

// The Toksvig roughness (Toksvig 2005, as a roughness offset): a mip-filtered normal map texel's length is below 1 where
// the normals it averages diverge; that variance widens the lobe, so a distant bumpy surface doesn't sparkle
float toksvig_roughness(float roughness, float normal_length)
{
    const float filtered_length = clamp(normal_length, 1e-4, 1.0);
    const float variance = (1.0 - filtered_length) / filtered_length;
    const float alpha = roughness * roughness;
    const float alpha2 = min(alpha * alpha + variance, 1.0);
    return sqrt(sqrt(alpha2));
}

// Specular occlusion from an ambient occlusion term (Lagarde and de Rousiers 2014)
float specular_occlusion(float n_dot_v, float ambient_occlusion, float roughness)
{
    return saturate(pow(n_dot_v + ambient_occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + ambient_occlusion);
}

// ---- importance sampling ----

// Point `index` of the `count`-point Hammersley set in [0, 1)²
float2 hammersley(uint index, uint count)
{
    return float2((float(index) + 0.5) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}

// A half vector around `normal` distributed by GGX(α) · NoH, from a uniform sample
float3 importance_sample_ggx(float2 uniform_sample, float alpha, float3 normal)
{
    const float phi = 2.0 * pi * uniform_sample.x;
    const float cos_theta = sqrt((1.0 - uniform_sample.y) / (1.0 + (alpha * alpha - 1.0) * uniform_sample.y));
    const float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
    const float3 local = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return tangent * local.x + bitangent * local.y + normal * local.z;
}

#endif
