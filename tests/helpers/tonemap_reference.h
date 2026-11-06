#ifndef AVIS_TESTS_HELPERS_TONEMAP_REFERENCE_H
#define AVIS_TESTS_HELPERS_TONEMAP_REFERENCE_H

// CPU references of the renderer's tonemap (data/shaders/visualizer/scene/tonemap.hlsl, docs/design/11-renderer.md
// §10.4, docs/tasks/11-renderer.md §11.16), in double precision: ACES fitted (Hill), AgX (Sobotka, with the polynomial
// fit of its default contrast), and the exact sRGB transfer function. They take the exposed linear colour the shader
// maps, after its rescale by the exposure change.

#include "rendering/render_settings.h"

#include <Eigen/Core>

#include <algorithm>
#include <cmath>

namespace avis::tests
{
    // The sRGB encoding (IEC 61966-2-1) of a linear value, clamped to [0, 1]
    inline double srgb_encode(double value)
    {
        value = std::clamp(value, 0.0, 1.0);
        return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    }

    inline Eigen::Vector3d aces_fitted(const Eigen::Vector3d& color)
    {
        Eigen::Matrix3d input;
        input << 0.59719, 0.35458, 0.04823, //
            0.07600, 0.90834, 0.01566, //
            0.02840, 0.13383, 0.83777;
        Eigen::Matrix3d output;
        output << 1.60475, -0.53108, -0.07367, //
            -0.10208, 1.10813, -0.00605, //
            -0.00327, -0.07276, 1.07602;
        const Eigen::Vector3d v = input * color;
        const Eigen::Vector3d a =
            v.cwiseProduct(v + Eigen::Vector3d::Constant(0.0245786)) - Eigen::Vector3d::Constant(0.000090537);
        const Eigen::Vector3d b =
            v.cwiseProduct(0.983729 * v + Eigen::Vector3d::Constant(0.4329510)) + Eigen::Vector3d::Constant(0.238081);
        return (output * a.cwiseQuotient(b)).cwiseMax(0.0).cwiseMin(1.0);
    }

    inline Eigen::Vector3d agx(const Eigen::Vector3d& color)
    {
        // Listed by columns, as the reference implementation's GLSL mat3 constructors
        Eigen::Matrix3d inset_columns;
        inset_columns << 0.842479062253094, 0.0423282422610123, 0.0423756549057051, //
            0.0784335999999992, 0.878468636469772, 0.0784336, //
            0.0792237451477643, 0.0791661274605434, 0.879142973793104;
        Eigen::Matrix3d outset_columns;
        outset_columns << 1.19687900512017, -0.0528968517574562, -0.0529716355144438, //
            -0.0980208811401368, 1.15190312990417, -0.0980434501171241, //
            -0.0990297440797205, -0.0989611768448433, 1.15107367264116;
        constexpr double minimum_stops = -12.47393;
        constexpr double maximum_stops = 4.026069;

        const Eigen::Vector3d inset = inset_columns.transpose() * color;
        Eigen::Vector3d display;
        for (int channel = 0; channel < 3; ++channel)
        {
            const double stops = std::clamp(std::log2(std::max(inset[channel], 1.0e-10)), minimum_stops, maximum_stops);
            const double x = (stops - minimum_stops) / (maximum_stops - minimum_stops);
            const double x2 = x * x;
            const double x4 = x2 * x2;
            display[channel] =
                15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
        }
        const Eigen::Vector3d outset = outset_columns.transpose() * display;
        Eigen::Vector3d linear;
        for (int channel = 0; channel < 3; ++channel)
        {
            linear[channel] = std::clamp(std::pow(std::max(outset[channel], 0.0), 2.2), 0.0, 1.0);
        }
        return linear;
    }

    // The 8-bit value the tonemap aims for, before rounding and the ±0.5 LSB dither: in [0, 255]
    inline Eigen::Vector3d
        tonemapped_lsb(const Eigen::Vector3d& exposed, const visualizer::rendering::tonemap_operator tonemapper)
    {
        const Eigen::Vector3d mapped =
            tonemapper == visualizer::rendering::tonemap_operator::agx ? agx(exposed) : aces_fitted(exposed);
        return { 255.0 * srgb_encode(mapped.x()), 255.0 * srgb_encode(mapped.y()), 255.0 * srgb_encode(mapped.z()) };
    }
} // namespace avis::tests

#endif
