#ifndef AVIS_SCENE_MATERIAL_HLSLI
#define AVIS_SCENE_MATERIAL_HLSLI

// What every scene pixel shader evaluates of a material (docs/design/11-renderer.md §3.3, §9, docs/tasks/11-renderer.md
// §11.10): its textures, the base colour and alpha, the alpha test of mask materials, the circle of a point and the
// edge coverage of a line. Texture fields are bindless SRV indices; 0 means "no texture", which samples as the
// field's neutral value, so the factors alone remain.

#include "bindless.hlsli"
#include "scene_varyings.hlsli"
#include "shared_types.hlsli"

// The texture at `texture` with the material's static sampler (binding_layout::static_sampler), or `neutral`
float4 sample_material_texture(uint texture, uint sampler_index, float2 uv, float4 neutral)
{
    if (!avis_has_descriptor(texture))
    {
        return neutral;
    }
    Texture2D<float4> source = ResourceDescriptorHeap[texture];
    switch (sampler_index)
    {
    case 0:
        return source.Sample(point_clamp_sampler, uv);
    case 1:
        return source.Sample(linear_clamp_sampler, uv);
    case 2:
        return source.Sample(linear_wrap_sampler, uv);
    case 5:
        return source.Sample(point_wrap_sampler, uv);
    default:
        return source.Sample(anisotropic_wrap_sampler, uv);
    }
}

// Linear base colour and alpha: the factor times the texture, times the vertex colour with use_vertex_color
float4 material_base_color(material_data material, float2 uv, float4 vertex_color)
{
    float4 color = material.base_color_factor *
                   sample_material_texture(material.base_color_texture, material.sampler_index, uv, float4(1.0, 1.0, 1.0, 1.0));
    if ((material.flags & material_flag_use_vertex_color) != 0)
    {
        color *= vertex_color;
    }
    return color;
}

// alpha_mode mask: whether the pixel is cut away (alpha below alpha_cutoff, glTF 2.0)
bool material_alpha_clipped(material_data material, float alpha)
{
    return material.alpha_mode == alpha_mode_mask && alpha < material.alpha_cutoff;
}

// The shading normal: the interpolated one (flipped on back faces, which only double-sided materials draw), perturbed
// by the normal texture when the mesh has tangents. `filtered_length` is the length of the mip-filtered normal texel,
// below 1 where the normals it averages diverge (1 without a normal texture), for the Toksvig term.
float3 shading_normal(scene_varyings input, material_data material, bool front_face, out float filtered_length)
{
    filtered_length = 1.0;
    float3 normal = normalize(input.normal);
    if (!front_face)
    {
        normal = -normal;
    }
    if (input.tangent.w != 0.0 && avis_has_descriptor(material.normal_texture))
    {
        float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz));
        const float3 bitangent = cross(normal, tangent) * input.tangent.w;
        float3 texel =
            sample_material_texture(material.normal_texture, material.sampler_index, input.uv, float4(0.5, 0.5, 1.0, 1.0))
                .xyz *
                2.0 -
            1.0;
        filtered_length = length(texel);
        texel.xy *= material.normal_scale;
        normal = normalize(texel.x * tangent + texel.y * bitangent + texel.z * normal);
    }
    return normal;
}

// Points (11 §9.1): whether a position in the point's quad lies outside its inscribed circle
bool point_outside_circle(float2 quad)
{
    return dot(quad, quad) > 1.0;
}

// Lines (11 §9.2): the analytic coverage of a pixel whose centre is `across_pixels` from the centre line, for a line of
// `width_pixels`: 1 inside, falling linearly to 0 over one pixel around the edge
float line_coverage(float across_pixels, float width_pixels)
{
    return saturate(0.5 * width_pixels + 0.5 - abs(across_pixels));
}

#endif
