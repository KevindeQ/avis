#ifndef AVIS_SCENE_VARYINGS_HLSLI
#define AVIS_SCENE_VARYINGS_HLSLI

// What the scene vertex shader (scene_vertex.hlsl) passes to every scene pixel shader (docs/design/11-renderer.md §4.8,
// docs/tasks/11-renderer.md §11.7). Every vertex shader permutation writes every member, with defaults for the
// attributes its mesh doesn't have, so any of them links with any scene pixel shader. The draw and pass constants of
// the scene passes, scene_draw_constants and scene_pass_constants, are in shared_types.hlsli (11.9).

#include "shared_types.hlsli"

struct scene_varyings
{
    float4 position : SV_Position;
    float3 world_position : WORLD_POSITION;
    float3 normal : NORMAL; // world space
    float4 tangent : TANGENT; // world space, w = bitangent sign
    float2 uv : TEXCOORD0;
    float4 color : COLOR0; // linear
    float4 current_clip : CURRENT_CLIP; // unjittered, for velocity
    float4 previous_clip : PREVIOUS_CLIP; // unjittered, of the previous rendered frame
    float2 quad : QUAD; // lines and points: the position in the expanded quad, in [-1, 1]
    nointerpolation uint instance : INSTANCE; // index into instance_data[]
    nointerpolation uint material : MATERIAL;
};

#endif
