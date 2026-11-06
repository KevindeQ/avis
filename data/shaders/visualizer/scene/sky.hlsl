// @avis-shader entry=vertex_main stage=vs entry=pixel_main stage=ps
//
// The sky (docs/design/11-renderer.md §2.3 pass 9, docs/tasks/11-renderer.md §11.11): a fullscreen triangle at the far
// plane (depth 0, reverse-Z), drawn with depth test EQUAL and no depth write, so it only covers the pixels nothing was
// drawn into. Each pixel is the analytic sky (sky.hlsli) in its view direction, pre-exposed like deferred lighting.

#include "bindless.hlsli"
#include "exposure.hlsli"
#include "shared_types.hlsli"
#include "sky.hlsli"

AVIS_PASS_CONSTANTS(sky_pass_constants, constants);
AVIS_VIEW_CONSTANTS(view_constants, view);

struct sky_varyings
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

sky_varyings vertex_main(uint vertex : SV_VertexID)
{
    // (-1, -1), (3, -1), (-1, 3): one triangle covering the viewport
    const float2 ndc = float2(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0);
    sky_varyings output;
    output.position = float4(ndc, 0.0, 1.0);
    output.ndc = ndc;
    return output;
}

float4 pixel_main(sky_varyings input) : SV_Target0
{
    // The view ray through the pixel: two points on it, at the near plane and halfway to infinity
    const float4 near_point = mul(view.view_projection_inverse, float4(input.ndc, 1.0, 1.0));
    const float4 far_point = mul(view.view_projection_inverse, float4(input.ndc, 0.5, 1.0));
    const float3 direction = normalize(far_point.xyz / far_point.w - near_point.xyz / near_point.w);
    return float4(sky_radiance(constants.sky, direction) * load_current_exposure(constants.exposure_buffer), 1.0);
}
