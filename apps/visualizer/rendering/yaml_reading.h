#ifndef AVIS_VISUALIZER_RENDERING_YAML_READING_H
#define AVIS_VISUALIZER_RENDERING_YAML_READING_H

// Typed reads from yaml-cpp nodes with errors that carry the position and the key path, shared by the renderer's
// parsers (render settings, shader manifest) and the Visualizer's (representation settings, visualizer.yaml). Errors
// are error_domain::parse and read
//   "<source>:<line>:<column>: <key path>: <message>"
// with the source left out when it's empty, and 0:0 when yaml-cpp has no position (as runtime_configuration::parse).

#include "avis/error.h"

#include <yaml-cpp/yaml.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace avis::visualizer::rendering::yaml_reading
{
    class reader
    {
    public:
        explicit reader(std::string_view source_name = {}) : source_name_{ source_name } {}

        error error_at(const YAML::Mark& mark, std::string_view path, std::string_view message) const;
        error error_at(const YAML::Node& node, std::string_view path, std::string_view message) const;

        // The value of `key` in `map`, or a "missing key" error at the map's position
        result<YAML::Node> required(const YAML::Node& map, std::string_view key, std::string_view map_path) const;

        // Fails at the first key of `map` that isn't in `allowed`
        result<void> check_keys(
            const YAML::Node& map, std::string_view map_path, std::initializer_list<std::string_view> allowed) const;

        result<void> expect_map(const YAML::Node& node, std::string_view path) const;
        result<void> expect_sequence(const YAML::Node& node, std::string_view path) const;

        result<std::string> text(const YAML::Node& node, std::string_view path) const;
        result<std::vector<std::string>> text_sequence(const YAML::Node& node, std::string_view path) const;
        // A decimal integer in [0, 2^64), without sign or base prefix
        result<std::uint64_t> unsigned_integer(const YAML::Node& node, std::string_view path) const;
        result<std::uint32_t> unsigned_integer_32(const YAML::Node& node, std::string_view path) const;
        // A finite number
        result<double> number(const YAML::Node& node, std::string_view path) const;
        result<bool> flag(const YAML::Node& node, std::string_view path) const;

    private:
        std::string_view source_name_;
    };

    // "<parent>.<key>", or "<key>" when the parent is empty
    std::string child_path(std::string_view parent, std::string_view key);
    // "<parent>[<index>]"
    std::string element_path(std::string_view parent, std::size_t index);

    // True when the node exists and isn't null: an absent key and "key:" or "key: ~" both mean "not given"
    inline bool is_given(const YAML::Node& node)
    {
        return node.IsDefined() && !node.IsNull();
    }

    // Reads the keys of one settings mapping into typed targets, leaving a target unchanged when its key isn't given,
    // so missing keys keep their defaults. Unknown keys are ignored. The first error is kept, and later reads do
    // nothing. Shared by the settings parsers (render, representation, the Visualizer's configuration).
    class section
    {
    public:
        // `node` may be null or missing (reads as empty); a given node must be a mapping
        section(const reader& reader, const YAML::Node& node, std::string path);

        // A nested section; null or missing reads as empty
        section child(std::string_view key);

        void read(std::string_view key, bool& target);
        void read(std::string_view key, std::uint32_t& target);
        void read(std::string_view key, std::uint64_t& target);
        void read(std::string_view key, float& target);
        void read(std::string_view key, std::string& target);
        // UTF-8, so non-ASCII paths survive on Windows
        void read(std::string_view key, std::filesystem::path& target);

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
        void check(std::string_view key, bool valid, std::string_view message);

        // For reads the typed overloads don't cover: the given node of `key` (or an undefined node), its key path,
        // and a way to record a failure
        YAML::Node given(std::string_view key) const;
        std::string path_of(std::string_view key) const;
        void fail(error failure);
        bool failed() const noexcept
        {
            return failure_.has_value();
        }

        const reader& reader_of() const noexcept
        {
            return *reader_;
        }

        std::optional<error> take_failure()
        {
            return std::move(failure_);
        }

    private:
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
            auto value = read_value(node, path_of(key));
            if (!value)
            {
                failure_ = std::move(value).error();
                return;
            }
            target = std::move(*value);
        }

        const reader* reader_;
        YAML::Node node_;
        std::string path_;
        std::optional<error> failure_;
    };

    // A settings value as a YAML scalar, for writing settings back out. Numbers use the shortest text that reads back
    // to the same value ("0.4", not "0.400000006").
    YAML::Node scalar(bool value);
    YAML::Node scalar(std::uint64_t value);
    YAML::Node scalar(std::uint32_t value);
    YAML::Node scalar(float value);
    YAML::Node scalar(std::string_view value);
    YAML::Node scalar(const char* value) = delete; // would pick the bool overload; pass a std::string_view
    YAML::Node path_scalar(const std::filesystem::path& value); // UTF-8, with forward slashes

    // An empty mapping that's written in flow style ({ a: 1, b: 2 }), for short nested settings
    YAML::Node flow_map();
} // namespace avis::visualizer::rendering::yaml_reading

#endif
