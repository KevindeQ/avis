#ifndef AVIS_SCENE_REFLECTION_H
#define AVIS_SCENE_REFLECTION_H

// Component reflection for the inspector (docs/design/08-scene-management.md §3, §7, §9.7). Each shown component has
// a reflection<T> specialisation with a constexpr table of fields: a name, a unit and a plain-function reader. The
// inspector and the 20 Hz selected-entity update use only these tables, so a new component needs no UI code.
//
// The specialisations are below, grouped by component header in the order of 08 §9.2. Components without one aren't
// shown: node_reference, previous_world and represents are internal, properties travels as rows in reflected_entity,
// and belief_geometry and lidar_points are summarised by the `geometry` pseudo-component (detail::geometry_summary).

#include "avis/scene/bounds.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/sensor.h"
#include "avis/scene/components/vehicle.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/flags.h"
#include "avis/scene/types.h"
#include "avis/simulation/time.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <entt/entity/fwd.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace avis::scene
{
    using field_value = std::variant<
        bool,
        std::int64_t,
        std::uint64_t,
        double,
        std::string,
        Eigen::Vector3d,
        Eigen::Quaterniond,
        color,
        simulation::simulation_duration>;

    struct field_description
    {
        std::string_view name;
        std::string_view unit; // "", "m", "m/s", "m/s²", "rad", "rad/s", "s", "lx", "cd"
        field_value (*read)(const void* component); // component points to the reflected type
    };

    // Specialised for each component the inspector shows; the primary template stays undefined. A specialisation has
    // `static constexpr std::string_view name` and `static constexpr std::array<field_description, N> fields`.
    template<typename Component>
    struct reflection;

    template<typename Component>
    concept reflected_type = requires {
        { reflection<Component>::name } -> std::convertible_to<std::string_view>;
        reflection<Component>::fields.size();
    };

    struct reflected_field
    {
        std::string_view name; // points into the static reflection tables
        std::string_view unit;
        field_value value;
    };

    struct reflected_component
    {
        std::string_view name;
        std::vector<reflected_field> fields; // in declaration order
    };

    // A value type, posted to the GUI thread
    struct reflected_entity
    {
        object_id id = 0;
        std::string name;
        std::vector<reflected_component> components; // in the order of the headers in 08 §9.2
        std::vector<components::property_entry> properties; // grouped by dotted key in the inspector
        components::update_stamp stamp; // §8.4
        std::optional<aabb> world_bounds; // §8.3
    };

    // Reads every field of `component` through its table
    template<reflected_type Component>
    reflected_component reflect_component(const Component& component)
    {
        reflected_component reflected{ .name = reflection<Component>::name, .fields = {} };
        reflected.fields.reserve(reflection<Component>::fields.size());
        for (const field_description& field : reflection<Component>::fields)
        {
            reflected.fields.push_back({ .name = field.name, .unit = field.unit, .value = field.read(&component) });
        }
        return reflected;
    }

    // The names the inspector shows for enum fields
    std::string_view to_string(entity_category category) noexcept;
    std::string_view to_string(draw_layer layer) noexcept;
    std::string_view to_string(light_kind kind) noexcept;
} // namespace avis::scene

namespace avis::scene::components
{
    std::string_view to_string(sensor_kind kind) noexcept;
    std::string_view to_string(shape_kind kind) noexcept;
} // namespace avis::scene::components

namespace avis::scene::detail
{
    // ---- reader helpers ----

    template<typename Component>
    const Component& component_cast(const void* component) noexcept
    {
        return *static_cast<const Component*>(component);
    }

    template<typename Member>
    struct member_class;

    template<typename Type, typename Class>
    struct member_class<Type Class::*>
    {
        using type = Class;
    };

    // Converts a stored value to the field_value alternative that shows it: floats to double, unsigned integers to
    // std::uint64_t, signed ones to std::int64_t, float vectors to double, enums to their names
    template<typename Value>
    field_value to_field_value(const Value& value)
    {
        if constexpr (std::is_same_v<Value, bool>)
        {
            return value;
        }
        else if constexpr (std::is_enum_v<Value>)
        {
            return std::string{ to_string(value) };
        }
        else if constexpr (std::is_floating_point_v<Value>)
        {
            return static_cast<double>(value);
        }
        else if constexpr (std::is_integral_v<Value> && std::is_unsigned_v<Value>)
        {
            return static_cast<std::uint64_t>(value);
        }
        else if constexpr (std::is_integral_v<Value>)
        {
            return static_cast<std::int64_t>(value);
        }
        else if constexpr (std::is_same_v<Value, Eigen::Vector3f>)
        {
            return Eigen::Vector3d{ value.template cast<double>() };
        }
        else if constexpr (std::is_same_v<Value, Eigen::Quaternionf>)
        {
            return Eigen::Quaterniond{ value.template cast<double>() };
        }
        else
        {
            return value; // std::string, Eigen::Vector3d, color, simulation_duration
        }
    }

    template<auto First, auto... Rest>
    const auto& follow_members(const auto& owner) noexcept
    {
        if constexpr (sizeof...(Rest) == 0)
        {
            return owner.*First;
        }
        else
        {
            return follow_members<Rest...>(owner.*First);
        }
    }

    // A reader for a field reached through a chain of data members: read_member<&vehicle::state, &vehicle_state::speed>
    template<auto First, auto... Rest>
    field_value read_member(const void* component)
    {
        using owner = typename member_class<decltype(First)>::type;
        return to_field_value(follow_members<First, Rest...>(component_cast<owner>(component)));
    }

    // One flag of an avis::flags member, as a boolean field
    template<auto Member, auto Flag>
    field_value read_flag(const void* component)
    {
        using owner = typename member_class<decltype(Member)>::type;
        return static_cast<bool>(component_cast<owner>(component).*Member & Flag);
    }

    // ---- the geometry pseudo-component ----

    // belief_geometry and lidar_points are too large to show field by field. reflect_components summarises them as
    // one `geometry` component when the entity has either.
    struct geometry_summary
    {
        const components::belief_geometry* geometry = nullptr;
        const components::lidar_points* points = nullptr;
    };

    field_value read_geometry_chunk_count(const void* summary);
    field_value read_geometry_vertex_count(const void* summary); // over every chunk
    field_value read_geometry_sweep_count(const void* summary);
    field_value read_geometry_point_count(const void* summary); // over every retained sweep
    field_value read_geometry_byte_count(const void* summary); // the chunks' and sweeps' CPU arrays

    // ---- light ----

    // The light's intensity is in lux for a directional light and candela otherwise; the field in the other unit reads
    // 0
    field_value read_light_illuminance(const void* light);
    field_value read_light_luminous_intensity(const void* light);
} // namespace avis::scene::detail

namespace avis::scene
{
    // ---- components/identity.h ----

    template<>
    struct reflection<components::identity>
    {
        using type = components::identity;
        static constexpr std::string_view name = "identity";
        static constexpr std::array fields = {
            field_description{ "id", "", &detail::read_member<&type::id> },
            field_description{ "name", "", &detail::read_member<&type::name> },
            field_description{ "category", "", &detail::read_member<&type::category> },
        };
    };

    // ---- components/rendering.h ----

    // Handles and counts only; the asset contents are the asset cache's
    template<>
    struct reflection<components::mesh>
    {
        using type = components::mesh;
        static constexpr std::string_view name = "mesh";
        static constexpr std::array fields = {
            field_description{ "asset_index", "", &detail::read_member<&type::asset, &mesh_handle::index> },
            field_description{ "asset_generation", "", &detail::read_member<&type::asset, &mesh_handle::generation> },
            field_description{ "primitive_first", "", &detail::read_member<&type::primitive_first> },
            field_description{ "primitive_count", "", &detail::read_member<&type::primitive_count> },
        };
    };

    template<>
    struct reflection<components::material>
    {
        using type = components::material;
        static constexpr std::string_view name = "material";
        static constexpr std::array fields = {
            field_description{
                "material_count",
                "",
                [](const void* component) -> field_value
                { return static_cast<std::uint64_t>(detail::component_cast<type>(component).materials.size()); } },
        };
    };

    template<>
    struct reflection<components::render_options>
    {
        using type = components::render_options;
        static constexpr std::string_view name = "render_options";
        static constexpr std::array fields = {
            field_description{ "layer", "", &detail::read_member<&type::layer> },
            field_description{ "layer_priority", "", &detail::read_member<&type::layer_priority> },
            field_description{ "visible", "", &detail::read_flag<&type::flags, instance_flags::visible> },
            field_description{ "casts_shadows", "", &detail::read_flag<&type::flags, instance_flags::casts_shadows> },
            field_description{ "selected", "", &detail::read_flag<&type::flags, instance_flags::selected> },
            field_description{ "pickable", "", &detail::read_flag<&type::flags, instance_flags::pickable> },
            field_description{ "screen_aligned", "", &detail::read_flag<&type::flags, instance_flags::screen_aligned> },
            field_description{ "maximum_view_distance", "m", &detail::read_member<&type::maximum_view_distance> },
        };
    };

    template<>
    struct reflection<components::visibility>
    {
        using type = components::visibility;
        static constexpr std::string_view name = "visibility";
        static constexpr std::array fields = {
            field_description{ "visible", "", &detail::read_member<&type::visible> },
            field_description{ "layers", "", &detail::read_member<&type::layers> },
        };
    };

    template<>
    struct reflection<components::light>
    {
        using type = components::light;
        static constexpr std::string_view name = "light";
        static constexpr std::array fields = {
            field_description{ "kind", "", &detail::read_member<&type::kind> },
            field_description{ "color", "", &detail::read_member<&type::light_color> },
            field_description{ "illuminance", "lx", &detail::read_light_illuminance },
            field_description{ "luminous_intensity", "cd", &detail::read_light_luminous_intensity },
            field_description{ "range", "m", &detail::read_member<&type::range> },
            field_description{ "inner_cone_angle", "rad", &detail::read_member<&type::inner_cone_angle> },
            field_description{ "outer_cone_angle", "rad", &detail::read_member<&type::outer_cone_angle> },
            field_description{ "casts_shadows", "", &detail::read_member<&type::casts_shadows> },
        };
    };

    // ---- components/vehicle.h ----

    // vehicle_state's fields are flattened into the vehicle's
    template<>
    struct reflection<components::vehicle>
    {
        using type = components::vehicle;
        using state = components::vehicle_state;
        static constexpr std::string_view name = "vehicle";
        static constexpr std::array fields = {
            field_description{ "primary", "", &detail::read_member<&type::primary> },
            field_description{ "time", "s", &detail::read_member<&type::state, &state::time> },
            field_description{ "speed", "m/s", &detail::read_member<&type::state, &state::speed> },
            field_description{ "steering_angle", "rad", &detail::read_member<&type::state, &state::steering_angle> },
            field_description{ "linear_velocity", "m/s", &detail::read_member<&type::state, &state::linear_velocity> },
            field_description{
                "angular_velocity", "rad/s", &detail::read_member<&type::state, &state::angular_velocity> },
            field_description{ "acceleration", "m/s²", &detail::read_member<&type::state, &state::acceleration> },
        };
    };

    // ---- components/sensor.h ----

    template<>
    struct reflection<components::sensor>
    {
        using type = components::sensor;
        static constexpr std::string_view name = "sensor";
        static constexpr std::array fields = {
            field_description{ "kind", "", &detail::read_member<&type::kind> },
        };
    };

    // ---- components/driver.h ----

    // Read-only in the UI (08 §3)
    template<>
    struct reflection<components::driver_entity>
    {
        using type = components::driver_entity;
        static constexpr std::string_view name = "driver_entity";
        static constexpr std::array fields = {
            field_description{ "category", "", &detail::read_member<&type::category> },
            field_description{ "primary", "", &detail::read_flag<&type::flags, driver_entity_flags::primary> },
            field_description{ "pickable", "", &detail::read_flag<&type::flags, driver_entity_flags::pickable> },
            field_description{
                "hidden_by_default", "", &detail::read_flag<&type::flags, driver_entity_flags::hidden_by_default> },
        };
    };

    template<>
    struct reflection<components::display_shape>
    {
        using type = components::display_shape;
        static constexpr std::string_view name = "display_shape";
        static constexpr std::array fields = {
            field_description{ "shape", "", &detail::read_member<&type::shape> },
            field_description{ "extent", "m", &detail::read_member<&type::extent> },
            field_description{ "color", "", &detail::read_member<&type::shape_color> },
            field_description{ "opacity", "", &detail::read_member<&type::opacity> },
            field_description{ "horizontal_fov", "rad", &detail::read_member<&type::horizontal_fov> },
            field_description{ "vertical_fov", "rad", &detail::read_member<&type::vertical_fov> },
            field_description{ "range", "m", &detail::read_member<&type::range> },
        };
    };

    template<>
    struct reflection<detail::geometry_summary>
    {
        static constexpr std::string_view name = "geometry";
        static constexpr std::array fields = {
            field_description{ "chunk_count", "", &detail::read_geometry_chunk_count },
            field_description{ "vertex_count", "", &detail::read_geometry_vertex_count },
            field_description{ "sweep_count", "", &detail::read_geometry_sweep_count },
            field_description{ "point_count", "", &detail::read_geometry_point_count },
            field_description{ "byte_count", "", &detail::read_geometry_byte_count },
        };
    };

    // ---- components/viewing.h ----

    template<>
    struct reflection<components::label>
    {
        using type = components::label;
        using style = components::label_style;
        static constexpr std::string_view name = "label";
        static constexpr std::array fields = {
            field_description{ "text_template", "", &detail::read_member<&type::text_template> },
            field_description{ "anchor_offset", "m", &detail::read_member<&type::anchor_offset> },
            field_description{ "pixel_offset_x", // pixels
                               "",
                               [](const void* component) -> field_value
                               { return double{ detail::component_cast<type>(component).pixel_offset.x() }; } },
            field_description{ "pixel_offset_y", // pixels
                               "",
                               [](const void* component) -> field_value
                               { return double{ detail::component_cast<type>(component).pixel_offset.y() }; } },
            field_description{ "text_color", "", &detail::read_member<&type::style, &style::text_color> },
            field_description{ "background", "", &detail::read_member<&type::style, &style::background> },
            field_description{ "pixel_height", "", &detail::read_member<&type::style, &style::pixel_height> },
            field_description{
                "maximum_view_distance", "m", &detail::read_member<&type::style, &style::maximum_view_distance> },
        };
    };

    // The settings and the sample count; the samples themselves are drawn, not listed
    template<>
    struct reflection<components::trail>
    {
        using type = components::trail;
        using settings = components::trail_settings;
        static constexpr std::string_view name = "trail";
        static constexpr std::array fields = {
            field_description{ "duration", "s", &detail::read_member<&type::settings, &settings::duration_seconds> },
            field_description{
                "minimum_spacing", "m", &detail::read_member<&type::settings, &settings::minimum_spacing> },
            field_description{ "color", "", &detail::read_member<&type::settings, &settings::line_color> },
            field_description{ "line_width", "", &detail::read_member<&type::settings, &settings::line_width> },
            field_description{
                "sample_count",
                "",
                [](const void* component) -> field_value
                { return static_cast<std::uint64_t>(detail::component_cast<type>(component).samples.size()); } },
        };
    };

    // created and latest(), which are always there; reflected_entity::stamp carries the optional kinds, which the
    // inspector shows relative to the current time (08 §8.4)
    template<>
    struct reflection<components::update_stamp>
    {
        using type = components::update_stamp;
        using time = components::update_time;
        static constexpr std::string_view name = "update_stamp";
        static constexpr std::array fields = {
            field_description{ "created_step", "", &detail::read_member<&type::created, &time::step> },
            field_description{ "created_time", "s", &detail::read_member<&type::created, &time::time> },
            field_description{
                "latest_step",
                "",
                [](const void* component) -> field_value
                { return detail::to_field_value(detail::component_cast<type>(component).latest().step); } },
            field_description{ "latest_time",
                               "s",
                               [](const void* component) -> field_value
                               { return detail::component_cast<type>(component).latest().time; } },
        };
    };
} // namespace avis::scene

namespace avis::scene::detail
{
    template<typename... Types>
    struct type_list
    {};

    // Every reflected component, in the order of the headers in 08 §9.2. scene::reflect visits them in this order, so
    // the inspector's layout doesn't depend on EnTT's pool order.
    using reflected_components = type_list<
        components::identity,
        components::mesh,
        components::material,
        components::render_options,
        components::visibility,
        components::light,
        components::vehicle,
        components::sensor,
        components::driver_entity,
        components::display_shape,
        geometry_summary,
        components::label,
        components::trail,
        components::update_stamp>;

    // Appends a reflected_component for each of reflected_components that `entity` has, in that order. The geometry
    // summary is added when the entity has belief_geometry or lidar_points. `entity` must be alive.
    void reflect_components(const entt::registry& registry, entity entity, std::vector<reflected_component>& output);
} // namespace avis::scene::detail

#endif
