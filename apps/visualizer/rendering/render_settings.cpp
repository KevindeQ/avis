#include "rendering/render_settings.h"

#include "rendering/yaml_reading.h"

#include <array>
#include <bit>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        using yaml_reading::child_path;
        using yaml_reading::is_given;

        constexpr std::string_view section_path = "render";
        constexpr std::uint32_t maximum_cascades = 4;
        constexpr std::uint32_t maximum_depth_bands = 1u << 24;

        // Reads the keys of one mapping into the settings, leaving a target unchanged when its key isn't given. The
        // first error is kept, and later reads do nothing.
        class section
        {
        public:
            section(const yaml_reading::reader& reader, YAML::Node node, std::string path) :
                reader_{ &reader },
                node_{ std::move(node) },
                path_{ std::move(path) }
            {}

            // A nested section; null or missing reads as empty
            section child(const std::string_view key)
            {
                const YAML::Node node = given(key);
                std::string path = child_path(path_, key);
                if (is_given(node) && !failure_)
                {
                    if (auto checked = reader_->expect_map(node, path); !checked)
                    {
                        failure_ = std::move(checked).error();
                    }
                }
                section nested{ *reader_, is_given(node) && node.IsMap() ? node : YAML::Node{}, std::move(path) };
                nested.failure_ = failure_;
                return nested;
            }

            void read(const std::string_view key, bool& target)
            {
                assign(
                    key,
                    target,
                    [&](const YAML::Node& node, const std::string& path) { return reader_->flag(node, path); });
            }

            void read(const std::string_view key, std::uint32_t& target)
            {
                assign(
                    key,
                    target,
                    [&](const YAML::Node& node, const std::string& path)
                    { return reader_->unsigned_integer_32(node, path); });
            }

            void read(const std::string_view key, std::uint64_t& target)
            {
                assign(
                    key,
                    target,
                    [&](const YAML::Node& node, const std::string& path)
                    { return reader_->unsigned_integer(node, path); });
            }

            void read(const std::string_view key, float& target)
            {
                assign(
                    key,
                    target,
                    [&](const YAML::Node& node, const std::string& path) -> result<float>
                    {
                        auto value = reader_->number(node, path);
                        if (!value)
                        {
                            return std::unexpected{ std::move(value).error() };
                        }
                        return static_cast<float>(*value);
                    });
            }

            void read(const std::string_view key, std::filesystem::path& target)
            {
                assign(
                    key,
                    target,
                    [&](const YAML::Node& node, const std::string& path) -> result<std::filesystem::path>
                    {
                        auto value = reader_->text(node, path);
                        if (!value)
                        {
                            return std::unexpected{ std::move(value).error() };
                        }
                        // UTF-8, so non-ASCII paths survive on Windows
                        return std::filesystem::path{ std::u8string_view{
                            reinterpret_cast<const char8_t*>(value->data()), value->size() } };
                    });
            }

            // One of the names in `choices`
            template<class Enum, std::size_t Count>
            void read_choice(
                const std::string_view key,
                Enum& target,
                const std::array<std::pair<std::string_view, Enum>, Count>& choices)
            {
                assign(
                    key,
                    target,
                    [&](const YAML::Node& node, const std::string& path) -> result<Enum>
                    {
                        auto value = reader_->text(node, path);
                        if (!value)
                        {
                            return std::unexpected{ std::move(value).error() };
                        }
                        std::string expected;
                        for (const auto& [name, choice] : choices)
                        {
                            if (*value == name)
                            {
                                return choice;
                            }
                            expected += expected.empty() ? std::string{ name } : std::format(" or {}", name);
                        }
                        return std::unexpected{
                            reader_->error_at(node, path, std::format("expected {}, got '{}'", expected, *value)),
                        };
                    });
            }

            // Fails at `key` with `message` unless `valid`; only checked when the key was given (defaults are valid)
            void check(const std::string_view key, const bool valid, const std::string_view message)
            {
                if (failure_ || valid)
                {
                    return;
                }
                const YAML::Node node = given(key);
                failure_ = reader_->error_at(is_given(node) ? node : node_, child_path(path_, key), message);
            }

            std::optional<error> take_failure()
            {
                return std::move(failure_);
            }

        private:
            YAML::Node given(const std::string_view key) const
            {
                return node_.IsMap() ? node_[std::string{ key }] : YAML::Node{};
            }

            template<class Target, class Read>
            void assign(const std::string_view key, Target& target, Read&& read_value)
            {
                if (failure_)
                {
                    return;
                }
                const YAML::Node node = given(key);
                if (!is_given(node))
                {
                    return;
                }
                auto value = read_value(node, child_path(path_, key));
                if (!value)
                {
                    failure_ = std::move(value).error();
                    return;
                }
                target = std::move(*value);
            }

            const yaml_reading::reader* reader_;
            YAML::Node node_;
            std::string path_;
            std::optional<error> failure_;
        };

        constexpr std::array exposure_modes{
            std::pair{ std::string_view{ "auto" }, exposure_mode::automatic },
            std::pair{ std::string_view{ "manual" }, exposure_mode::manual },
        };

        constexpr std::array tonemap_operators{
            std::pair{ std::string_view{ "aces" }, tonemap_operator::aces },
            std::pair{ std::string_view{ "agx" }, tonemap_operator::agx },
        };
    } // namespace

    result<render_settings> parse_render_settings(const YAML::Node& node)
    {
        render_settings settings;
        if (!is_given(node))
        {
            return settings;
        }

        const yaml_reading::reader reader;
        if (auto checked = reader.expect_map(node, section_path); !checked)
        {
            return std::unexpected{ std::move(checked).error() };
        }

        section render{ reader, node, std::string{ section_path } };
        render.read("frames_in_flight", settings.frames_in_flight);
        render.check("frames_in_flight", settings.frames_in_flight >= 1, "expected at least 1");
        render.read("async_compute", settings.async_compute);
        render.read("graph_aliasing", settings.graph_aliasing);
        render.read("debug_layer", settings.debug_layer);
        render.read("gpu_validation", settings.gpu_validation);
        render.read("maximum_fps", settings.maximum_fps);
        render.check("maximum_fps", settings.maximum_fps >= 0.0f, "expected 0 (display refresh) or more");
        render.read("shader_directory", settings.shader_directory);
        render.read_choice("tonemapper", settings.tonemapper, tonemap_operators);
        if (auto failure = render.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section shadows = render.child("shadows");
        shadows.read("cascades", settings.shadows.cascades);
        shadows.check(
            "cascades",
            settings.shadows.cascades >= 1 && settings.shadows.cascades <= maximum_cascades,
            std::format("expected 1 to {}, got {}", maximum_cascades, settings.shadows.cascades));
        shadows.read("resolution", settings.shadows.resolution);
        shadows.check("resolution", settings.shadows.resolution >= 1, "expected at least 1");
        shadows.read("distance", settings.shadows.distance);
        shadows.check("distance", settings.shadows.distance > 0.0f, "expected a positive distance");
        if (auto failure = shadows.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section ao = render.child("ao");
        ao.read("half_resolution", settings.ao.half_resolution);
        ao.read("radius", settings.ao.radius);
        ao.check("radius", settings.ao.radius > 0.0f, "expected a positive radius");
        ao.read("slices", settings.ao.slices);
        ao.check("slices", settings.ao.slices >= 1, "expected at least 1");
        ao.read("steps", settings.ao.steps);
        ao.check("steps", settings.ao.steps >= 1, "expected at least 1");
        ao.read("intensity", settings.ao.intensity);
        ao.check("intensity", settings.ao.intensity >= 0.0f, "expected 0 or more");
        if (auto failure = ao.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section taa = render.child("taa");
        taa.read("enabled", settings.taa.enabled);
        if (auto failure = taa.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section exposure = render.child("exposure");
        exposure.read_choice("mode", settings.exposure.mode, exposure_modes);
        exposure.read("ev100", settings.exposure.ev100);
        if (auto failure = exposure.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section progressive = render.child("progressive");
        progressive.read("vertex_budget", settings.progressive.vertex_budget);
        progressive.read("shadow_lod_scale", settings.progressive.shadow_lod_scale);
        progressive.check(
            "shadow_lod_scale",
            settings.progressive.shadow_lod_scale > 0.0f && settings.progressive.shadow_lod_scale <= 1.0f,
            std::format("expected a value in (0, 1], got {}", settings.progressive.shadow_lod_scale));
        if (auto failure = progressive.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section geometry = render.child("geometry");
        geometry.read("arena_mib", settings.geometry.arena_mib);
        geometry.check("arena_mib", settings.geometry.arena_mib >= 1, "expected at least 1");
        if (auto failure = geometry.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section draw_sorting = render.child("draw_sorting");
        draw_sorting.read("opaque_depth_bands", settings.draw_sorting.opaque_depth_bands);
        const std::uint32_t bands = settings.draw_sorting.opaque_depth_bands;
        draw_sorting.check(
            "opaque_depth_bands",
            std::has_single_bit(bands) && bands <= maximum_depth_bands,
            std::format("expected a power of two from 1 to 2^24, got {}", bands));
        if (auto failure = draw_sorting.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        return settings;
    }

    std::string_view to_string(const exposure_mode mode) noexcept
    {
        return mode == exposure_mode::manual ? "manual" : "auto";
    }

    std::string_view to_string(const tonemap_operator value) noexcept
    {
        return value == tonemap_operator::agx ? "agx" : "aces";
    }
} // namespace avis::visualizer::rendering
