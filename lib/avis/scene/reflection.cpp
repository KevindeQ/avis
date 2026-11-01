#include "avis/scene/reflection.h"

#include <entt/entity/registry.hpp>

namespace avis::scene
{
    std::string_view to_string(const entity_category category) noexcept
    {
        switch (category)
        {
        case entity_category::group:
            return "group";
        case entity_category::vehicle:
            return "vehicle";
        case entity_category::sensor:
            return "sensor";
        case entity_category::object:
            return "object";
        case entity_category::geometry:
            return "geometry";
        case entity_category::helper:
            return "helper";
        }
        return "unknown";
    }

    std::string_view to_string(const draw_layer layer) noexcept
    {
        switch (layer)
        {
        case draw_layer::world:
            return "world";
        case draw_layer::overlay:
            return "overlay";
        case draw_layer::overlay_on_top:
            return "overlay_on_top";
        }
        return "unknown";
    }

    std::string_view to_string(const light_kind kind) noexcept
    {
        switch (kind)
        {
        case light_kind::directional:
            return "directional";
        case light_kind::point:
            return "point";
        case light_kind::spot:
            return "spot";
        }
        return "unknown";
    }
} // namespace avis::scene

namespace avis::scene::components
{
    std::string_view to_string(const sensor_kind kind) noexcept
    {
        switch (kind)
        {
        case sensor_kind::camera:
            return "camera";
        case sensor_kind::lidar:
            return "lidar";
        case sensor_kind::gnss_imu:
            return "gnss_imu";
        }
        return "unknown";
    }

    std::string_view to_string(const shape_kind kind) noexcept
    {
        switch (kind)
        {
        case shape_kind::none:
            return "none";
        case shape_kind::box:
            return "box";
        case shape_kind::vehicle:
            return "vehicle";
        case shape_kind::axes:
            return "axes";
        case shape_kind::frustum:
            return "frustum";
        }
        return "unknown";
    }
} // namespace avis::scene::components

namespace avis::scene::detail
{
    namespace
    {
        template<typename Element>
        std::uint64_t bytes_of(const std::vector<Element>& elements) noexcept
        {
            return elements.size() * sizeof(Element);
        }

        const std::vector<components::geometry_chunk>& chunks_of(const geometry_summary& summary) noexcept
        {
            static const std::vector<components::geometry_chunk> none;
            return summary.geometry != nullptr ? summary.geometry->chunks() : none;
        }

        const std::vector<components::lidar_sweep>& sweeps_of(const geometry_summary& summary) noexcept
        {
            static const std::vector<components::lidar_sweep> none;
            return summary.points != nullptr ? summary.points->sweeps : none;
        }
    } // namespace

    field_value read_geometry_chunk_count(const void* summary)
    {
        return static_cast<std::uint64_t>(chunks_of(component_cast<geometry_summary>(summary)).size());
    }

    field_value read_geometry_vertex_count(const void* summary)
    {
        std::uint64_t count = 0;
        for (const components::geometry_chunk& chunk : chunks_of(component_cast<geometry_summary>(summary)))
        {
            count += chunk.positions.size();
        }
        return count;
    }

    field_value read_geometry_sweep_count(const void* summary)
    {
        return static_cast<std::uint64_t>(sweeps_of(component_cast<geometry_summary>(summary)).size());
    }

    field_value read_geometry_point_count(const void* summary)
    {
        std::uint64_t count = 0;
        for (const components::lidar_sweep& sweep : sweeps_of(component_cast<geometry_summary>(summary)))
        {
            count += sweep.positions.size();
        }
        return count;
    }

    field_value read_geometry_byte_count(const void* summary)
    {
        const auto& reflected = component_cast<geometry_summary>(summary);
        std::uint64_t bytes = 0;
        for (const components::geometry_chunk& chunk : chunks_of(reflected))
        {
            bytes +=
                bytes_of(chunk.positions) + bytes_of(chunk.indices) + bytes_of(chunk.normals) + bytes_of(chunk.colors);
        }
        for (const components::lidar_sweep& sweep : sweeps_of(reflected))
        {
            bytes +=
                bytes_of(sweep.positions) + bytes_of(sweep.intensity) + bytes_of(sweep.ring) + bytes_of(sweep.colors);
        }
        return bytes;
    }

    field_value read_light_illuminance(const void* light)
    {
        const auto& reflected = component_cast<components::light>(light);
        return reflected.kind == light_kind::directional ? static_cast<double>(reflected.intensity) : 0.0;
    }

    field_value read_light_luminous_intensity(const void* light)
    {
        const auto& reflected = component_cast<components::light>(light);
        return reflected.kind != light_kind::directional ? static_cast<double>(reflected.intensity) : 0.0;
    }

    namespace
    {
        template<typename Component>
        void reflect_one(const entt::registry& registry, const entity entity, std::vector<reflected_component>& output)
        {
            if constexpr (std::is_same_v<Component, geometry_summary>)
            {
                const geometry_summary summary{
                    .geometry = registry.try_get<components::belief_geometry>(entity),
                    .points = registry.try_get<components::lidar_points>(entity),
                };
                if (summary.geometry != nullptr || summary.points != nullptr)
                {
                    output.push_back(reflect_component(summary));
                }
            }
            else if (const Component* component = registry.try_get<Component>(entity))
            {
                output.push_back(reflect_component(*component));
            }
        }

        template<typename... Components>
        void reflect_all(
            type_list<Components...>,
            const entt::registry& registry,
            const entity entity,
            std::vector<reflected_component>& output)
        {
            (reflect_one<Components>(registry, entity, output), ...);
        }
    } // namespace

    void reflect_components(
        const entt::registry& registry, const entity entity, std::vector<reflected_component>& output)
    {
        reflect_all(reflected_components{}, registry, entity, output);
    }
} // namespace avis::scene::detail
