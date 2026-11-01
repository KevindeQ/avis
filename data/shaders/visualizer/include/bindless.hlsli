#ifndef AVIS_BINDLESS_HLSLI
#define AVIS_BINDLESS_HLSLI

// The Avis standard binding model, as the shaders see it (docs/design/18-graphics-shaders-and-pipelines.md §9). It
// mirrors graphics::binding_layout (include/avis/graphics/binding_layout.h): change both together. binding_probe.hlsl
// declares every binding named here, and a test checks its reflected bindings against binding_layout's constants
// (docs/design/11-renderer.md §13).
//
// Root parameters (space0, visible to every stage):
//   b0  draw constants, at most 16 32-bit values (binding_layout::draw_constant_count)
//   b1  pass constants, a root CBV
//   b2  view constants, a root CBV
// Static samplers s0-s5 (space0), declared below.
// Every other resource is reached through ResourceDescriptorHeap / SamplerDescriptorHeap by bindless index; index 0 is
// the null descriptor.

// Register names, for declarations that can't use the macros below
#define AVIS_DRAW_CONSTANTS_REGISTER b0
#define AVIS_PASS_CONSTANTS_REGISTER b1
#define AVIS_VIEW_CONSTANTS_REGISTER b2

// The number of 32-bit draw constants: a draw constants struct is at most 64 bytes
static const uint avis_draw_constant_count = 16;

// The bindless index of the null descriptor
static const uint avis_null_descriptor = 0;

// Declares the draw, pass or view constants of a shader as `name`, of the shader's own struct `type`:
//   struct draw_constants { uint instance_offset; uint material; };
//   AVIS_DRAW_CONSTANTS(draw_constants, draw);
#define AVIS_DRAW_CONSTANTS(type, name) ConstantBuffer<type> name : register(b0, space0)
#define AVIS_PASS_CONSTANTS(type, name) ConstantBuffer<type> name : register(b1, space0)
#define AVIS_VIEW_CONSTANTS(type, name) ConstantBuffer<type> name : register(b2, space0)

// The static samplers, in binding_layout::static_sampler order. A shader that doesn't use one doesn't bind it (DXC
// drops unused declarations from the reflection).
SamplerState point_clamp_sampler : register(s0, space0);
SamplerState linear_clamp_sampler : register(s1, space0);
SamplerState linear_wrap_sampler : register(s2, space0);
SamplerState anisotropic_wrap_sampler : register(s3, space0); // 16x, linear mips
SamplerComparisonState shadow_comparison_sampler : register(s4, space0); // linear, GREATER_EQUAL (reverse-Z), clamp
SamplerState point_wrap_sampler : register(s5, space0);

// Whether a bindless index names a resource (anything but the null descriptor)
bool avis_has_descriptor(uint index)
{
    return index != avis_null_descriptor;
}

#endif
