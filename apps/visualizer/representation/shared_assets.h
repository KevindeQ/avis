#ifndef AVIS_VISUALIZER_REPRESENTATION_SHARED_ASSETS_H
#define AVIS_VISUALIZER_REPRESENTATION_SHARED_ASSETS_H

// The representation's shared library (docs/design/12-visualizer.md §9): the meshes and base materials every entity
// of a kind uses, created once through the asset cache. They're excluded from the geometry budget (12 §9.1).
//
// Mesh frames, in the Avis world convention (Z up, X forward, 00 §4.1):
// - unit_cube and cube_edges: [-0.5, 0.5]³, centred on the entity's origin, as the Driver's boxes are
// - vehicle_proxy: [-0.5, 0.5] × [-0.5, 0.5] × [0, 1], standing on z = 0; the representation scales it to the extent
//   and moves it forward from the rear axle (vehicle_body_offset). Primitive 0 is the body, primitive 1 the wheels.
// - axes: unit lines along +X (red), +Y (green) and +Z (blue)
// - grid: lines on z = 0 around the origin, fading by distance through vertex alpha
//
// Owned by the thread that created it, the simulation thread; the asset cache itself is thread-safe.

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_data.h"

#include <Eigen/Dense>

#include <memory>

namespace avis::visualizer::representation
{
    // The proxy's body sits this far forward of the vehicle frame's origin, the rear axle, as a fraction of the
    // extent's length (00 §4.1: the vehicle frame's origin is the rear-axle centre on the ground)
    inline constexpr float vehicle_body_offset = 0.3f;

    // The grid's reach: 1 m lines out to minor_grid_radius, 10 m lines out to major_grid_radius
    inline constexpr float minor_grid_radius = 50.0f;
    inline constexpr float major_grid_radius = 200.0f;

    // Procedural meshes (no asset cache): for the shared library and for tests
    namespace meshes
    {
        result<scene::mesh_data> unit_cube();
        result<scene::mesh_data> cube_edges();
        result<scene::mesh_data> axes();
        result<scene::mesh_data> grid();
        result<scene::mesh_data> vehicle_proxy();

        // Lines from the apex at the origin to the four corners of the far rectangle, and the rectangle itself, looking
        // along +X. Angles are the full fields of view in radians, clamped below 179°; a range of 0 or less draws 1 m.
        result<scene::mesh_data> frustum(float horizontal_fov, float vertical_fov, float range);
    } // namespace meshes

    class shared_assets
    {
    public:
        // Creates every mesh and material; the first failure is returned and nothing is kept
        static result<std::unique_ptr<shared_assets>> create(scene::asset_cache& assets);

        ~shared_assets(); // releases this object's references; components hold their own

        shared_assets(const shared_assets&) = delete;
        shared_assets& operator=(const shared_assets&) = delete;

        scene::mesh_handle unit_cube() const noexcept
        {
            return unit_cube_;
        }

        scene::mesh_handle cube_edges() const noexcept
        {
            return cube_edges_;
        }

        scene::mesh_handle axes() const noexcept
        {
            return axes_;
        }

        scene::mesh_handle grid() const noexcept
        {
            return grid_;
        }

        scene::mesh_handle vehicle_proxy() const noexcept
        {
            return vehicle_proxy_;
        }

        // Unlit, vertex colours: the axes shape and the gizmo
        scene::material_handle axes_material() const noexcept
        {
            return axes_material_;
        }

        // Unlit, vertex colours with alpha, blended
        scene::material_handle grid_material() const noexcept
        {
            return grid_material_;
        }

        // Lit dark rubber for the proxy's wheels (primitive 1)
        scene::material_handle wheel_material() const noexcept
        {
            return wheel_material_;
        }

    private:
        explicit shared_assets(scene::asset_cache& assets) : assets_{ assets } {}

        scene::asset_cache& assets_;
        scene::mesh_handle unit_cube_;
        scene::mesh_handle cube_edges_;
        scene::mesh_handle axes_;
        scene::mesh_handle grid_;
        scene::mesh_handle vehicle_proxy_;
        scene::material_handle axes_material_;
        scene::material_handle grid_material_;
        scene::material_handle wheel_material_;
    };
} // namespace avis::visualizer::representation

#endif
