#include "representation/representation_settings.h"

#include "rendering/yaml_reading.h"

#include "avis/scene/reflection.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace avis::visualizer::representation
{
    namespace
    {
        namespace yaml_reading = rendering::yaml_reading;
        using yaml_reading::is_given;
        using yaml_reading::scalar;
        using yaml_reading::section;

        constexpr std::string_view section_path = "representation";

        constexpr std::size_t index_of(const scene::entity_category category)
        {
            return static_cast<std::size_t>(category);
        }

        // The categories settings may name: every one but helper, which the representation creates itself
        constexpr std::array configurable_categories{
            scene::entity_category::group,  scene::entity_category::vehicle,  scene::entity_category::sensor,
            scene::entity_category::object, scene::entity_category::geometry,
        };

        std::string category_list()
        {
            std::string names;
            for (const scene::entity_category category : configurable_categories)
            {
                names += names.empty() ? std::string{ scene::to_string(category) } :
                                         std::format(", {}", scene::to_string(category));
            }
            return names;
        }

        result<scene::entity_category>
            parse_category(const yaml_reading::reader& reader, const YAML::Node& node, const std::string& path)
        {
            auto name = reader.text(node, path);
            if (!name)
            {
                return std::unexpected{ std::move(name).error() };
            }
            for (const scene::entity_category category : configurable_categories)
            {
                if (*name == scene::to_string(category))
                {
                    return category;
                }
            }
            return std::unexpected{ reader.error_at(
                node, path, std::format("expected an entity category ({}), got '{}'", category_list(), *name)) };
        }

        // A mapping of category names to values, merged into `target`: { vehicle: ..., object: ... }
        template<class Value, class ReadValue>
        void read_per_category(
            section& parent, const std::string_view key, per_category<Value>& target, ReadValue&& read)
        {
            if (parent.failed())
            {
                return;
            }
            const YAML::Node node = parent.given(key);
            if (!is_given(node))
            {
                return;
            }
            const std::string path = parent.path_of(key);
            const yaml_reading::reader& reader = parent.reader_of();
            if (auto checked = reader.expect_map(node, path); !checked)
            {
                parent.fail(std::move(checked).error());
                return;
            }
            for (const auto& entry : node)
            {
                auto category = parse_category(reader, entry.first, path);
                if (!category)
                {
                    parent.fail(std::move(category).error());
                    return;
                }
                const std::string entry_path = yaml_reading::child_path(path, entry.first.Scalar());
                if (auto failure = read(reader, entry.second, entry_path, target[index_of(*category)]))
                {
                    parent.fail(std::move(*failure));
                    return;
                }
            }
        }

        // A sequence of distinct category names, replacing `target`
        void
            read_category_list(section& parent, const std::string_view key, std::vector<scene::entity_category>& target)
        {
            if (parent.failed())
            {
                return;
            }
            const YAML::Node node = parent.given(key);
            if (!is_given(node))
            {
                return;
            }
            const std::string path = parent.path_of(key);
            const yaml_reading::reader& reader = parent.reader_of();
            if (auto checked = reader.expect_sequence(node, path); !checked)
            {
                parent.fail(std::move(checked).error());
                return;
            }
            std::vector<scene::entity_category> categories;
            for (std::size_t index = 0; index < node.size(); ++index)
            {
                const std::string element = yaml_reading::element_path(path, index);
                auto category = parse_category(reader, node[index], element);
                if (!category)
                {
                    parent.fail(std::move(category).error());
                    return;
                }
                if (std::ranges::contains(categories, *category))
                {
                    parent.fail(reader.error_at(
                        node[index], element, std::format("'{}' is listed twice", scene::to_string(*category))));
                    return;
                }
                categories.push_back(*category);
            }
            target = std::move(categories);
        }
    } // namespace

    per_category<std::string> label_settings::make_default_templates()
    {
        per_category<std::string> templates;
        templates[index_of(scene::entity_category::vehicle)] = "{name}";
        templates[index_of(scene::entity_category::object)] = "{name} · {property:perception.speed:.1f}";
        return templates;
    }

    per_category<trail_category_settings> representation_settings::make_default_trails()
    {
        per_category<trail_category_settings> trails;
        trails[index_of(scene::entity_category::vehicle)] = { .duration_seconds = 30.0f, .minimum_spacing = 0.25f };
        return trails;
    }

    result<representation_settings> parse_representation_settings(const YAML::Node& node)
    {
        representation_settings settings;
        if (!is_given(node))
        {
            return settings;
        }

        const yaml_reading::reader reader;
        section representation{ reader, node, std::string{ section_path } };

        section boxes = representation.child("boxes");
        boxes.read("edge_width", settings.boxes.edge_width);
        boxes.check("edge_width", settings.boxes.edge_width > 0.0f, "expected a positive width");
        if (auto failure = boxes.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section lidar = representation.child("lidar");
        lidar.read("maximum_points_per_sweep", settings.lidar.maximum_points_per_sweep);
        lidar.check("maximum_points_per_sweep", settings.lidar.maximum_points_per_sweep >= 1, "expected at least 1");
        if (auto failure = lidar.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section geometry = representation.child("geometry");
        geometry.read("budget_mib", settings.geometry.budget_mib);
        geometry.check("budget_mib", settings.geometry.budget_mib >= 1, "expected at least 1");
        if (auto failure = geometry.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section labels = representation.child("labels");
        labels.read("enabled", settings.labels.enabled);
        labels.read("maximum_count", settings.labels.maximum_count);
        labels.read("font", settings.labels.font);
        labels.check("font", !settings.labels.font.empty(), "expected a path");
        section style = labels.child("style");
        style.read("pixel_height", settings.labels.style.pixel_height);
        style.check("pixel_height", settings.labels.style.pixel_height > 0.0f, "expected a positive height");
        style.read("maximum_view_distance", settings.labels.style.maximum_view_distance);
        style.check(
            "maximum_view_distance",
            settings.labels.style.maximum_view_distance > 0.0f,
            "expected a positive distance");
        if (auto failure = style.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }
        read_per_category(
            labels,
            "templates",
            settings.labels.templates,
            [](const yaml_reading::reader& reader,
               const YAML::Node& value,
               const std::string& path,
               std::string& target) -> std::optional<error>
            {
                // "vehicle:" with no value means no label, like ""
                if (!is_given(value))
                {
                    target.clear();
                    return std::nullopt;
                }
                auto text = reader.text(value, path);
                if (!text)
                {
                    return std::move(text).error();
                }
                target = std::move(*text);
                return std::nullopt;
            });
        if (auto failure = labels.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        read_per_category(
            representation,
            "trails",
            settings.trails,
            [](const yaml_reading::reader& reader,
               const YAML::Node& value,
               const std::string& path,
               trail_category_settings& target) -> std::optional<error>
            {
                section trail{ reader, value, path };
                trail.read("duration_seconds", target.duration_seconds);
                trail.check("duration_seconds", target.duration_seconds >= 0.0f, "expected 0 (off) or more");
                trail.read("minimum_spacing", target.minimum_spacing);
                trail.check("minimum_spacing", target.minimum_spacing > 0.0f, "expected a positive spacing");
                return trail.take_failure();
            });
        if (auto failure = representation.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section stale = representation.child("stale");
        stale.read("enabled", settings.stale.enabled);
        stale.read("after_seconds", settings.stale.after_seconds);
        stale.check("after_seconds", settings.stale.after_seconds > 0.0f, "expected a positive duration");
        read_category_list(stale, "categories", settings.stale.categories);
        if (auto failure = stale.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        return settings;
    }

    YAML::Node to_yaml(const representation_settings& settings)
    {
        YAML::Node representation{ YAML::NodeType::Map };

        YAML::Node boxes = yaml_reading::flow_map();
        boxes["edge_width"] = scalar(settings.boxes.edge_width);
        representation["boxes"] = boxes;

        YAML::Node lidar = yaml_reading::flow_map();
        lidar["maximum_points_per_sweep"] = scalar(settings.lidar.maximum_points_per_sweep);
        representation["lidar"] = lidar;

        YAML::Node geometry = yaml_reading::flow_map();
        geometry["budget_mib"] = scalar(settings.geometry.budget_mib);
        representation["geometry"] = geometry;

        YAML::Node labels{ YAML::NodeType::Map };
        labels["enabled"] = scalar(settings.labels.enabled);
        labels["maximum_count"] = scalar(settings.labels.maximum_count);
        labels["font"] = yaml_reading::path_scalar(settings.labels.font);
        YAML::Node style = yaml_reading::flow_map();
        style["pixel_height"] = scalar(settings.labels.style.pixel_height);
        style["maximum_view_distance"] = scalar(settings.labels.style.maximum_view_distance);
        labels["style"] = style;
        YAML::Node templates{ YAML::NodeType::Map };
        for (const scene::entity_category category : configurable_categories)
        {
            templates[std::string{ scene::to_string(category) }] =
                scalar(settings.labels.templates[index_of(category)]);
        }
        labels["templates"] = templates;
        representation["labels"] = labels;

        YAML::Node trails{ YAML::NodeType::Map };
        for (const scene::entity_category category : configurable_categories)
        {
            const trail_category_settings& trail_settings = settings.trails[index_of(category)];
            YAML::Node trail = yaml_reading::flow_map();
            trail["duration_seconds"] = scalar(trail_settings.duration_seconds);
            trail["minimum_spacing"] = scalar(trail_settings.minimum_spacing);
            trails[std::string{ scene::to_string(category) }] = trail;
        }
        representation["trails"] = trails;

        YAML::Node stale = yaml_reading::flow_map();
        stale["enabled"] = scalar(settings.stale.enabled);
        stale["after_seconds"] = scalar(settings.stale.after_seconds);
        YAML::Node categories{ YAML::NodeType::Sequence };
        categories.SetStyle(YAML::EmitterStyle::Flow);
        for (const scene::entity_category category : settings.stale.categories)
        {
            categories.push_back(scalar(scene::to_string(category)));
        }
        stale["categories"] = categories;
        representation["stale"] = stale;
        return representation;
    }
} // namespace avis::visualizer::representation
