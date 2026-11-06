#ifndef AVIS_LIGHTING_HLSLI
#define AVIS_LIGHTING_HLSLI

// Lights and clusters (docs/design/11-renderer.md §6.2, §6.3, docs/tasks/11-renderer.md §11.11).
// - Directional lights: `intensity` is the illuminance in lux, perpendicular to the light.
// - Point and spot lights: `intensity` is the luminous intensity in candela (the snapshot's unit, 08 §6.1), so the
//   illuminance at distance d is I / d², windowed by (1 - (d / range)^4)² so it reaches exactly 0 at the range. Spot
//   lights add a smooth angular attenuation between the inner and the outer cone.
// - The cluster grid: clusters.h mirrors these functions on the CPU, for the tests.

#include "brdf.hlsli"
#include "shared_types.hlsli"

// ---- clusters ----

// The depth slice of a linear depth (metres): exponential from cluster_near to cluster_far, clamped to the grid
uint cluster_slice(float linear_depth)
{
    if (!(linear_depth > cluster_near))
    {
        return 0;
    }
    const float slice = log(linear_depth / cluster_near) * (float(cluster_count_z) / log(cluster_far / cluster_near));
    return min(uint(slice), cluster_count_z - 1);
}

// The linear depth where slice `slice` starts (slice cluster_count_z: cluster_far)
float cluster_slice_depth(uint slice)
{
    return cluster_near * pow(cluster_far / cluster_near, float(slice) / float(cluster_count_z));
}

uint cluster_index(uint3 cluster)
{
    return cluster.x + cluster_count_x * (cluster.y + cluster_count_y * cluster.z);
}

// The cluster of a pixel at a linear depth
uint3 pixel_cluster(uint2 pixel, uint2 tile_size, float linear_depth)
{
    const uint2 tile = min(pixel / tile_size, uint2(cluster_count_x - 1, cluster_count_y - 1));
    return uint3(tile, cluster_slice(linear_depth));
}

// ---- lights ----

// The inverse-square falloff with the smooth window (1 - (d / range)^4)², from the squared distance
float distance_falloff(float distance_squared, float range)
{
    const float ratio2 = distance_squared / max(range * range, 1e-8);
    const float window = saturate(1.0 - ratio2 * ratio2);
    return window * window / max(distance_squared, 1e-4);
}

// A spot light's angular attenuation towards a surface `to_light` (unit, from the surface towards the light)
float spot_attenuation(light_data light, float3 to_light)
{
    const float cos_angle = dot(-to_light, light.direction);
    const float t =
        saturate((cos_angle - light.spot_cos_outer) / max(light.spot_cos_inner - light.spot_cos_outer, 1e-4));
    return t * t;
}

// The radiance a point or spot light reflects off `s` towards `to_view`
float3 evaluate_punctual_light(light_data light, surface s, float3 to_view)
{
    const float3 offset = light.position - s.position;
    const float distance_squared = dot(offset, offset);
    const float3 to_light = offset * rsqrt(max(distance_squared, 1e-8));
    float attenuation = distance_falloff(distance_squared, light.range);
    if (light.kind == light_kind_spot)
    {
        attenuation *= spot_attenuation(light, to_light);
    }
    if (attenuation <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    return evaluate_brdf(s, to_view, to_light) * light.color * (light.intensity * attenuation);
}

// The radiance a directional light reflects off `s` towards `to_view`; `shadow` is 1 where lit
float3 evaluate_directional_light(light_data light, surface s, float3 to_view, float shadow)
{
    return evaluate_brdf(s, to_view, -light.direction) * light.color * (light.intensity * shadow);
}

#endif
