#include "avis/scene/assets/mesh_builder.h"

#include "avis/detail/check.h"
#include "avis/scene/error.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <utility>

namespace avis::scene
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";

        // [-1, 1] → [0, 65535], rounded
        std::uint16_t to_unorm16(const float value) noexcept
        {
            const float clamped = std::clamp(value, -1.0f, 1.0f);
            return static_cast<std::uint16_t>(std::lround((clamped * 0.5f + 0.5f) * 65535.0f));
        }

        float from_unorm16(const std::uint16_t value) noexcept
        {
            return static_cast<float>(value) / 65535.0f * 2.0f - 1.0f;
        }

        // Sign that is +1 at zero, so the fold maps the equator consistently
        float sign_not_zero(const float value) noexcept
        {
            return value >= 0.0f ? 1.0f : -1.0f;
        }
    } // namespace

    std::array<std::uint16_t, 2> encode_octahedral(const Eigen::Vector3f& direction) noexcept
    {
        const float l1 = direction.cwiseAbs().sum();
        if (!(l1 > 0.0f) || !std::isfinite(l1))
        {
            return { to_unorm16(0.0f), to_unorm16(0.0f) }; // +Z
        }

        float x = direction.x() / l1;
        float y = direction.y() / l1;
        if (direction.z() < 0.0f)
        {
            const float folded_x = (1.0f - std::abs(y)) * sign_not_zero(x);
            const float folded_y = (1.0f - std::abs(x)) * sign_not_zero(y);
            x = folded_x;
            y = folded_y;
        }
        return { to_unorm16(x), to_unorm16(y) };
    }

    Eigen::Vector3f decode_octahedral(const std::array<std::uint16_t, 2> encoded) noexcept
    {
        Eigen::Vector3f direction{ from_unorm16(encoded[0]), from_unorm16(encoded[1]), 0.0f };
        direction.z() = 1.0f - std::abs(direction.x()) - std::abs(direction.y());
        const float fold = std::max(-direction.z(), 0.0f);
        direction.x() += direction.x() >= 0.0f ? -fold : fold;
        direction.y() += direction.y() >= 0.0f ? -fold : fold;
        return direction.normalized();
    }

    std::uint16_t encode_f16(const float value) noexcept
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
        const std::uint32_t exponent = (bits >> 23) & 0xFFu;
        std::uint32_t mantissa = bits & 0x7FFFFFu;

        if (exponent == 0xFFu)
        {
            // Infinity, or a quiet NaN keeping the top mantissa bits
            return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa != 0 ? 0x200u | (mantissa >> 13) : 0u));
        }

        const std::int32_t half_exponent = static_cast<std::int32_t>(exponent) - 127 + 15;
        if (half_exponent >= 0x1F)
        {
            return static_cast<std::uint16_t>(sign | 0x7C00u); // overflow to infinity
        }

        if (half_exponent <= 0)
        {
            // Subnormal half, or zero: shift the mantissa with its implicit bit into place, rounding to nearest even
            if (half_exponent < -10)
            {
                return sign;
            }
            mantissa |= 0x800000u;
            const std::uint32_t shift = static_cast<std::uint32_t>(14 - half_exponent);
            std::uint32_t half_mantissa = mantissa >> shift;
            const std::uint32_t remainder = mantissa & ((1u << shift) - 1u);
            const std::uint32_t halfway = 1u << (shift - 1);
            if (remainder > halfway || (remainder == halfway && (half_mantissa & 1u) != 0))
            {
                ++half_mantissa; // may carry into the smallest normal, which is the right result
            }
            return static_cast<std::uint16_t>(sign | half_mantissa);
        }

        // Normal: drop 13 mantissa bits, rounding to nearest even; a carry may bump the exponent, up to infinity
        std::uint32_t half = (static_cast<std::uint32_t>(half_exponent) << 10) | (mantissa >> 13);
        const std::uint32_t remainder = mantissa & 0x1FFFu;
        if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0))
        {
            ++half;
        }
        return static_cast<std::uint16_t>(sign | half);
    }

    float decode_f16(const std::uint16_t bits) noexcept
    {
        const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
        const std::uint32_t exponent = (bits >> 10) & 0x1Fu;
        const std::uint32_t mantissa = bits & 0x3FFu;

        if (exponent == 0)
        {
            // Zero or subnormal: mantissa · 2⁻²⁴, exact in float
            const float magnitude = std::ldexp(static_cast<float>(mantissa), -24);
            return sign != 0 ? -magnitude : magnitude;
        }
        if (exponent == 0x1F)
        {
            return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
        }
        return std::bit_cast<float>(sign | ((exponent - 15 + 127) << 23) | (mantissa << 13));
    }

    mesh_builder::mesh_builder(const mesh_topology topology, const bool progressive)
    {
        data_.topology = topology;
        data_.progressive = progressive;
    }

    mesh_builder& mesh_builder::add_vertex(const Eigen::Vector3f& position)
    {
        if (!data_.positions.empty())
        {
            finish_vertex();
        }
        data_.positions.push_back(position);
        vertex_attributes_ = avis::flags<vertex_attribute>{ avis::empty };
        return *this;
    }

    mesh_builder& mesh_builder::normal(const Eigen::Vector3f& normal)
    {
        if (has_vertex("normal"))
        {
            const auto encoded = encode_octahedral(normal);
            if (vertex_attributes_ & vertex_attribute::normal)
            {
                data_.normals.back() = encoded;
            }
            else
            {
                data_.normals.push_back(encoded);
                vertex_attributes_ |= vertex_attribute::normal;
            }
        }
        return *this;
    }

    mesh_builder& mesh_builder::tangent(const Eigen::Vector4f& tangent)
    {
        if (has_vertex("tangent"))
        {
            const auto encoded = encode_octahedral(tangent.head<3>());
            const std::int8_t sign = tangent.w() < 0.0f ? std::int8_t{ -1 } : std::int8_t{ 1 };
            if (vertex_attributes_ & vertex_attribute::tangent)
            {
                data_.tangents.back() = encoded;
                data_.tangent_signs.back() = sign;
            }
            else
            {
                data_.tangents.push_back(encoded);
                data_.tangent_signs.push_back(sign);
                vertex_attributes_ |= vertex_attribute::tangent;
            }
        }
        return *this;
    }

    mesh_builder& mesh_builder::uv(const Eigen::Vector2f& uv)
    {
        if (has_vertex("uv"))
        {
            const std::array<std::uint16_t, 2> encoded{ encode_f16(uv.x()), encode_f16(uv.y()) };
            if (vertex_attributes_ & vertex_attribute::uv0)
            {
                data_.uv0.back() = encoded;
            }
            else
            {
                data_.uv0.push_back(encoded);
                vertex_attributes_ |= vertex_attribute::uv0;
            }
        }
        return *this;
    }

    mesh_builder& mesh_builder::color(const std::array<std::uint8_t, 4> srgb)
    {
        if (has_vertex("color"))
        {
            if (vertex_attributes_ & vertex_attribute::color0)
            {
                data_.color0.back() = srgb;
            }
            else
            {
                data_.color0.push_back(srgb);
                vertex_attributes_ |= vertex_attribute::color0;
            }
        }
        return *this;
    }

    mesh_builder& mesh_builder::add_index(const std::uint32_t index)
    {
        data_.indices.push_back(index);
        return *this;
    }

    mesh_builder& mesh_builder::end_primitive()
    {
        const auto vertex_end = static_cast<std::uint32_t>(data_.positions.size());
        const auto index_end = static_cast<std::uint32_t>(data_.indices.size());
        if (vertex_end == primitive_vertex_first_ && index_end == primitive_index_first_)
        {
            return *this;
        }

        data_.primitives.push_back(mesh_primitive{
            .index_first = primitive_index_first_,
            .index_count = index_end - primitive_index_first_,
            .vertex_first = primitive_vertex_first_,
            .vertex_count = vertex_end - primitive_vertex_first_,
        });
        primitive_vertex_first_ = vertex_end;
        primitive_index_first_ = index_end;
        return *this;
    }

    void mesh_builder::reserve(const std::size_t vertices, const std::size_t indices)
    {
        data_.positions.reserve(vertices);
        data_.indices.reserve(indices);
        reserved_vertices_ = vertices;
    }

    result<mesh_data> mesh_builder::build() &&
    {
        if (!data_.positions.empty())
        {
            finish_vertex();
        }
        end_primitive();
        if (data_.primitives.empty())
        {
            data_.primitives.emplace_back();
        }
        if (mismatch_)
        {
            return std::unexpected{ make_error(
                error_kind::invalid_mesh,
                "invalid mesh: mesh_builder vertices don't all have the attributes of the first vertex") };
        }
        return std::move(data_);
    }

    void mesh_builder::finish_vertex()
    {
        if (!attributes_fixed_)
        {
            data_.attributes = vertex_attributes_;
            attributes_fixed_ = true;

            // Now the streams are known, reserve them like the positions
            if (reserved_vertices_ != 0)
            {
                if (vertex_attributes_ & vertex_attribute::normal)
                {
                    data_.normals.reserve(reserved_vertices_);
                }
                if (vertex_attributes_ & vertex_attribute::tangent)
                {
                    data_.tangents.reserve(reserved_vertices_);
                    data_.tangent_signs.reserve(reserved_vertices_);
                }
                if (vertex_attributes_ & vertex_attribute::uv0)
                {
                    data_.uv0.reserve(reserved_vertices_);
                }
                if (vertex_attributes_ & vertex_attribute::color0)
                {
                    data_.color0.reserve(reserved_vertices_);
                }
            }
            return;
        }

        if (!(vertex_attributes_ == data_.attributes) && !mismatch_)
        {
            mismatch_ = true;
            AVIS_CHECK(
                logger_name,
                false,
                "mesh_builder: a vertex's attributes differ from the first vertex's; build() will fail");
        }
    }

    bool mesh_builder::has_vertex(const char* const operation) const noexcept
    {
        if (!data_.positions.empty())
        {
            return true;
        }
        try
        {
            return AVIS_CHECK(
                logger_name, false, std::format("mesh_builder: {} called before any add_vertex", operation));
        }
        catch (...)
        {
            return AVIS_CHECK(logger_name, false, "mesh_builder: an attribute called before any add_vertex");
        }
    }
} // namespace avis::scene
