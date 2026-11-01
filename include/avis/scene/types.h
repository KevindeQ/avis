#ifndef AVIS_SCENE_TYPES_H
#define AVIS_SCENE_TYPES_H

// Common scene types (docs/design/08-scene-management.md §9.1): entity and object ids, entity categories, linear
// colours, TRS transforms and scene graph node handles.
//
// Eigen alignment rule (docs/tasks/10-scene-management.md, Conventions). Scene types and components hold fixed-size
// vectorisable Eigen types (Affine3f, Quaternionf, Vector4f) by value, with Eigen's default static alignment. They
// need no EIGEN_MAKE_ALIGNED_OPERATOR_NEW and no unaligned variants: C++17 aligned new gives every heap allocation
// the type's alignment, through std::allocator in std::vector and in EnTT's pools alike. The build must not define
// EIGEN_DONT_ALIGN, EIGEN_DONT_ALIGN_STATICALLY or EIGEN_MAX_STATIC_ALIGN_BYTES; the static_asserts below catch a
// configuration where this rule doesn't hold.

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <entt/entity/fwd.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>

static_assert(EIGEN_HAS_CXX17_OVERALIGN, "scene types rely on C++17 aligned new for Eigen members");
static_assert(__cpp_aligned_new >= 201606L, "scene types rely on C++17 aligned new for Eigen members");

namespace avis::scene
{
    using entity = entt::entity; // runtime only; never stored across runs or sent anywhere

    // identity.id: the Driver's entity_id (02 §5.7) for streamed entities; helpers use source 0xFF in bits 63–56
    using object_id = std::uint64_t;
    inline constexpr std::uint64_t helper_source = 0xFF;

    constexpr bool is_helper(const object_id id) noexcept
    {
        return (id >> 56) == helper_source;
    }

    // The Driver's entity categories (02 §5.8.1), plus helpers
    enum class entity_category : std::uint8_t
    {
        group,
        vehicle,
        sensor,
        object,
        geometry,
        helper
    };
    inline constexpr std::size_t entity_category_count = 6;
    static_assert(static_cast<std::size_t>(entity_category::helper) + 1 == entity_category_count);

    // Linear RGBA; conversions from the protocol's sRGB u8x4
    struct color
    {
        float red = 1.0f;
        float green = 1.0f;
        float blue = 1.0f;
        float alpha = 1.0f;

        // Exact sRGB transfer function (IEC 61966-2-1) on red, green and blue; alpha is linear
        static color from_srgb8(std::array<std::uint8_t, 4> rgba) noexcept;

        friend bool operator==(const color&, const color&) = default;
    };

    // TRS, composed as T · R · S (08 §4.1)
    struct transform
    {
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
        Eigen::Vector3f scale = Eigen::Vector3f::Ones();

        // Translation · Rotation · Scaling
        Eigen::Affine3f to_affine() const noexcept;

        // The closest TRS (08 §4, reparent with keep_world). The rotation is the one closest to the linear part (the
        // polar decomposition's), normalised; the scale is the diagonal scale that best fits the linear part for that
        // rotation. Both are exact when the linear part has no shear, and the scale is then positive. A mirroring
        // matrix keeps its reflection in the scale, as no rotation can carry it.
        static transform from_affine(const Eigen::Affine3f& affine) noexcept;
    };

    struct node_handle
    {
        std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;

        // Not default; says nothing about being alive
        bool valid() const noexcept;

        friend auto operator<=>(const node_handle&, const node_handle&) = default;
    };
} // namespace avis::scene

#endif
