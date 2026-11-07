#ifndef AVIS_VISUALIZER_REPRESENTATION_REPRESENTATION_SETTINGS_H
#define AVIS_VISUALIZER_REPRESENTATION_REPRESENTATION_SETTINGS_H

// The scene representation's settings: the `representation` section of visualizer.yaml (docs/design/12-visualizer.md
// §9). Every visual choice the Driver doesn't make lives here, so changing how something looks never touches the
// renderer. Reading the file is visualizer_configuration's job; this parses and writes the section's node.

#include "avis/error.h"
#include "avis/scene/types.h"

#include <yaml-cpp/yaml.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace avis::visualizer::representation
{
    // Indexed by scene::entity_category
    template<class T>
    using per_category = std::array<T, scene::entity_category_count>;

    struct box_settings
    {
        float edge_width = 2.0f; // pixels

        friend bool operator==(const box_settings&, const box_settings&) = default;
    };

    struct lidar_settings
    {
        std::uint32_t maximum_points_per_sweep = 200'000;

        friend bool operator==(const lidar_settings&, const lidar_settings&) = default;
    };

    struct geometry_settings
    {
        // CPU-side budget for dynamic meshes: belief geometry, lidar sweeps and trails (12 §9.1)
        std::uint32_t budget_mib = 512;

        friend bool operator==(const geometry_settings&, const geometry_settings&) = default;
    };

    struct label_style_settings
    {
        float pixel_height = 14.0f;
        float maximum_view_distance = 80.0f; // metres

        friend bool operator==(const label_style_settings&, const label_style_settings&) = default;
    };

    struct label_settings
    {
        bool enabled = true;
        std::uint32_t maximum_count = 200; // nearest to the primary vehicle first
        std::filesystem::path font = "data/assets/fonts/labels"; // atlas and metrics, 1x and 2x (08 §8.6)
        label_style_settings style;
        // Per entity category; "" = no label. Helpers are never labelled.
        per_category<std::string> templates = make_default_templates();

        friend bool operator==(const label_settings&, const label_settings&) = default;

    private:
        static per_category<std::string> make_default_templates();
    };

    struct trail_category_settings
    {
        float duration_seconds = 0.0f; // 0 = off
        float minimum_spacing = 0.25f; // metres between samples

        friend bool operator==(const trail_category_settings&, const trail_category_settings&) = default;
    };

    struct stale_settings
    {
        bool enabled = false;
        float after_seconds = 2.0f;
        std::vector<scene::entity_category> categories{ scene::entity_category::object };

        friend bool operator==(const stale_settings&, const stale_settings&) = default;
    };

    struct representation_settings
    {
        box_settings boxes;
        lidar_settings lidar;
        geometry_settings geometry;
        label_settings labels;
        per_category<trail_category_settings> trails = make_default_trails(); // per entity category
        stale_settings stale;

        friend bool operator==(const representation_settings&, const representation_settings&) = default;

    private:
        static per_category<trail_category_settings> make_default_trails();
    };

    // Parses the `representation` section. A missing or null section, and every missing key, keeps the default of
    // 12 §9; `labels.templates` and `trails` merge per category, and `stale.categories` replaces the list. Unknown keys
    // are ignored, but unknown category names are errors. Errors are error_domain::parse and name the position and the
    // key path ("4:22: representation.labels.maximum_count: expected an unsigned integer, got '-1'"). Pure: safe on
    // any thread.
    result<representation_settings> parse_representation_settings(const YAML::Node& section);

    // The `representation` section with every key, for writing visualizer.yaml; parse_representation_settings reads it
    // back unchanged
    YAML::Node to_yaml(const representation_settings& settings);
} // namespace avis::visualizer::representation

#endif
