// @avis-shader entry=main stage=cs
//
// The TAA resolve (docs/design/11-renderer.md §10.2, §2.3 pass 11, docs/tasks/11-renderer.md §11.15), on pre-exposed
// HDR colour, one pixel per thread in 8 × 8 groups:
// 1. Velocity dilation: the velocity of the closest pixel (smallest linear depth) of the 3 × 3 neighbourhood. Where
//    nothing was drawn there, the velocity is the camera's, by reprojecting the pixel's direction.
// 2. The history at the previous position, from 5-tap Catmull-Rom sampling, rescaled to this frame's exposure (the
//    current exposure of the view's exposure buffer over the previous one, which the history was pre-exposed with).
// 3. Disocclusion: the previous linear depth of the 3 × 3 texels around the previous position is compared with the
//    surface's, both its current depth and the depth the camera's motion alone gives it. With no texel within
//    disocclusion_tolerance of either, the surface wasn't visible there, and the history is rejected. A previous
//    position outside the view rejects it too.
// 4. Variance clipping: the history is clipped towards the centre of the neighbourhood's YCoCg box, mean ± gamma ·
//    standard deviation.
// 5. The blend: this frame's weight is `blend` (0.1), raised towards 1 by the reactive mask, and 1 when the history
//    is rejected or missing. The two are weighted by 1 / (1 + luminance) (Karis), which keeps fireflies out.
// It writes taa_output into the view's history slot, and this frame's linear depth into the depth history.

#include "bindless.hlsli"
#include "exposure.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(taa_constants, constants);
AVIS_VIEW_CONSTANTS(view_constants, view);

// A linear depth at or beyond this is "nothing drawn" (linearize_depth writes +infinity there)
static const float no_depth = 1.0e30;

float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 rgb_to_ycocg(float3 color)
{
    return float3(dot(color, float3(0.25, 0.5, 0.25)), dot(color, float3(0.5, 0.0, -0.5)),
                  dot(color, float3(-0.25, 0.5, -0.25)));
}

float3 ycocg_to_rgb(float3 color)
{
    const float base = color.x - color.z;
    return float3(base + color.y, color.x + color.z, base - color.y);
}

// The view-space position at linear depth 1 through the centre of `pixel`, through the jittered projection the frame
// was drawn with. View space looks down −z, so the position at depth d is d · ray.
float3 view_ray(float2 pixel)
{
    const float2 ndc = ((pixel + 0.5) * view.inverse_size * 2.0 - 1.0) * float2(1.0, -1.0);
    const float4 near_point = mul(view.projection_inverse, float4(ndc, 1.0, 1.0));
    const float3 ray = near_point.xyz / near_point.w;
    return ray / -ray.z;
}

// The UV (y down) of a clip position
float2 clip_to_uv(float4 clip)
{
    return clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
}

// The history at `uv`, by Catmull-Rom over its 4 × 4 texels, with the four corner taps left out (5 bilinear taps)
float3 sample_history(Texture2D<float4> history, float2 uv)
{
    const float2 position = uv * float2(constants.size);
    const float2 centre = floor(position - 0.5) + 0.5;
    const float2 f = position - centre;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 inverse_size = view.inverse_size;
    const float2 uv0 = (centre - 1.0) * inverse_size;
    const float2 uv12 = (centre + w2 / w12) * inverse_size;
    const float2 uv3 = (centre + 2.0) * inverse_size;

    const float weights[5] = { w12.x * w0.y, w0.x * w12.y, w12.x * w12.y, w3.x * w12.y, w12.x * w3.y };
    const float2 positions[5] = { float2(uv12.x, uv0.y), float2(uv0.x, uv12.y), uv12, float2(uv3.x, uv12.y),
                                  float2(uv12.x, uv3.y) };
    float3 sum = 0.0;
    float total = 0.0;
    [unroll] for (uint tap = 0; tap < 5; ++tap)
    {
        sum += history.SampleLevel(linear_clamp_sampler, positions[tap], 0.0).rgb * weights[tap];
        total += weights[tap];
    }
    return max(sum / total, 0.0);
}

// `history` clipped towards the centre of the box `centre` ± `extents`, along the line between them
float3 clip_to_box(float3 history, float3 centre, float3 extents)
{
    const float3 offset = history - centre;
    const float3 units = abs(offset) / max(extents, 1.0e-6);
    const float largest = max(units.x, max(units.y, units.z));
    return largest > 1.0 ? centre + offset / largest : history;
}

// Whether a previous depth matches the surface: both nothing, or within the relative tolerance
bool depth_matches(float previous, float expected)
{
    if (expected >= no_depth || previous >= no_depth)
    {
        return expected >= no_depth && previous >= no_depth;
    }
    return abs(previous - expected) <= constants.disocclusion_tolerance * expected;
}

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float4> hdr_color = ResourceDescriptorHeap[constants.hdr_color];
    Texture2D<float> linear_depth = ResourceDescriptorHeap[constants.linear_depth];
    RWTexture2D<float4> output = ResourceDescriptorHeap[constants.output];
    RWTexture2D<float> depth_output = ResourceDescriptorHeap[constants.depth_output];

    const int2 pixel = int2(thread.xy);
    const int2 last = int2(constants.size) - 1;
    const float3 current = max(hdr_color.Load(int3(pixel, 0)).rgb, 0.0);
    depth_output[pixel] = linear_depth.Load(int3(pixel, 0));
    if (constants.has_history == 0)
    {
        output[pixel] = float4(current, 1.0);
        return;
    }
    Texture2D<float2> velocity = ResourceDescriptorHeap[constants.velocity];
    Texture2D<float> reactive_mask = ResourceDescriptorHeap[constants.reactive_mask];
    Texture2D<float4> history = ResourceDescriptorHeap[constants.history];
    Texture2D<float> depth_history = ResourceDescriptorHeap[constants.depth_history];

    // The neighbourhood: its YCoCg moments and its closest pixel
    float3 first_moment = 0.0;
    float3 second_moment = 0.0;
    int2 closest = pixel;
    float closest_depth = 1.0e38;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 neighbour = clamp(pixel + int2(x, y), 0, last);
            const float3 color = rgb_to_ycocg(max(hdr_color.Load(int3(neighbour, 0)).rgb, 0.0));
            first_moment += color;
            second_moment += color * color;
            const float depth = linear_depth.Load(int3(neighbour, 0));
            if (depth < closest_depth)
            {
                closest_depth = depth;
                closest = neighbour;
            }
        }
    }

    // The closest surface in world space (homogeneous: a direction where nothing was drawn), and where the camera
    // alone puts it in the previous frame
    const bool drawn = closest_depth < no_depth;
    const float3 ray = view_ray(float2(closest));
    const float4 world = mul(view.view_inverse, drawn ? float4(ray * closest_depth, 1.0) : float4(ray, 0.0));
    const float4 previous_clip = mul(view.view_projection_previous, world);
    const float2 motion = drawn ? velocity.Load(int3(closest, 0))
                                : clip_to_uv(mul(view.view_projection_unjittered, world)) - clip_to_uv(previous_clip);
    const float2 uv = (float2(pixel) + 0.5) * view.inverse_size;
    const float2 previous_uv = uv - motion;

    bool rejected = any(previous_uv < 0.0) || any(previous_uv > 1.0);
    if (!rejected)
    {
        // The 3 × 3 texels around the previous position: thin geometry covers different pixels from one jitter
        // sample to the next, so a smaller footprint rejects the history of its edges every few frames
        const float camera_depth = drawn ? previous_clip.w : 1.0e38;
        const int2 centre = int2(floor(previous_uv * float2(constants.size)));
        bool matched = false;
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                const int2 texel = clamp(centre + int2(x, y), 0, last);
                const float stored = depth_history.Load(int3(texel, 0));
                matched = matched || depth_matches(stored, closest_depth) || depth_matches(stored, camera_depth);
            }
        }
        rejected = !matched;
    }

    float weight = 1.0;
    float3 previous = current;
    if (!rejected)
    {
        previous = sample_history(history, previous_uv) * exposure_change(constants.exposure_buffer);
        const float3 mean = first_moment / 9.0;
        const float3 deviation = sqrt(max(second_moment / 9.0 - mean * mean, 0.0));
        previous = ycocg_to_rgb(clip_to_box(rgb_to_ycocg(previous), mean, constants.gamma * deviation));
        if (any(isnan(previous)))
        {
            previous = current;
        }
        weight = lerp(constants.blend, 1.0, saturate(reactive_mask.Load(int3(pixel, 0))));
    }

    const float current_weight = weight / (1.0 + luminance(current));
    const float previous_weight = (1.0 - weight) / (1.0 + luminance(previous));
    const float3 resolved = (current * current_weight + previous * previous_weight) / (current_weight + previous_weight);
    output[pixel] = float4(max(resolved, 0.0), 1.0);
}
