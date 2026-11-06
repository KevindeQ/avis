#ifndef AVIS_SHADOWS_HLSLI
#define AVIS_SHADOWS_HLSLI

// The receiver side of the sun's cascaded shadow maps (docs/design/11-renderer.md §7, docs/tasks/11-renderer.md
// §11.12), for deferred_lighting and, later, forward_transparent:
//   - the cascade is the first whose split_far is beyond the pixel's view depth; past the last one, the pixel is lit;
//   - the position moves along its normal by normal_offset texels of the cascade, times sin of the normal-sun angle, so
//     surfaces facing the sun need no offset and grazing ones get the most (against acne, with the PSO's slope bias);
//   - 16-tap Poisson-disk PCF with the comparison sampler (s4, GREATER_EQUAL: reverse-Z, lit where the receiver is at
//     least as near the sun as the caster), rotated per pixel by interleaved gradient noise, which TAA resolves;
//   - over the last part of a cascade (blend_start to split_far) the result blends into the next cascade's, and the
//     last cascade's into "lit".

#include "bindless.hlsli"
#include "shared_types.hlsli"

static const float2 shadow_poisson_disk[16] = { float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
                                                float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
                                                float2(-0.91588581, 0.45771432),  float2(-0.81544232, -0.87912464),
                                                float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),
                                                float2(0.44323325, -0.97511554),  float2(0.53742981, -0.47373420),
                                                float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
                                                float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590),
                                                float2(0.19984126, 0.78641367),   float2(0.14383161, -0.14100790) };

// Jimenez 2014's interleaved gradient noise in [0, 1), moved each frame so TAA averages the rotations
float interleaved_gradient_noise(float2 pixel, uint frame_index)
{
    pixel += 5.588238 * float(frame_index % 64);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

shadow_cascade_data shadow_cascade_at(shadow_constants shadow, uint index)
{
    switch (index)
    {
    case 0:
        return shadow.cascade0;
    case 1:
        return shadow.cascade1;
    case 2:
        return shadow.cascade2;
    default:
        return shadow.cascade3;
    }
}

// The lit fraction of `position` in cascade `index`
float sample_shadow_cascade(
    shadow_constants shadow, uint index, float3 position, float3 normal, float n_dot_l, float rotation)
{
    const shadow_cascade_data cascade = shadow_cascade_at(shadow, index);
    const float sin_angle = sqrt(saturate(1.0 - n_dot_l * n_dot_l));
    const float3 offset = position + normal * (shadow.normal_offset * cascade.texel_size * sin_angle);
    const float4 clip = mul(cascade.shadow_from_world, float4(offset, 1.0));
    const float2 uv = clip.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0; // outside the cascade: the fitting keeps receivers inside, so only at its very edge
    }
    // Receivers nearer the sun than depth 1 meet pancaked casters at 1: lit
    const float depth = saturate(clip.z);

    Texture2DArray<float> map = ResourceDescriptorHeap[shadow.shadow_map];
    float sine;
    float cosine;
    sincos(rotation * 6.28318530718, sine, cosine);
    const float2x2 rotate = float2x2(cosine, -sine, sine, cosine);
    const float radius = shadow.filter_radius / shadow.resolution;
    float lit = 0.0;
    [unroll] for (uint tap = 0; tap < 16; ++tap)
    {
        const float2 at = uv + mul(rotate, shadow_poisson_disk[tap]) * radius;
        lit += map.SampleCmpLevelZero(shadow_comparison_sampler, float3(at, float(index)), depth);
    }
    return lit / 16.0;
}

// The sun's lit fraction at a pixel: `view_depth` is its linear depth (metres), `position` and `normal` world space,
// `n_dot_l` the cosine towards the sun. `cascade_index` is the cascade used, or no_cascade.
float sun_shadow(
    shadow_constants shadow,
    float view_depth,
    float3 position,
    float3 normal,
    float n_dot_l,
    float2 pixel,
    uint frame_index,
    out uint cascade_index)
{
    cascade_index = no_cascade;
    if (!avis_has_descriptor(shadow.shadow_map) || shadow.cascade_count == 0)
    {
        return 1.0;
    }
    uint index = 0;
    while (index < shadow.cascade_count && view_depth >= shadow_cascade_at(shadow, index).split_far)
    {
        ++index;
    }
    if (index == shadow.cascade_count)
    {
        return 1.0; // beyond the shadow distance
    }
    cascade_index = index;

    const float rotation = interleaved_gradient_noise(pixel, frame_index);
    float lit = sample_shadow_cascade(shadow, index, position, normal, n_dot_l, rotation);
    const shadow_cascade_data cascade = shadow_cascade_at(shadow, index);
    if (view_depth > cascade.blend_start)
    {
        const float blend = saturate((view_depth - cascade.blend_start) / (cascade.split_far - cascade.blend_start));
        float next = 1.0;
        if (index + 1 < shadow.cascade_count)
        {
            next = sample_shadow_cascade(shadow, index + 1, position, normal, n_dot_l, rotation);
        }
        lit = lerp(lit, next, blend);
    }
    return lit;
}

#endif
