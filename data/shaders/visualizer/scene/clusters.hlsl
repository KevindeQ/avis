// @avis-shader entry=build_main stage=cs entry=cull_main stage=cs
//
// Clustered light culling (docs/design/11-renderer.md §6.3, §2.3 pass 3, docs/tasks/11-renderer.md §11.11), on async
// compute. One thread per cluster of the 16 × 9 × 24 grid (lighting.hlsli).
//   build_main  cluster_build: the view-space bounds of every cluster, from the unjittered projection. Run only when
//               the projection or the view size changes; the bounds live in the view's history.
//   cull_main   light_cull: the point and spot lights touching each cluster, at most cluster_maximum_lights, in light
//               order (so the frame is reproducible). Clusters whose depth slice is behind the depth range of their
//               tile (from depth_mips, min in R and max in G) get no lights; so do those in front of it, and tiles of
//               sky only, unless keep_front is set because the frame draws transparents (11.14). Directional lights
//               aren't clustered: the sun is lit separately.
// clusters.cpp mirrors the bounds and the intersection tests on the CPU for the tests.

#include "bindless.hlsli"
#include "lighting.hlsli"
#include "shared_types.hlsli"

static const float infinity = asfloat(0x7F800000u);

// ---- cluster_build ----

AVIS_PASS_CONSTANTS(cluster_build_constants, build);

// The view-space point at linear depth `depth` on the ray through pixel position `pixel`
float3 tile_corner(float2 pixel, float depth)
{
    const float2 ndc = float2(pixel.x / float(build.size.x) * 2.0 - 1.0, 1.0 - pixel.y / float(build.size.y) * 2.0);
    const float4 near_point = mul(build.projection_inverse, float4(ndc, 1.0, 1.0)); // reverse-Z: the near plane is 1
    const float3 on_ray = near_point.xyz / near_point.w;
    return on_ray * (depth / -on_ray.z);
}

[numthreads(64, 1, 1)] void build_main(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x;
    if (index >= cluster_count)
    {
        return;
    }
    RWStructuredBuffer<cluster_bounds> bounds = ResourceDescriptorHeap[build.bounds];

    const uint3 cluster =
        uint3(index % cluster_count_x, (index / cluster_count_x) % cluster_count_y, index / (cluster_count_x * cluster_count_y));
    const uint2 first = cluster.xy * build.tile_size;
    const uint2 last = min(first + build.tile_size, build.size);

    cluster_bounds result;
    result.padding0 = 0.0;
    result.padding1 = 0.0;
    if (any(first >= build.size))
    {
        // A tile past the view's edge: empty bounds, which nothing intersects
        result.minimum = float3(infinity, infinity, infinity);
        result.maximum = -result.minimum;
        bounds[index] = result;
        return;
    }

    const float depths[2] = { cluster_slice_depth(cluster.z), cluster_slice_depth(cluster.z + 1) };
    const float2 corners[4] = { float2(first), float2(last.x, first.y), float2(first.x, last.y), float2(last) };
    result.minimum = float3(infinity, infinity, infinity);
    result.maximum = -result.minimum;
    [unroll] for (uint plane = 0; plane < 2; ++plane)
    {
        [unroll] for (uint corner = 0; corner < 4; ++corner)
        {
            const float3 corner_point = tile_corner(corners[corner], depths[plane]);
            result.minimum = min(result.minimum, corner_point);
            result.maximum = max(result.maximum, corner_point);
        }
    }
    bounds[index] = result;
}

// ---- light_cull ----

struct light_cull_constants
{
    uint bounds; // StructuredBuffer<cluster_bounds>
    uint lights; // StructuredBuffer<light_data>
    uint light_count;
    uint depth_mips; // SRV, RG32F min/max chain
    uint depth_mip_count;
    uint cluster_lights; // UAV, RWStructuredBuffer<cluster_lights>
    uint light_indices; // UAV, RWStructuredBuffer<uint>
    uint keep_front; // 1: keep the clusters in front of the tile's opaque depth, where transparents can be (11.14)
    uint2 size; // the view's size, pixels
    uint2 tile_size; // pixels per cluster tile
};

AVIS_DRAW_CONSTANTS(light_cull_constants, cull);
AVIS_VIEW_CONSTANTS(view_constants, view);

// Whether a sphere touches an axis-aligned box
bool sphere_overlaps_bounds(float3 centre, float radius, cluster_bounds box)
{
    const float3 outside = max(max(box.minimum - centre, centre - box.maximum), 0.0);
    return dot(outside, outside) <= radius * radius;
}

// Whether a cone (apex, unit axis, length, half angle as cos and sin) touches a sphere (Wronski 2017)
bool cone_overlaps_sphere(float3 apex, float3 axis, float cone_length, float cos_angle, float sin_angle, float3 centre,
                          float radius)
{
    const float3 offset = centre - apex;
    const float offset_squared = dot(offset, offset);
    const float along = dot(offset, axis);
    const float closest = cos_angle * sqrt(max(offset_squared - along * along, 0.0)) - along * sin_angle;
    return !(closest > radius || along > radius + cone_length || along < -radius);
}

// The min and max linear depth drawn in a tile, from the coarsest depth_mips level whose texels fit in the tile
float2 tile_depth_range(uint2 first, uint2 last)
{
    Texture2D<float2> depth_mips = ResourceDescriptorHeap[cull.depth_mips];
    const uint smallest = max(min(last.x - first.x, last.y - first.y), 1u);
    const uint mip = min(firstbithigh(smallest), cull.depth_mip_count - 1);
    const uint2 mip_size = max(cull.size >> mip, uint2(1, 1));
    const uint2 texel_first = min(first >> mip, mip_size - 1);
    const uint2 texel_last = min((last - 1) >> mip, mip_size - 1);

    float2 range = float2(infinity, -infinity);
    for (uint y = texel_first.y; y <= texel_last.y; ++y)
    {
        for (uint x = texel_first.x; x <= texel_last.x; ++x)
        {
            const float2 texel = depth_mips.Load(int3(x, y, mip));
            range = float2(min(range.x, texel.x), max(range.y, texel.y));
        }
    }
    return range;
}

[numthreads(64, 1, 1)] void cull_main(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x;
    if (index >= cluster_count)
    {
        return;
    }
    StructuredBuffer<cluster_bounds> all_bounds = ResourceDescriptorHeap[cull.bounds];
    StructuredBuffer<light_data> lights = ResourceDescriptorHeap[cull.lights];
    RWStructuredBuffer<cluster_lights> clusters = ResourceDescriptorHeap[cull.cluster_lights];
    RWStructuredBuffer<uint> light_indices = ResourceDescriptorHeap[cull.light_indices];

    const uint3 cluster =
        uint3(index % cluster_count_x, (index / cluster_count_x) % cluster_count_y, index / (cluster_count_x * cluster_count_y));
    const uint2 first = cluster.xy * cull.tile_size;
    const uint offset = index * cluster_maximum_lights;

    cluster_lights result;
    result.offset = offset;
    result.count = 0;
    result.dropped = 0;
    result.padding0 = 0;

    const cluster_bounds box = all_bounds[index];
    bool skipped = any(first >= cull.size) || box.minimum.x > box.maximum.x;
    if (!skipped)
    {
        // The slice's depth interval: slice 0 starts at the camera and the last one has no end, as cluster_slice clamps
        const uint2 last = min(first + cull.tile_size, cull.size);
        const float2 drawn = tile_depth_range(first, last);
        const float slice_near = cluster.z == 0 ? 0.0 : cluster_slice_depth(cluster.z);
        const float slice_far = cluster.z == cluster_count_z - 1 ? infinity : cluster_slice_depth(cluster.z + 1);
        // Behind every opaque surface of the tile, nothing is visible. In front of them only transparents can be,
        // so those clusters are skipped unless the frame draws transparents; then a tile of sky only keeps them too.
        const bool behind = drawn.y < slice_near;
        const bool in_front = isinf(drawn.x) || drawn.x > slice_far;
        skipped = behind || (in_front && cull.keep_front == 0);
    }

    if (!skipped)
    {
        const float3 box_centre = 0.5 * (box.minimum + box.maximum);
        const float box_radius = length(0.5 * (box.maximum - box.minimum));
        for (uint light_index = 0; light_index < cull.light_count; ++light_index)
        {
            const light_data light = lights[light_index];
            if (light.kind == light_kind_directional)
            {
                continue;
            }
            const float3 centre = mul(view.view, float4(light.position, 1.0)).xyz;
            if (!sphere_overlaps_bounds(centre, light.range, box))
            {
                continue;
            }
            if (light.kind == light_kind_spot && light.spot_cos_outer > 0.0)
            {
                const float3 axis = normalize(mul((float3x3)view.view, light.direction));
                const float sin_angle = sqrt(saturate(1.0 - light.spot_cos_outer * light.spot_cos_outer));
                if (!cone_overlaps_sphere(centre, axis, light.range, light.spot_cos_outer, sin_angle, box_centre,
                                          box_radius))
                {
                    continue;
                }
            }
            if (result.count < cluster_maximum_lights)
            {
                light_indices[offset + result.count] = light_index;
                ++result.count;
            }
            else
            {
                ++result.dropped;
            }
        }
    }
    clusters[index] = result;
}
