// @avis-shader entry=main stage=cs
//
// The binding probe (docs/design/11-renderer.md §13, docs/tasks/11-renderer.md §11.1): uses every binding that
// bindless.hlsli names, so its reflected bindings in the manifest can be checked against graphics::binding_layout's
// constants. It also includes every shared header, so they compile with the scene shaders. Never run by the renderer.
//
// Bindings: b0 (draw constants, the full 64 bytes), b1 (pass constants), b2 (view_constants of shared_types.hlsli, so
// the test can compare the reflected size with the C++ struct), s0-s5.

#include "bindless.hlsli"
#include "brdf.hlsli"
#include "packing.hlsli"
#include "shared_types.hlsli"
#include "vertex_pulling.hlsli"

struct probe_draw_constants
{
    uint4 values[4]; // binding_layout::draw_constant_count 32-bit values; x of values[0] is the output buffer
};

struct probe_pass_constants
{
    uint texture; // bindless SRV index of a Texture2D<float4>
    uint comparison_texture; // bindless SRV index of a Texture2D<float>
    uint2 padding;
};

AVIS_DRAW_CONSTANTS(probe_draw_constants, draw);
AVIS_PASS_CONSTANTS(probe_pass_constants, pass);
AVIS_VIEW_CONSTANTS(view_constants, view);

[numthreads(1, 1, 1)]
void main()
{
    Texture2D<float4> texture = ResourceDescriptorHeap[pass.texture];
    Texture2D<float> comparison_texture = ResourceDescriptorHeap[pass.comparison_texture];
    const float2 uv = view.jitter * view.inverse_size;

    float4 sum = texture.SampleLevel(point_clamp_sampler, uv, 0.0);
    sum += texture.SampleLevel(linear_clamp_sampler, uv, 0.0);
    sum += texture.SampleLevel(linear_wrap_sampler, uv, 0.0);
    sum += texture.SampleLevel(anisotropic_wrap_sampler, uv, 0.0);
    sum += texture.SampleLevel(point_wrap_sampler, uv, 0.0);
    sum += comparison_texture.SampleCmpLevelZero(shadow_comparison_sampler, uv, view.near_plane);
    sum += float4(unpack_r11g11b10(pack_r11g11b10(sum.rgb)), 0.0);
    sum.rg += octahedral_encode(normalize(mul(view.view_projection, float4(view.camera_position, 1.0)).xyz));

    uint packed = pack_unorm4x8(float4(linear_to_srgb(sum.rgb), sum.a)) ^ pack_half2(sum.xy);
    [unroll]
    for (uint index = 0; index < 4; ++index)
    {
        packed ^= draw.values[index].x ^ draw.values[index].y ^ draw.values[index].z ^ draw.values[index].w;
    }

    if (avis_has_descriptor(draw.values[0].x))
    {
        RWByteAddressBuffer output = ResourceDescriptorHeap[draw.values[0].x];
        output.Store(0, packed);
    }
}
