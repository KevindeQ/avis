// @avis-shader entry=main stage=cs
//
// The tonemap (docs/design/11-renderer.md §10.4, §2.3 pass 13, docs/tasks/11-renderer.md §11.16), one pixel per thread
// in 8 × 8 groups: taa_output, pre-exposed with this frame's exposure, is rescaled to the exposure exposure_adapt just
// computed (so an exposure change shows in the frame that made it), mapped by ACES fitted (Hill's fit of the RRT and
// ODT) or AgX (Sobotka's, with the polynomial fit of its default contrast), sRGB-encoded with the exact transfer
// function, and dithered by ±0.5 LSB of tiled blue noise into ldr_color (RGBA8_UNORM). The CPU references of the
// tests (tests/helpers/tonemap_reference.h) follow the same steps.

#include "bindless.hlsli"
#include "exposure.hlsli"
#include "packing.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(tonemap_constants, constants);

// ACES fitted: sRGB to the RRT's input space, the fitted RRT + ODT curve, back to sRGB
float3 aces_fitted(float3 color)
{
    const float3x3 input = { 0.59719, 0.35458, 0.04823, //
                             0.07600, 0.90834, 0.01566, //
                             0.02840, 0.13383, 0.83777 };
    const float3x3 output = { 1.60475, -0.53108, -0.07367, //
                              -0.10208, 1.10813, -0.00605, //
                              -0.00327, -0.07276, 1.07602 };
    color = mul(input, color);
    const float3 a = color * (color + 0.0245786) - 0.000090537;
    const float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return saturate(mul(output, a / b));
}

// AgX: the inset matrix, log2 encoding over [−12.47393, 4.026069] stops, the default contrast curve (which gives a
// display value with a 2.2 gamma), then back through the outset matrix to linear. The matrices are written by columns,
// so they apply as mul(color, matrix).
float3 agx(float3 color)
{
    const float3x3 inset = { 0.842479062253094, 0.0423282422610123, 0.0423756549057051, //
                             0.0784335999999992, 0.878468636469772, 0.0784336, //
                             0.0792237451477643, 0.0791661274605434, 0.879142973793104 };
    const float3x3 outset = { 1.19687900512017, -0.0528968517574562, -0.0529716355144438, //
                              -0.0980208811401368, 1.15190312990417, -0.0980434501171241, //
                              -0.0990297440797205, -0.0989611768448433, 1.15107367264116 };
    const float minimum_stops = -12.47393;
    const float maximum_stops = 4.026069;

    color = mul(color, inset);
    color = clamp(log2(max(color, 1.0e-10)), minimum_stops, maximum_stops);
    const float3 x = (color - minimum_stops) / (maximum_stops - minimum_stops);
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    const float3 display = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
                           0.1191 * x - 0.00232;
    color = mul(display, outset);
    return saturate(pow(max(color, 0.0), 2.2));
}

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float4> color = ResourceDescriptorHeap[constants.color];
    Texture2D<float> blue_noise = ResourceDescriptorHeap[constants.blue_noise];
    RWTexture2D<float4> ldr_color = ResourceDescriptorHeap[constants.ldr_color];

    const float3 exposed = max(color.Load(int3(thread.xy, 0)).rgb, 0.0) * exposure_change(constants.exposure_buffer);
    const float3 mapped = constants.tonemapper == tonemapper_agx ? agx(exposed) : aces_fitted(exposed);
    // The rank's level in [0, 255] gives an offset in (−0.5, 0.5) LSB with a mean of 0
    const float level = round(blue_noise.Load(int3(thread.xy % blue_noise_size, 0)) * 255.0);
    const float dither = ((level + 0.5) / 256.0 - 0.5) / 255.0;
    ldr_color[thread.xy] = float4(saturate(linear_to_srgb(mapped) + dither), 1.0);
}
