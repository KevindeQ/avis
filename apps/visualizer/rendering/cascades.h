#ifndef AVIS_VISUALIZER_RENDERING_CASCADES_H
#define AVIS_VISUALIZER_RENDERING_CASCADES_H

// The sun's cascaded shadow maps, CPU side (docs/design/11-renderer.md §7, docs/tasks/11-renderer.md §11.12): the
// splits, the cascades' viewpoints and the constants the receiver reads.
//
// - Splits: the practical scheme, c_i = λ·n·(f/n)^(i/N) + (1 − λ)·(n + (f − n)·i/N), from the camera's near plane n to
//   f = shadows.distance (or the camera's far plane when that is nearer), λ = 0.8. λ = 0 is uniform.
// - Fitting: each cascade bounds its slice of the camera frustum with the slice's smallest enclosing sphere, centred on
//   the view axis. The radius depends only on the projection and the split depths, so it doesn't change when the
//   camera moves or turns.
// - Stabilisation: the light's view has a fixed rotation (light_rotation), and each cascade's origin is the sphere's
//   centre in light space with x and y rounded to whole texels. Moving the camera moves the origins by whole texels
//   only, so the texels the scene falls into don't shimmer.
// - Projection: orthographic and reverse-Z, 2r × 2r. Depth 1 is caster_extension metres beyond the sphere towards the
//   sun (casters there still shadow it), depth 0 the sphere's far side. The shadow PSO disables depth clipping, so
//   casters nearer still are flattened onto depth 1 ("pancaking").
// - Blending: the receiver blends a cascade into the next over the last blend_fraction (10%) of its depth range, and
//   the last cascade into "lit".

#include "rendering/draw_lists.h"
#include "rendering/render_settings.h"
#include "rendering/sort_key.h"
#include "rendering/view.h"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <span>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // The shadow PSO's slope-scaled depth bias, as a magnitude: reverse-Z applies it negated, away from the sun
    inline constexpr float shadow_slope_scaled_depth_bias = 1.5f;

    // The receiver's normal offset: the position moves along its normal by this many texels of its cascade, times
    // sin(θ) of the angle between the normal and the sun
    inline constexpr float shadow_normal_offset_texels = 1.0f;

    // The radius of the 16-tap Poisson disk of the receiver's PCF, in texels
    inline constexpr float shadow_filter_radius_texels = 1.0f;

    struct cascade_parameters
    {
        std::uint32_t count = 4; // 1 to maximum_shadow_cascades
        std::uint32_t resolution = 2048; // texels per side
        float distance = 200.0f; // metres of view depth the last cascade ends at
        float split_lambda = 0.8f; // 0 uniform, 1 logarithmic
        float blend_fraction = 0.1f; // of each cascade's depth range, blended into the next
        float caster_extension = 200.0f; // metres towards the sun beyond each cascade's sphere
    };

    // render.shadows (11 §14) with the fixed parameters of 11 §7
    cascade_parameters cascade_parameters_of(const shadow_settings& settings) noexcept;

    // The split depths: splits[0] = near, splits[count] = distance, splits[i] the practical split i. Entries past
    // `count` are `distance`.
    std::array<float, maximum_shadow_cascades + 1>
        cascade_splits(float near, float distance, std::uint32_t count, float lambda) noexcept;

    // The smallest sphere centred on the view axis that holds the frustum slice between view depths near and far, for a
    // frustum whose image half-extents at depth 1 are tan_half_x and tan_half_y
    struct slice_sphere
    {
        float centre_depth = 0.0f; // view depth of the centre
        float radius = 0.0f;
    };

    slice_sphere slice_bounding_sphere(float near, float far, float tan_half_x, float tan_half_y) noexcept;

    // The fixed rotation of the light's view for a sun shining along `direction` (unit length, Z up): rows are the
    // light-space x, y and z axes in world space, z pointing towards the sun (the view looks down −Z, along
    // `direction`). y is world up projected, or world Y when the sun is near vertical.
    Eigen::Matrix3f light_rotation(const Eigen::Vector3f& direction) noexcept;

    struct shadow_cascade
    {
        cascade_viewpoint viewpoint; // for the draw lists and the shadow pass
        Eigen::Matrix4f shadow_from_world = Eigen::Matrix4f::Identity(); // viewpoint.projection · view_from_world
        float near_depth = 0.0f; // the slice's view depths
        float far_depth = 0.0f;
        float blend_start = 0.0f; // view depth where the blend into the next cascade starts
        float radius = 0.0f; // of the bounding sphere, metres
        float texel_size = 0.0f; // metres per texel: 2r / resolution
        Eigen::Vector2f origin = Eigen::Vector2f::Zero(); // light-space x and y of the cascade's centre, snapped
        Eigen::Vector2f unsnapped_origin = Eigen::Vector2f::Zero(); // the same before snapping
        float depth_near = 0.0f; // light-space z of depth 1 (towards the sun) and of depth 0, metres
        float depth_far = 0.0f;
    };

    struct shadow_cascades
    {
        std::array<shadow_cascade, maximum_shadow_cascades> cascades{};
        std::array<cascade_viewpoint, maximum_shadow_cascades> viewpoints{}; // cascades[i].viewpoint
        std::uint32_t count = 0;
        std::uint32_t resolution = 0;
        Eigen::Matrix3f light_from_world = Eigen::Matrix3f::Identity(); // light_rotation
        Eigen::Vector3f direction = -Eigen::Vector3f::UnitZ(); // the sun's, unit length

        std::span<const shadow_cascade> used() const noexcept
        {
            return std::span{ cascades }.first(count);
        }

        std::span<const cascade_viewpoint> used_viewpoints() const noexcept
        {
            return std::span{ viewpoints }.first(count);
        }
    };

    // The cascades of `camera` (its view for `aspect_ratio`) under a sun shining along `direction`. A zero direction
    // shines straight down.
    shadow_cascades compute_cascades(
        const rendering::camera& camera,
        float aspect_ratio,
        const Eigen::Vector3f& direction,
        const cascade_parameters& parameters);

    // The receiver's constants (shared_types.hlsli); shadow_map is the shadow map's SRV, filled in when recording
    gpu::shadow_constants make_shadow_constants(const shadow_cascades& cascades, std::uint32_t shadow_map) noexcept;

    // The view constants a shadow pass draws cascade `index` with: its view and projection as both the jittered and
    // the unjittered matrices (no jitter, no motion), a near plane of 0 (orthographic: clip w is 1) and the map's size
    gpu::view_constants
        make_cascade_view_constants(const shadow_cascades& cascades, std::uint32_t index, std::uint32_t frame_index);
} // namespace avis::visualizer::rendering

#endif
