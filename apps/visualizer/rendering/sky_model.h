#ifndef AVIS_VISUALIZER_RENDERING_SKY_MODEL_H
#define AVIS_VISUALIZER_RENDERING_SKY_MODEL_H

// The analytic sky's coefficients (docs/design/11-renderer.md §6.4, docs/tasks/11-renderer.md §11.11), computed on the
// CPU from the snapshot's environment: the sun's direction and the turbidity, with a fixed ground albedo. The model is
// Preetham, Shirley and Smits 1999, "A Practical Analytic Model for Daylight", the alternative the task's Open point
// names: Hosek-Wilkie needs its authors' coefficient tables vendored first. Both the environment bake and the sky pass
// evaluate the result with sky.hlsli; sky_radiance here mirrors that function for the tests.
//
// Preetham is valid for turbidity 2 to 10 and the sun above the horizon. The turbidity is clamped to that range. A sun
// below the horizon is evaluated at the horizon, and the sky's luminance fades to 0 by 0.1 below it (sin of the
// elevation). Below the horizon the sky is a uniform ground: ground albedo · (sun illuminance · sin(elevation) / π +
// zenith radiance), a rough estimate of a lit Lambertian ground.

#include "avis/scene/snapshot.h"

#include <Eigen/Core>

#include <optional>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    inline constexpr float sky_ground_albedo = 0.3f;
    inline constexpr float sky_minimum_turbidity = 2.0f;
    inline constexpr float sky_maximum_turbidity = 10.0f;

    // What the environment bake bakes: the snapshot's environment, or a uniform radiance in every direction instead of
    // the sky (scene_renderer::set_environment_override, for tests and tools)
    struct environment_description
    {
        scene::environment_settings settings;
        std::optional<Eigen::Vector3f> uniform_radiance; // linear RGB, cd/m²

        friend bool operator==(const environment_description& left, const environment_description& right) noexcept;
    };

    gpu::sky_constants make_sky_constants(const environment_description& environment);

    // The radiance (linear RGB, cd/m²) arriving from world direction `direction` (unit length, Z up): sky.hlsli's
    // sky_radiance
    Eigen::Vector3f sky_radiance(const gpu::sky_constants& sky, const Eigen::Vector3f& direction);
} // namespace avis::visualizer::rendering

#endif
