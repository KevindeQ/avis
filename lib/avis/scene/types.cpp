#include "avis/scene/types.h"

#include <Eigen/SVD>

#include <cmath>

namespace avis::scene
{
    namespace
    {
        float srgb_to_linear(const std::uint8_t value) noexcept
        {
            const float encoded = static_cast<float>(value) / 255.0f;
            if (encoded <= 0.04045f)
            {
                return encoded / 12.92f;
            }
            return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }
    } // namespace

    color color::from_srgb8(const std::array<std::uint8_t, 4> rgba) noexcept
    {
        return color{
            .red = srgb_to_linear(rgba[0]),
            .green = srgb_to_linear(rgba[1]),
            .blue = srgb_to_linear(rgba[2]),
            .alpha = static_cast<float>(rgba[3]) / 255.0f,
        };
    }

    Eigen::Affine3f transform::to_affine() const noexcept
    {
        Eigen::Affine3f affine = Eigen::Affine3f::Identity();
        affine.linear() = rotation.toRotationMatrix() * scale.asDiagonal();
        affine.translation() = position;
        return affine;
    }

    transform transform::from_affine(const Eigen::Affine3f& affine) noexcept
    {
        // Polar decomposition through the SVD, in double so that a round trip loses only the float matrix's precision:
        // linear = U Σ Vᵀ = (U Vᵀ)(V Σ Vᵀ) = R · S, with R the rotation closest to linear
        const Eigen::Matrix3d linear = affine.linear().cast<double>();
        const Eigen::JacobiSVD<Eigen::Matrix3d, Eigen::ComputeFullU | Eigen::ComputeFullV> svd{ linear };

        Eigen::Matrix3d u = svd.matrixU();
        const Eigen::Matrix3d v = svd.matrixV();
        if ((u * v.transpose()).determinant() < 0.0)
        {
            // A reflection: flip the axis of the smallest singular value, so R is a proper rotation and S carries the
            // mirror (JacobiSVD sorts the singular values in decreasing order)
            u.col(2) = -u.col(2);
        }
        const Eigen::Matrix3d rotation = u * v.transpose();

        // For a fixed R, the diagonal scale closest to linear (Frobenius norm) is the diagonal of Rᵀ · linear = S
        const Eigen::Vector3d scale = (rotation.transpose() * linear).diagonal();

        Eigen::Quaterniond quaternion{ rotation };
        quaternion.normalize();

        return transform{
            .position = affine.translation(),
            .rotation = quaternion.cast<float>(),
            .scale = scale.cast<float>(),
        };
    }

    bool node_handle::valid() const noexcept
    {
        return index != std::numeric_limits<std::uint32_t>::max();
    }
} // namespace avis::scene
