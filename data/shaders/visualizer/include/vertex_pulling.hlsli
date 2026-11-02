#ifndef AVIS_VERTEX_PULLING_HLSLI
#define AVIS_VERTEX_PULLING_HLSLI

// Vertex pulling from the geometry arena (docs/design/11-renderer.md §3.2): positions and attributes read from
// ByteAddressBuffers by the range offsets and the attribute mask, and instance_data through
// instance_indices[offset + SV_InstanceID] (11 §4.8).
//
// So far (docs/tasks/11-renderer.md §11.9) it reads instances, positions and vertex colours, which the range walker's
// tests draw with. The depth prepass and G-buffer task (11.10) adds the normals, tangents and uvs. Included by
// binding_probe.hlsl, so it compiles with the scene shaders.

#include "packing.hlsli"
#include "shared_types.hlsli"

// The instance a draw's SV_InstanceID draws: instance_indices[instance_index_offset + SV_InstanceID]
instance_data pull_instance(scene_pass_constants pass_constants, scene_draw_constants draw, uint instance_id)
{
    StructuredBuffer<uint> instance_indices = ResourceDescriptorHeap[pass_constants.instance_indices];
    StructuredBuffer<instance_data> instances = ResourceDescriptorHeap[pass_constants.instances];
    return instances[instance_indices[draw.instance_index_offset + instance_id]];
}

// The primitive's vertex `vertex` (relative to its first vertex)
float3 pull_position(scene_pass_constants pass_constants, scene_draw_constants draw, uint vertex)
{
    ByteAddressBuffer vertices = ResourceDescriptorHeap[pass_constants.vertices];
    return asfloat(vertices.Load3(draw.position_offset + (draw.vertex_first + vertex) * 12));
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
