#ifndef AVIS_VERTEX_PULLING_HLSLI
#define AVIS_VERTEX_PULLING_HLSLI

// Vertex pulling from the geometry arena (docs/design/11-renderer.md §3.2): positions and attributes read from
// ByteAddressBuffers by the range offsets and the attribute mask, and instance_data through
// instance_indices[offset + SV_InstanceID] (11 §4.8).
//
// A stub for now: the depth prepass and G-buffer task (docs/tasks/11-renderer.md §11.10) fills it in. Included by
// binding_probe.hlsl, so it already compiles with the scene shaders.

#include "packing.hlsli"
#include "shared_types.hlsli"

#endif
