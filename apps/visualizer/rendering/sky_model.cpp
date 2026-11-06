#include "rendering/sky_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace avis::visualizer::rendering
{
    namespace
    {
        gpu::float3 to_gpu(const Eigen::Vector3f& value) noexcept
        {
            return { value.x(), value.y(), value.z() };
        }

        Eigen::Vector3f from_gpu(const gpu::float3& value) noexcept
        {
            return { value.x, value.y, value.z };
        }

        // Preetham's Perez coefficients A to E, linear in the turbidity: { slope, offset } per coefficient
        struct linear_fit
        {
            float slope;
            float offset;
        };

        constexpr std::array<linear_fit, 5> luminance_fit{ { { 0.1787f, -1.4630f },
                                                             { -0.3554f, 0.4275f },
                                                             { -0.0227f, 5.3251f },
                                                             { 0.1206f, -2.5771f },
                                                             { -0.0670f, 0.3703f } } };
        constexpr std::array<linear_fit, 5> x_fit{ { { -0.0193f, -0.2592f },
                                                     { -0.0665f, 0.0008f },
                                                     { -0.0004f, 0.2125f },
                                                     { -0.0641f, -0.8989f },
                                                     { -0.0033f, 0.0452f } } };
        constexpr std::array<linear_fit, 5> y_fit{ { { -0.0167f, -0.2608f },
                                                     { -0.0950f, 0.0092f },
                                                     { -0.0079f, 0.2102f },
                                                     { -0.0441f, -1.6537f },
                                                     { -0.0109f, 0.0529f } } };

        // The zenith chromaticity's cubic in the sun's zenith angle, per power of the turbidity (T², T, 1)
        constexpr std::array<std::array<float, 4>, 3> zenith_x{ { { 0.00166f, -0.00375f, 0.00209f, 0.0f },
                                                                  { -0.02903f, 0.06377f, -0.03202f, 0.00394f },
                                                                  { 0.11693f, -0.21196f, 0.06052f, 0.25886f } } };
        constexpr std::array<std::array<float, 4>, 3> zenith_y{ { { 0.00275f, -0.00610f, 0.00317f, 0.0f },
                                                                  { -0.04214f, 0.08970f, -0.04153f, 0.00516f },
                                                                  { 0.15346f, -0.26756f, 0.06670f, 0.26688f } } };

        float zenith_chromaticity(
            const std::array<std::array<float, 4>, 3>& fit, const float turbidity, const float theta)
        {
            const auto cubic = [theta](const std::array<float, 4>& c)
            { return ((c[0] * theta + c[1]) * theta + c[2]) * theta + c[3]; };
            return turbidity * turbidity * cubic(fit[0]) + turbidity * cubic(fit[1]) + cubic(fit[2]);
        }

        Eigen::Vector3f
            perez(const gpu::sky_constants& sky, const float cos_theta, const float gamma, const float cos_gamma)
        {
            const float clamped = std::max(cos_theta, 1e-3f);
            const Eigen::Vector3f a = from_gpu(sky.perez_a);
            const Eigen::Vector3f b = from_gpu(sky.perez_b);
            const Eigen::Vector3f c = from_gpu(sky.perez_c);
            const Eigen::Vector3f d = from_gpu(sky.perez_d);
            const Eigen::Vector3f e = from_gpu(sky.perez_e);
            Eigen::Vector3f result;
            for (int channel = 0; channel < 3; ++channel)
            {
                result[channel] =
                    (1.0f + a[channel] * std::exp(b[channel] / clamped)) *
                    (1.0f + c[channel] * std::exp(d[channel] * gamma) + e[channel] * cos_gamma * cos_gamma);
            }
            return result;
        }

        Eigen::Vector3f xyy_to_rgb(const float x, const float y, const float luminance)
        {
            const float safe_y = std::max(y, 1e-4f);
            const Eigen::Vector3f xyz{ x / safe_y * luminance, luminance, (1.0f - x - y) / safe_y * luminance };
            return { 3.2406f * xyz.x() - 1.5372f * xyz.y() - 0.4986f * xyz.z(),
                     -0.9689f * xyz.x() + 1.8758f * xyz.y() + 0.0415f * xyz.z(),
                     0.0557f * xyz.x() - 0.2040f * xyz.y() + 1.0570f * xyz.z() };
        }
    } // namespace

    bool operator==(const environment_description& left, const environment_description& right) noexcept
    {
        const scene::environment_settings& a = left.settings;
        const scene::environment_settings& b = right.settings;
        return a.sun_direction == b.sun_direction && a.sun_illuminance == b.sun_illuminance &&
               a.sky_turbidity == b.sky_turbidity && a.ambient_intensity == b.ambient_intensity &&
               left.uniform_radiance == right.uniform_radiance;
    }

    gpu::sky_constants make_sky_constants(const environment_description& environment)
    {
        gpu::sky_constants sky{};
        if (environment.uniform_radiance)
        {
            sky.mode = gpu::sky_mode_uniform;
            sky.uniform_radiance = to_gpu(*environment.uniform_radiance);
            sky.ground_radiance = sky.uniform_radiance;
            sky.luminance_scale = 1.0f;
            sky.sun_position = { 0.0f, 0.0f, 1.0f };
            return sky;
        }
        sky.mode = gpu::sky_mode_model;

        // Towards the sun; the sun is kept at or above the horizon for the model
        const scene::environment_settings& settings = environment.settings;
        Eigen::Vector3f towards_sun = -settings.sun_direction;
        if (!(towards_sun.squaredNorm() > 0.0f))
        {
            towards_sun = Eigen::Vector3f::UnitZ();
        }
        towards_sun.normalize();
        const float elevation_sine = towards_sun.z();
        sky.luminance_scale = std::clamp((elevation_sine + 0.1f) / 0.1f, 0.0f, 1.0f);
        Eigen::Vector3f sun = towards_sun;
        if (sun.z() < 0.0f)
        {
            sun.z() = 0.0f;
            sun = sun.squaredNorm() > 0.0f ? sun.normalized() : Eigen::Vector3f::UnitX();
        }
        sky.sun_position = to_gpu(sun);

        const float turbidity = std::clamp(settings.sky_turbidity, sky_minimum_turbidity, sky_maximum_turbidity);
        const float theta_sun = std::acos(std::clamp(sun.z(), 0.0f, 1.0f));
        const auto coefficient = [turbidity](const std::array<linear_fit, 5>& fit, const std::size_t index)
        { return fit[index].slope * turbidity + fit[index].offset; };
        const auto channels = [&](const std::size_t index) -> gpu::float3
        { return { coefficient(luminance_fit, index), coefficient(x_fit, index), coefficient(y_fit, index) }; };
        sky.perez_a = channels(0);
        sky.perez_b = channels(1);
        sky.perez_c = channels(2);
        sky.perez_d = channels(3);
        sky.perez_e = channels(4);

        // The zenith: luminance in kcd/m², then the chromaticity
        const float chi = (4.0f / 9.0f - turbidity / 120.0f) * (std::numbers::pi_v<float> - 2.0f * theta_sun);
        const float zenith_luminance =
            1000.0f * ((4.0453f * turbidity - 4.9710f) * std::tan(chi) - 0.2155f * turbidity + 2.4192f);
        const Eigen::Vector3f zenith{ std::max(zenith_luminance, 0.0f),
                                      zenith_chromaticity(zenith_x, turbidity, theta_sun),
                                      zenith_chromaticity(zenith_y, turbidity, theta_sun) };
        const Eigen::Vector3f at_zenith = perez(sky, 1.0f, theta_sun, std::cos(theta_sun));
        sky.zenith = to_gpu(zenith.cwiseQuotient(at_zenith));

        const Eigen::Vector3f zenith_radiance = sky_radiance(sky, Eigen::Vector3f::UnitZ());
        const float sun_on_ground =
            settings.sun_illuminance * std::max(elevation_sine, 0.0f) / std::numbers::pi_v<float>;
        sky.ground_radiance = to_gpu(sky_ground_albedo * (Eigen::Vector3f::Constant(sun_on_ground) + zenith_radiance));
        return sky;
    }

    Eigen::Vector3f sky_radiance(const gpu::sky_constants& sky, const Eigen::Vector3f& direction)
    {
        if (sky.mode == gpu::sky_mode_uniform)
        {
            return from_gpu(sky.uniform_radiance);
        }
        if (direction.z() < 0.0f)
        {
            return from_gpu(sky.ground_radiance);
        }
        const float cos_gamma = std::clamp(direction.dot(from_gpu(sky.sun_position)), -1.0f, 1.0f);
        const Eigen::Vector3f value =
            from_gpu(sky.zenith).cwiseProduct(perez(sky, direction.z(), std::acos(cos_gamma), cos_gamma));
        return xyy_to_rgb(value.y(), value.z(), value.x() * sky.luminance_scale).cwiseMax(0.0f);
    }
} // namespace avis::visualizer::rendering
