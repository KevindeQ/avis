// @avis-shader entry=vertex_main stage=vs entry=pixel_main stage=ps entry=inspect_main stage=cs
//
// The debug blit and the pixel inspector (docs/design/11-renderer.md §11, docs/tasks/11-renderer.md §11.20).
//
// vertex_main and pixel_main draw one blit: a fullscreen triangle in the blit's viewport, the rectangle of ldr_color it
// covers (the whole view, or a picture-in-picture corner). Each pixel Loads the texel of the selected mip and slice
// under it (the rectangle maps onto the whole mip), shows it as the mode says, and writes the colour as is: ldr_color
// holds sRGB-encoded bytes, and a debug value of 0.5 is byte 128. Integer formats are read as uint4, depth formats
// through their R32_FLOAT SRV.
//
// inspect_main reads one texel for the inspector into a small buffer, which a copy pass reads back: float4 value at 0,
// uint4 bits at 16 (debug_inspector_bytes):
// - a float format: the texel as the shader reads it (missing channels 0, alpha 1), and its bits;
// - an integer format: the texel as float, and the texel;
// - a depth format: value.x the stored depth and value.y its linear depth (metres), and their bits;
// - cluster_lights: value.x the pixel's linear depth; bits the cluster's offset, count, dropped lights and index.

#include "bindless.hlsli"
#include "lighting.hlsli"
#include "packing.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(debug_blit_constants, constants);
AVIS_VIEW_CONSTANTS(view_constants, view);

// ---- reading the source ----

float4 load_float(uint2 texel)
{
    if (constants.is_array != 0)
    {
        Texture2DArray<float4> source = ResourceDescriptorHeap[constants.source];
        return source.Load(int4(texel, constants.slice, constants.mip));
    }
    Texture2D<float4> source = ResourceDescriptorHeap[constants.source];
    return source.Load(int3(texel, constants.mip));
}

uint4 load_uint(uint2 texel)
{
    if (constants.is_array != 0)
    {
        Texture2DArray<uint4> source = ResourceDescriptorHeap[constants.source];
        return source.Load(int4(texel, constants.slice, constants.mip));
    }
    Texture2D<uint4> source = ResourceDescriptorHeap[constants.source];
    return source.Load(int3(texel, constants.mip));
}

// A reverse-Z device depth's view-space depth, through the camera's projection: +infinity at 0 with an infinite far
// plane
float linearize(float depth)
{
    const float4 point_in_view = mul(view.projection_inverse, float4(0.0, 0.0, depth, 1.0));
    return -point_in_view.z / point_in_view.w;
}

struct cluster_of_pixel
{
    cluster_lights lights;
    uint index;
    float linear_depth;
};

cluster_of_pixel load_cluster(uint2 pixel)
{
    Texture2D<float> linear_depth = ResourceDescriptorHeap[constants.linear_depth];
    StructuredBuffer<cluster_lights> clusters = ResourceDescriptorHeap[constants.source];
    cluster_of_pixel result;
    result.linear_depth = linear_depth.Load(int3(pixel, 0));
    result.index = cluster_index(pixel_cluster(pixel, constants.tile_size, result.linear_depth));
    result.lights = clusters[result.index];
    return result;
}

// The source's texel as floats; a cluster's light count in x
float4 load_value(uint2 texel)
{
    if (constants.source_kind == debug_source_uint)
    {
        return float4(load_uint(texel));
    }
    if (constants.source_kind == debug_source_clusters)
    {
        return float4(float(load_cluster(texel).lights.count), 0.0, 0.0, 0.0);
    }
    return load_float(texel);
}

// ---- showing it ----

float remap(float value)
{
    return saturate((value - constants.range_minimum) / (constants.range_maximum - constants.range_minimum));
}

// Blue, cyan, green, yellow, red from 0 to 1
float3 heat(float t)
{
    const float3 stops[5] = { float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 1.0), float3(0.0, 1.0, 0.0),
                              float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0) };
    const float scaled = saturate(t) * 4.0;
    const uint below = min(uint(scaled), 3u);
    return lerp(stops[below], stops[below + 1], scaled - float(below));
}

float3 hsv_to_rgb(float hue, float saturation, float value)
{
    const float3 k = saturate(abs(frac(hue + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return value * lerp(float3(1.0, 1.0, 1.0), k, saturation);
}

float3 show(float4 value)
{
    switch (constants.mode)
    {
    case debug_mode_depth_linear:
    {
        const float depth = constants.source_kind == debug_source_depth ? linearize(value.x) : value.x;
        return isfinite(depth) ? remap(depth).xxx : float3(1.0, 1.0, 1.0);
    }
    case debug_mode_normal_octahedral:
        return octahedral_decode(value.xy) * 0.5 + 0.5;
    case debug_mode_velocity:
    {
        const float2 texels = value.xy * float2(constants.source_size);
        const float hue = atan2(texels.y, texels.x) / (2.0 * 3.14159265) + 0.5;
        return hsv_to_rgb(hue, 1.0, saturate(length(texels) / constants.range_maximum));
    }
    case debug_mode_ao:
        return remap(value.x).xxx;
    case debug_mode_cascade_index:
    {
        const uint cascade = uint(value.x);
        const float3 colors[4] = { float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0),
                                   float3(1.0, 1.0, 0.0) };
        return cascade < 4 ? colors[cascade] : float3(0.0, 0.0, 0.0);
    }
    case debug_mode_cluster_light_count:
        return heat(remap(value.x));
    case debug_mode_luminance_heat:
        return heat(remap(dot(value.rgb, float3(0.2126, 0.7152, 0.0722))));
    case debug_mode_alpha_only:
        return remap(value.a).xxx;
    default: // debug_mode_raw
    {
        if (countbits(constants.channels) == 1)
        {
            const uint channel = firstbitlow(constants.channels);
            return remap(value[channel]).xxx;
        }
        const float3 mask = float3((constants.channels & 1u) != 0, (constants.channels & 2u) != 0,
                                   (constants.channels & 4u) != 0);
        return float3(remap(value.r), remap(value.g), remap(value.b)) * mask;
    }
    }
}

// ---- the blit ----

float4 vertex_main(uint vertex : SV_VertexID) : SV_Position
{
    // (-1, -1), (3, -1), (-1, 3): one triangle covering the viewport
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
}

float4 pixel_main(float4 position : SV_Position) : SV_Target0
{
    const float2 local = (position.xy - float2(constants.origin)) / float2(constants.extent);
    const uint2 texel = min(uint2(local * float2(constants.source_size)), constants.source_size - 1);
    return float4(show(load_value(texel)), 1.0);
}

// ---- the inspector ----

[numthreads(1, 1, 1)] void inspect_main()
{
    float4 value;
    uint4 bits;
    if (constants.source_kind == debug_source_clusters)
    {
        const cluster_of_pixel cluster = load_cluster(constants.texel);
        value = float4(cluster.linear_depth, 0.0, 0.0, 0.0);
        bits = uint4(cluster.lights.offset, cluster.lights.count, cluster.lights.dropped, cluster.index);
    }
    else if (constants.source_kind == debug_source_uint)
    {
        bits = load_uint(constants.texel);
        value = float4(bits);
    }
    else
    {
        value = load_float(constants.texel);
        if (constants.source_kind == debug_source_depth)
        {
            value = float4(value.x, linearize(value.x), 0.0, 0.0);
        }
        bits = asuint(value);
    }
    RWByteAddressBuffer output = ResourceDescriptorHeap[constants.output];
    output.Store4(0, asuint(value));
    output.Store4(16, bits);
}
