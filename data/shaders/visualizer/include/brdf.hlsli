#ifndef AVIS_BRDF_HLSLI
#define AVIS_BRDF_HLSLI

// The scene renderer's BRDF (docs/design/11-renderer.md §6.1): Cook-Torrance with GGX D, height-correlated Smith V and
// Schlick F, Lambert diffuse with (1 - F)(1 - metallic), roughness clamped to at least 0.045, and a Toksvig term from
// the normal map's mips.
//
// A stub for now: the lighting task (docs/tasks/11-renderer.md §11.11) fills it in. Included by binding_probe.hlsl, so
// it already compiles with the scene shaders.

#endif
