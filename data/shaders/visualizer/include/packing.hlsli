#ifndef AVIS_PACKING_HLSLI
#define AVIS_PACKING_HLSLI

// Packing helpers of the scene renderer (docs/design/11-renderer.md §3.2, §5): octahedral normals, half floats, sRGB,
// 8-bit unorm vectors and R11G11B10 floats.

// ---- Octahedral unit vectors (Cigolle et al. 2014) ----

float2 octahedral_wrap(float2 value)
{
    return (1.0 - abs(value.yx)) * select(value >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
}

// A unit vector to [-1, 1]^2
float2 octahedral_encode_signed(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : octahedral_wrap(n.xy);
}

float3 octahedral_decode_signed(float2 encoded)
{
    float3 n = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    const float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, float2(-t, -t), float2(t, t));
    return normalize(n);
}

// A unit vector to [0, 1]^2, for UNORM targets (gbuffer_normal, RG16_UNORM)
float2 octahedral_encode(float3 n)
{
    return octahedral_encode_signed(n) * 0.5 + 0.5;
}

float3 octahedral_decode(float2 encoded)
{
    return octahedral_decode_signed(encoded * 2.0 - 1.0);
}

// Two 16-bit UNORM components in one uint (x in the low half): the vertex normal and tangent format (11 §3.2)
uint pack_octahedral_unorm16(float3 n)
{
    const uint2 quantised = uint2(round(saturate(octahedral_encode(n)) * 65535.0));
    return quantised.x | (quantised.y << 16);
}

float3 unpack_octahedral_unorm16(uint packed)
{
    const float2 encoded = float2(packed & 0xFFFFu, packed >> 16) / 65535.0;
    return octahedral_decode(encoded);
}

// ---- Half floats ----

// x in the low half
uint pack_half2(float2 value)
{
    const uint2 halves = f32tof16(value);
    return halves.x | (halves.y << 16);
}

float2 unpack_half2(uint packed)
{
    return f16tof32(uint2(packed & 0xFFFFu, packed >> 16));
}

// ---- 8-bit UNORM ----

// x in the lowest byte
uint pack_unorm4x8(float4 value)
{
    const uint4 bytes = uint4(round(saturate(value) * 255.0));
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

float4 unpack_unorm4x8(uint packed)
{
    return float4(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu, packed >> 24) / 255.0;
}

// ---- sRGB transfer function (IEC 61966-2-1, exact) ----

float3 srgb_to_linear(float3 value)
{
    return select(value <= 0.04045, value / 12.92, pow((value + 0.055) / 1.055, 2.4));
}

float3 linear_to_srgb(float3 value)
{
    value = saturate(value);
    return select(value <= 0.0031308, value * 12.92, 1.055 * pow(value, 1.0 / 2.4) - 0.055);
}

// ---- R11G11B10 floats (gbuffer_emissive) ----
// The 11- and 10-bit formats are half floats without sign and with fewer mantissa bits: 5 exponent bits, then 6 (R, G)
// or 5 (B) mantissa bits. Negative values become 0; the mantissa is truncated.

uint pack_r11g11b10(float3 value)
{
    const uint3 halves = f32tof16(max(value, 0.0));
    const uint r = (halves.x >> 4) & 0x7FFu;
    const uint g = (halves.y >> 4) & 0x7FFu;
    const uint b = (halves.z >> 5) & 0x3FFu;
    return r | (g << 11) | (b << 22);
}

float3 unpack_r11g11b10(uint packed)
{
    const uint r = (packed & 0x7FFu) << 4;
    const uint g = ((packed >> 11) & 0x7FFu) << 4;
    const uint b = ((packed >> 22) & 0x3FFu) << 5;
    return f16tof32(uint3(r, g, b));
}

#endif
