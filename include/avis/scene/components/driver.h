#ifndef AVIS_SCENE_COMPONENTS_DRIVER_H
#define AVIS_SCENE_COMPONENTS_DRIVER_H

// What the Driver's scene stream says about an entity (docs/design/08-scene-management.md §3, §3.1; 02 §5.8): its
// marker and flags, how it's drawn, its properties, its belief geometry and its lidar sweeps. Semantic data: the
// Visualizer's scene representation turns it into mesh and material components, and none of it reaches the snapshot.

#include "avis/enum_flags.h"
#include "avis/scene/flags.h"
#include "avis/scene/types.h"
#include "avis/simulation/time.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace avis::scene::components
{
    // Marks an entity the Driver created. Read-only in the UI.
    struct driver_entity
    {
        entity_category category = entity_category::group;
        avis::flags<driver_entity_flags> flags{ avis::empty };
    };

    // ---- display_shape (entity_update.display, transform_update.extent) ----

    enum class shape_kind : std::uint8_t
    {
        none,
        box,
        vehicle,
        axes,
        frustum
    };

    // How the entity itself is drawn. The extent never becomes the node's scale (08 §4.3).
    struct display_shape
    {
        shape_kind shape = shape_kind::none;
        Eigen::Vector3f extent = Eigen::Vector3f::Ones(); // metres
        color shape_color;
        float opacity = 1.0f;
        float horizontal_fov = 0.0f; // radians, frustum
        float vertical_fov = 0.0f; // radians, frustum
        float range = 0.0f; // metres, frustum
    };

    // ---- properties (entity_properties) ----

    // The five property types of 02 §5.7: boolean, integer, number, text, vector3
    using property_value = std::variant<bool, std::int64_t, double, std::string, Eigen::Vector3d>;

    struct property_entry
    {
        std::string key; // dotted keys group the inspector rows: "perception.speed"
        property_value value;
        std::string unit; // "" when unitless

        friend bool operator==(const property_entry&, const property_entry&) = default;
    };

    // Ordered key → value map: entries stay in insertion order for the inspector, and a sorted index serves lookups
    // by key. Replacing a value keeps its position.
    class properties
    {
    public:
        // Replaces the value and unit of an existing key in place, or appends a new entry
        void set(std::string_view key, property_value value, std::string_view unit = {});

        // Erases the entry; the later ones keep their order. False when the key isn't there.
        bool remove(std::string_view key);

        // nullptr when the key isn't there. Valid until the next set or remove.
        const property_entry* find(std::string_view key) const noexcept;

        // Insertion order
        const std::vector<property_entry>& entries() const noexcept
        {
            return entries_;
        }

        std::size_t size() const noexcept
        {
            return entries_.size();
        }

        bool empty() const noexcept
        {
            return entries_.empty();
        }

        void clear() noexcept
        {
            entries_.clear();
            index_.clear();
        }

    private:
        // The index position of the first entry whose key isn't less than `key`
        std::size_t lower_bound(std::string_view key) const noexcept;

        std::vector<property_entry> entries_;
        std::vector<std::uint32_t> index_; // positions in entries_, sorted by key
    };

    // ---- belief_geometry (geometry_update) ----

    enum class geometry_topology : std::uint8_t
    {
        triangles,
        lines,
        line_strip,
        points
    };

    struct geometry_style
    {
        color style_color;
        float opacity = 1.0f;
        bool lit = true;
        float line_width = 1.0f; // pixels
        float point_size = 0.05f; // metres
        draw_layer layer = draw_layer::world; // world or overlay
    };

    // One chunk of an entity's geometry, in the entity's frame
    struct geometry_chunk
    {
        std::uint32_t id = 0; // within the entity
        geometry_topology topology = geometry_topology::triangles;
        Eigen::Vector3d origin = Eigen::Vector3d::Zero(); // relative to the entity, so positions stay small
        std::vector<Eigen::Vector3f> positions; // relative to origin
        std::vector<std::uint32_t> indices; // triangles and lines; empty when not indexed
        std::vector<Eigen::Vector3f> normals; // empty or one per position
        std::vector<std::array<std::uint8_t, 4>> colors; // sRGB; empty or one per position
        geometry_style style;
    };

    // The entity's chunks, sorted by id. Created on the first geometry_update (08 §3.1).
    class belief_geometry
    {
    public:
        // Creates or overwrites the chunk with the same id
        void replace(geometry_chunk chunk);

        // False when there's no such chunk
        bool remove(std::uint32_t id);

        // nullptr when there's no such chunk. Valid until the next replace or remove.
        const geometry_chunk* find(std::uint32_t id) const noexcept;

        const std::vector<geometry_chunk>& chunks() const noexcept
        {
            return chunks_;
        }

        void clear() noexcept
        {
            chunks_.clear();
        }

    private:
        std::vector<geometry_chunk> chunks_;
    };

    // ---- lidar_points (lidar_sweep) ----

    // One decoded sweep, in the sensor's frame at capture
    struct lidar_sweep
    {
        simulation::simulation_duration capture_time{ 0 };
        Eigen::Vector3d sensor_position = Eigen::Vector3d::Zero(); // world
        Eigen::Quaternionf sensor_orientation = Eigen::Quaternionf::Identity(); // world
        std::vector<Eigen::Vector3f> positions; // sensor frame, metres
        std::vector<std::uint8_t> intensity; // empty when the sweep has no intensity channel
        std::vector<std::uint8_t> ring; // empty when the sweep has no ring channel
        std::vector<std::array<std::uint8_t, 4>> colors; // sRGB; empty when the sweep has no colour channel
    };

    // The last `retention` sweeps of one lidar, oldest first
    struct lidar_points
    {
        std::uint8_t retention = 1; // from the newest sweep; 0 counts as 1
        std::vector<lidar_sweep> sweeps;

        // Appends the sweep, dropping the oldest beyond retention
        void push(lidar_sweep sweep);
    };
} // namespace avis::scene::components

#endif
