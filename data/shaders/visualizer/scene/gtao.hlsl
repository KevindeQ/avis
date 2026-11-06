// @avis-shader entry=main stage=cs entry=denoise_main stage=cs
//
// Ground-truth ambient occlusion (docs/design/11-renderer.md §8, §2.3 passes 5 and 6, docs/tasks/11-renderer.md
// §11.13), after Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate Indirect Occlusion", with the
// structure of the public XeGTAO description. Written from scratch. On async compute, 8 × 8 threads per group.
//   main          gtao: one AO texel per thread. Texel p is centred on pixel p · scale (scale 2 at half resolution).
//                 Its slice_count slices are planes through the view vector, evenly spaced around it (spacing them
//                 evenly by their angle on screen instead would bias off-axis pixels: a plane facing the camera would
//                 read about 0.95 near the corners of the view). For each, it searches the horizon on both sides with
//                 steps_per_side samples at quadratically growing distances, up to radius metres, reading the depth
//                 mip that matches the step's distance. Occluders fade out over the last falloff · radius. The
//                 visibility is the analytic cosine-weighted integral between the two horizons, weighted by the
//                 length of the normal projected onto the slice. Slice rotations and step offsets come from a
//                 Hilbert-curve index times the R2 sequence, offset by the frame index, so TAA resolves the noise.
//                 Writes the AO (R8) and the depth edges (R8, 2 bits per side) for the denoiser.
//   denoise_main  gtao_denoise: an edge-aware 3 × 3 filter, run gtao_denoise_passes times.
// The lighting pass upsamples a half-resolution result with depth-aware weights (deferred_lighting.hlsl).

#include "bindless.hlsli"
#include "packing.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(gtao_constants, constants);
AVIS_DRAW_CONSTANTS(gtao_denoise_constants, denoise_constants);
AVIS_VIEW_CONSTANTS(view_constants, view);

static const float pi = 3.14159265358979;
static const float half_pi = 1.57079632679490;
// A linear depth at or beyond this is "nothing drawn" (linearize_depth writes +infinity there)
static const float no_depth = 1.0e30;
// The first step stays this many pixels away from the centre, so it never samples its own texel
static const float minimum_step_pixels = 1.3;
// Mip m serves steps of about 2^(m + this) pixels, so a step's samples stay close in the cache (XeGTAO's 3.3)
static const float mip_sampling_offset = 3.3;

// ---- shared ----

// The view-space position at linear depth 1 through `pixel` (pixel units, texel centres at + 0.5), through the
// jittered projection the depth was drawn with. View space looks down −z, so the position at depth d is d · ray.
float3 view_ray(float2 pixel)
{
    const float2 ndc = (pixel * view.inverse_size * 2.0 - 1.0) * float2(1.0, -1.0);
    const float4 near_point = mul(view.projection_inverse, float4(ndc, 1.0, 1.0));
    const float3 ray = near_point.xyz / near_point.w;
    return ray / -ray.z;
}

// Edge weights in [0, 1] towards the left, right, top and bottom neighbours: 1 where the neighbour continues the
// centre's plane, 0 across a depth discontinuity. A planar slope is removed first, so tilted surfaces stay connected.
float4 depth_edges(float centre, float4 neighbours, float scale)
{
    float4 edges = min(neighbours, centre * 16.0) - centre; // finite, also against "nothing drawn"
    const float slope_lr = (edges.y - edges.x) * 0.5;
    const float slope_tb = (edges.w - edges.z) * 0.5;
    const float4 adjusted = edges + float4(slope_lr, -slope_lr, slope_tb, -slope_tb);
    edges = min(abs(edges), abs(adjusted));
    return saturate(1.25 - edges / (centre * 0.011 * scale));
}

float pack_edges(float4 edges)
{
    const uint4 bits = uint4(round(saturate(edges) * 2.9));
    return float((bits.x << 6) | (bits.y << 4) | (bits.z << 2) | bits.w) / 255.0;
}

float4 unpack_edges(float packed)
{
    const uint bits = uint(packed * 255.0 + 0.5);
    return float4((bits >> 6) & 3u, (bits >> 4) & 3u, (bits >> 2) & 3u, bits & 3u) / 3.0;
}

// ---- gtao ----

// The index of `texel` along a Hilbert curve over 64 × 64 tiles
uint hilbert_index(uint2 texel)
{
    const uint size = 64;
    uint x = texel.x % size;
    uint y = texel.y % size;
    uint index = 0;
    for (uint level = size / 2; level > 0; level /= 2)
    {
        const uint rx = (x & level) > 0 ? 1 : 0;
        const uint ry = (y & level) > 0 ? 1 : 0;
        index += level * level * ((3 * rx) ^ ry);
        if (ry == 0)
        {
            if (rx == 1)
            {
                x = size - 1 - x;
                y = size - 1 - y;
            }
            const uint swapped = x;
            x = y;
            y = swapped;
        }
    }
    return index;
}

// Two values in [0, 1): the slice rotation and the step offset of a texel in a frame. The Hilbert index keeps
// neighbouring texels far apart in the R2 sequence; the frame index walks it over 64 frames.
float2 gtao_noise(uint2 texel, uint frame_index)
{
    const uint index = hilbert_index(texel) + 288 * (frame_index % 64);
    return frac(0.5 + float(index) * float2(0.75487766624669276, 0.56984029099805327));
}

// The linear depth of one texel of `depth_mips`: the middle of its min and max, which is exact at the texel's centre
// for a plane; the min alone where part of the texel has nothing drawn
float mip_depth(Texture2D<float2> depth_mips, int2 texel, uint mip)
{
    const float2 range = depth_mips.Load(int3(texel, mip));
    return range.y < no_depth ? 0.5 * (range.x + range.y) : range.x;
}

// The linear depth at `pixel` from mip `mip`: bilinear between the four texel centres around it, so the sample stays
// on the slice's line (a sample moved to a texel centre leaves the slice, and on a surface seen at an angle that
// raises the horizon). Where one of the four has nothing drawn, the nearest texel's depth, so silhouettes don't blend
// into the background.
float sample_depth(Texture2D<float2> depth_mips, float2 pixel, uint mip)
{
    const int2 mip_size = int2(max(uint2(view.size) >> mip, 1u));
    const float2 position = pixel / float(1u << mip) - 0.5; // in texels, centres at integers
    const int2 base = int2(floor(position));
    const float2 fraction = position - float2(base);
    const int2 last = mip_size - 1;
    const float d00 = mip_depth(depth_mips, clamp(base, 0, last), mip);
    const float d10 = mip_depth(depth_mips, clamp(base + int2(1, 0), 0, last), mip);
    const float d01 = mip_depth(depth_mips, clamp(base + int2(0, 1), 0, last), mip);
    const float d11 = mip_depth(depth_mips, clamp(base + int2(1, 1), 0, last), mip);
    if (max(max(d00, d10), max(d01, d11)) < no_depth)
    {
        return lerp(lerp(d00, d10, fraction.x), lerp(d01, d11, fraction.x), fraction.y);
    }
    const int2 nearest = clamp(int2(floor(position + 0.5)), 0, last);
    return mip_depth(depth_mips, nearest, mip);
}

// The cosine of the horizon a sample at `pixel` gives, from `position` along `view_vector`; `low` (the tangent plane's)
// where nothing was drawn, or faded towards it by the distance falloff
float horizon_cosine(
    Texture2D<float2> depth_mips,
    float2 pixel,
    uint mip,
    float3 position,
    float3 view_vector,
    float low,
    float falloff_scale,
    float falloff_offset)
{
    const float2 clamped = clamp(pixel, 0.5, view.size - 0.5);
    const float depth = sample_depth(depth_mips, clamped, mip);
    if (!(depth < no_depth))
    {
        return low;
    }
    const float3 delta = view_ray(clamped) * depth - position;
    const float distance = length(delta);
    if (!(distance > 0.0))
    {
        return low;
    }
    const float weight = saturate(distance * falloff_scale + falloff_offset);
    return lerp(low, dot(delta / distance, view_vector), weight);
}

// The visibility over one side of a slice, from the normal's angle n to the horizon angle h (both from the view
// vector), cosine-weighted (Jimenez et al. 2016, equation 10)
float arc_visibility(float h, float n, float cos_n, float sin_n)
{
    return 0.25 * (cos_n + 2.0 * h * sin_n - cos(2.0 * h - n));
}

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float2> depth_mips = ResourceDescriptorHeap[constants.depth_mips];
    Texture2D<float2> normals = ResourceDescriptorHeap[constants.normal];
    RWTexture2D<float> ao = ResourceDescriptorHeap[constants.ao];
    RWTexture2D<float> edges = ResourceDescriptorHeap[constants.edges];

    const int2 full_size = int2(view.size);
    const int scale = int(constants.scale);
    const int2 pixel = min(int2(thread.xy) * scale, full_size - 1);
    const float depth = depth_mips.Load(int3(pixel, 0)).x;
    if (!(depth < no_depth))
    {
        ao[thread.xy] = 1.0;
        edges[thread.xy] = 0.0;
        return;
    }

    // The edges, between this texel's pixel and the next texels' pixels
    const float4 neighbours = float4(
        depth_mips.Load(int3(clamp(pixel - int2(scale, 0), 0, full_size - 1), 0)).x,
        depth_mips.Load(int3(clamp(pixel + int2(scale, 0), 0, full_size - 1), 0)).x,
        depth_mips.Load(int3(clamp(pixel - int2(0, scale), 0, full_size - 1), 0)).x,
        depth_mips.Load(int3(clamp(pixel + int2(0, scale), 0, full_size - 1), 0)).x);
    edges[thread.xy] = pack_edges(depth_edges(depth, neighbours, float(scale)));

    const float2 centre = float2(pixel) + 0.5;
    const float3 position = view_ray(centre) * depth;
    const float3 view_vector = normalize(-position);
    const float3 normal = normalize(mul((float3x3)view.view, octahedral_decode(normals.Load(int3(pixel, 0)))));

    // The radius in pixels at this depth: projection[1][1] is 1 / tan(fov / 2), and pixels are square
    const float radius_pixels = constants.radius * 0.5 * view.size.y * view.projection[1][1] / depth;
    if (radius_pixels < minimum_step_pixels)
    {
        ao[thread.xy] = 1.0; // the radius covers no other pixel
        return;
    }
    const float falloff_range = max(constants.falloff * constants.radius, 1.0e-4);
    const float falloff_scale = -1.0 / falloff_range;
    const float falloff_offset = (constants.radius - falloff_range) / falloff_range + 1.0;
    const float minimum_step = minimum_step_pixels / radius_pixels;
    const float2 noise = gtao_noise(thread.xy, view.frame_index);
    const uint highest_mip = constants.depth_mip_count - 1;
    // A basis of the plane perpendicular to the view vector (whose z is positive: the point is in front)
    const float3 basis_x = normalize(float3(view_vector.z, 0.0, -view_vector.x));
    const float3 basis_y = cross(view_vector, basis_x);

    float visibility = 0.0;
    for (uint slice = 0; slice < constants.slice_count; ++slice)
    {
        // The slice's direction perpendicular to the view vector, and where it goes on screen (y down): the
        // derivative of the perspective projection at the position along it
        const float psi = (float(slice) + noise.x) * pi / float(constants.slice_count);
        const float3 ortho_direction = cos(psi) * basis_x + sin(psi) * basis_y;
        const float2 omega = normalize(float2(
            ortho_direction.x * -position.z + position.x * ortho_direction.z,
            -(ortho_direction.y * -position.z + position.y * ortho_direction.z)));
        const float3 axis = normalize(cross(ortho_direction, view_vector));
        const float3 projected_normal = normal - axis * dot(normal, axis);
        const float projected_length = length(projected_normal);
        if (!(projected_length > 1.0e-4))
        {
            continue; // the normal is perpendicular to the slice: it adds nothing
        }

        // The normal's angle from the view vector within the slice, positive towards +omega
        const float sign_n = dot(ortho_direction, projected_normal) >= 0.0 ? 1.0 : -1.0;
        const float cos_n = saturate(dot(projected_normal, view_vector) / projected_length);
        const float n = sign_n * acos(cos_n);
        const float sin_n = sin(n);

        // The horizons start at the tangent plane; samples can only raise them
        const float low_positive = cos(n + half_pi);
        const float low_negative = cos(n - half_pi);
        float horizon_positive = low_positive;
        float horizon_negative = low_negative;
        for (uint step_index = 0; step_index < constants.steps_per_side; ++step_index)
        {
            float s = (float(step_index) + noise.y) / float(constants.steps_per_side);
            s = s * s + minimum_step;
            const float2 offset = s * radius_pixels * omega;
            const uint mip = uint(clamp(round(log2(length(offset)) - mip_sampling_offset), 0.0, float(highest_mip)));
            horizon_positive =
                max(horizon_positive,
                    horizon_cosine(
                        depth_mips,
                        centre + offset,
                        mip,
                        position,
                        view_vector,
                        low_positive,
                        falloff_scale,
                        falloff_offset));
            horizon_negative =
                max(horizon_negative,
                    horizon_cosine(
                        depth_mips,
                        centre - offset,
                        mip,
                        position,
                        view_vector,
                        low_negative,
                        falloff_scale,
                        falloff_offset));
        }

        // The horizon angles, within the hemisphere around the normal
        float h_negative = -acos(clamp(horizon_negative, -1.0, 1.0));
        float h_positive = acos(clamp(horizon_positive, -1.0, 1.0));
        h_negative = n + clamp(h_negative - n, -half_pi, half_pi);
        h_positive = n + clamp(h_positive - n, -half_pi, half_pi);
        visibility += projected_length *
                      (arc_visibility(h_negative, n, cos_n, sin_n) + arc_visibility(h_positive, n, cos_n, sin_n));
    }
    visibility = saturate(visibility / float(constants.slice_count));
    ao[thread.xy] = pow(visibility, constants.intensity);
}

    // ---- gtao_denoise ----

    [numthreads(8, 8, 1)] void denoise_main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= denoise_constants.size))
    {
        return;
    }
    Texture2D<float> source = ResourceDescriptorHeap[denoise_constants.source];
    Texture2D<float> edges = ResourceDescriptorHeap[denoise_constants.edges];
    RWTexture2D<float> destination = ResourceDescriptorHeap[denoise_constants.destination];

    const int2 texel = int2(thread.xy);
    const int2 last = int2(denoise_constants.size) - 1;
    const float4 centre_edges = unpack_edges(edges.Load(int3(texel, 0)));

    // A neighbour's weight is the lower of the two edges between it and the centre; off the texture it's 0
    float4 weights = centre_edges;
    if (texel.x > 0)
    {
        weights.x = min(weights.x, unpack_edges(edges.Load(int3(texel + int2(-1, 0), 0))).y);
    }
    else
    {
        weights.x = 0.0;
    }
    if (texel.x < last.x)
    {
        weights.y = min(weights.y, unpack_edges(edges.Load(int3(texel + int2(1, 0), 0))).x);
    }
    else
    {
        weights.y = 0.0;
    }
    if (texel.y > 0)
    {
        weights.z = min(weights.z, unpack_edges(edges.Load(int3(texel + int2(0, -1), 0))).w);
    }
    else
    {
        weights.z = 0.0;
    }
    if (texel.y < last.y)
    {
        weights.w = min(weights.w, unpack_edges(edges.Load(int3(texel + int2(0, 1), 0))).z);
    }
    else
    {
        weights.w = 0.0;
    }

    float sum = source.Load(int3(texel, 0));
    float total = 1.0;
    const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll] for (uint side = 0; side < 4; ++side)
    {
        if (weights[side] > 0.0)
        {
            sum += weights[side] * source.Load(int3(texel + offsets[side], 0));
            total += weights[side];
        }
    }
    // The diagonals, through both of their orthogonal neighbours, at half weight
    const float4 diagonal =
        0.5 * float4(weights.x * weights.z, weights.y * weights.z, weights.x * weights.w, weights.y * weights.w);
    const int2 corners[4] = { int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
    [unroll] for (uint corner = 0; corner < 4; ++corner)
    {
        if (diagonal[corner] > 0.0)
        {
            sum += diagonal[corner] * source.Load(int3(texel + corners[corner], 0));
            total += diagonal[corner];
        }
    }
    destination[thread.xy] = sum / total;
}
