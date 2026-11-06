#ifndef AVIS_VERTEX_PULLING_HLSLI
#define AVIS_VERTEX_PULLING_HLSLI

// Vertex pulling from the geometry arena (docs/design/11-renderer.md §3.2, docs/tasks/11-renderer.md §11.10): positions
// and attributes read from ByteAddressBuffers by the stream offsets of the draw constants, and instance_data through
// instance_indices[offset + SV_InstanceID] (11 §4.8). The streams are laid out as geometry_arena.h describes:
//   position  f32x3
//   normal    octahedral u16x2
//   tangent   octahedral u16x2; bit 0 of the second component is the bitangent sign, 1 = -1
//   uv0       f16x2
//   color0    sRGB u8x4
// Indices are u32, relative to the primitive's first vertex. Included by binding_probe.hlsl, so it compiles with the
// scene shaders.

#include "packing.hlsli"
#include "shared_types.hlsli"

// The index into instance_data[] of the instance a draw's SV_InstanceID draws:
// instance_indices[instance_index_offset + SV_InstanceID]
uint pull_instance_index(scene_pass_constants pass_constants, scene_draw_constants draw, uint instance_id)
{
    StructuredBuffer<uint> instance_indices = ResourceDescriptorHeap[pass_constants.instance_indices];
    return instance_indices[draw.instance_index_offset + instance_id];
}

instance_data load_instance(scene_pass_constants pass_constants, uint instance)
{
    StructuredBuffer<instance_data> instances = ResourceDescriptorHeap[pass_constants.instances];
    return instances[instance];
}

material_data pull_material(scene_pass_constants pass_constants, uint slot)
{
    StructuredBuffer<material_data> materials = ResourceDescriptorHeap[pass_constants.materials];
    return materials[slot];
}

// The primitive's index `index` (counted from its first index): a vertex relative to the primitive's first vertex
uint pull_index(scene_pass_constants pass_constants, scene_draw_constants draw, uint index)
{
    ByteAddressBuffer indices = ResourceDescriptorHeap[pass_constants.indices];
    return indices.Load((draw.index_first + index) * 4);
}

// The primitive's vertex `vertex` (relative to its first vertex)
float3 pull_position(scene_pass_constants pass_constants, scene_draw_constants draw, uint vertex)
{
    ByteAddressBuffer vertices = ResourceDescriptorHeap[pass_constants.vertices];
    return asfloat(vertices.Load3(draw.position_offset + (draw.vertex_first + vertex) * 12));
}

// Object space, unit length; +Z when the mesh has no normals
float3 pull_normal(scene_pass_constants pass_constants, scene_draw_constants draw, uint vertex)
{
    if (draw.normal_offset == vertex_stream_absent)
    {
        return float3(0.0, 0.0, 1.0);
    }
    ByteAddressBuffer vertices = ResourceDescriptorHeap[pass_constants.vertices];
    return unpack_octahedral_unorm16(vertices.Load(draw.normal_offset + (draw.vertex_first + vertex) * 4));
}

// Object space, w = the bitangent sign (+1 or -1); w = 0 when the mesh has no tangents
float4 pull_tangent(scene_pass_constants pass_constants, scene_draw_constants draw, uint vertex)
{
    if (draw.tangent_offset == vertex_stream_absent)
    {
        return float4(1.0, 0.0, 0.0, 0.0);
    }
    ByteAddressBuffer vertices = ResourceDescriptorHeap[pass_constants.vertices];
    const uint packed = vertices.Load(draw.tangent_offset + (draw.vertex_first + vertex) * 4);
    // The sign bit replaces the lowest bit of the second component: one step of 65535, below the encoding's precision
    return float4(unpack_octahedral_unorm16(packed & ~0x10000u), (packed & 0x10000u) != 0 ? -1.0 : 1.0);
}

// (0, 0) when the mesh has no uv0
float2 pull_uv0(scene_pass_constants pass_constants, scene_draw_constants draw, uint vertex)
{
    if (draw.uv0_offset == vertex_stream_absent)
    {
        return float2(0.0, 0.0);
    }
    ByteAddressBuffer vertices = ResourceDescriptorHeap[pass_constants.vertices];
    return unpack_half2(vertices.Load(draw.uv0_offset + (draw.vertex_first + vertex) * 4));
}

// Linear RGBA; white when the mesh has no colours
float4 pull_color(scene_pass_constants pass_constants, scene_draw_constants draw, uint vertex)
{
    if (draw.color0_offset == vertex_stream_absent)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    ByteAddressBuffer vertices = ResourceDescriptorHeap[pass_constants.vertices];
    const float4 encoded = unpack_unorm4x8(vertices.Load(draw.color0_offset + (draw.vertex_first + vertex) * 4));
    return float4(srgb_to_linear(encoded.rgb), encoded.a);
}

#endif
