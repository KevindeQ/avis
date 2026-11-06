#ifndef AVIS_SKY_HLSLI
#define AVIS_SKY_HLSLI

// The analytic sky (docs/design/11-renderer.md §6.4, docs/tasks/11-renderer.md §11.11): the radiance seen in a world
// direction, from the sky_constants that sky_model.cpp computes on the CPU. Both the environment bake and the sky pass
// evaluate it, so the baked lighting and the visible sky agree. sky_model.cpp's sky_radiance mirrors this function for
// the tests.

#include "shared_types.hlsli"

// The Perez function of each channel (Y, x, y) at view zenith angle θ (as cos θ) and angle γ to the sun
float3 sky_perez(sky_constants sky, float cos_theta, float gamma, float cos_gamma)
{
    const float clamped = max(cos_theta, 1e-3); // at the horizon exp(B / cos θ) → 0, since B < 0
    return (1.0 + sky.perez_a * exp(sky.perez_b / clamped)) *
           (1.0 + sky.perez_c * exp(sky.perez_d * gamma) + sky.perez_e * cos_gamma * cos_gamma);
}

// CIE xyY (D65 white) to linear sRGB / Rec. 709
float3 sky_xyy_to_rgb(float x, float y, float luminance)
{
    const float safe_y = max(y, 1e-4);
    const float3 xyz = float3(x / safe_y * luminance, luminance, (1.0 - x - y) / safe_y * luminance);
    return float3(3.2406 * xyz.x - 1.5372 * xyz.y - 0.4986 * xyz.z,
                  -0.9689 * xyz.x + 1.8758 * xyz.y + 0.0415 * xyz.z,
                  0.0557 * xyz.x - 0.2040 * xyz.y + 1.0570 * xyz.z);
}

// Linear RGB radiance (cd/m²) arriving from world direction `direction` (unit length, Z up)
float3 sky_radiance(sky_constants sky, float3 direction)
{
    if (sky.mode == sky_mode_uniform)
    {
        return sky.uniform_radiance;
    }
    if (direction.z < 0.0)
    {
        return sky.ground_radiance;
    }
    const float cos_gamma = clamp(dot(direction, sky.sun_position), -1.0, 1.0);
    const float3 value = sky.zenith * sky_perez(sky, direction.z, acos(cos_gamma), cos_gamma);
    return max(sky_xyy_to_rgb(value.y, value.z, value.x * sky.luminance_scale), 0.0);
}

#endif
