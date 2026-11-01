#ifndef AVIS_VISUALIZER_RENDERING_YAML_READING_H
#define AVIS_VISUALIZER_RENDERING_YAML_READING_H

// Typed reads from yaml-cpp nodes with errors that carry the position and the key path, shared by the renderer's
// parsers (render settings, shader manifest). Errors are error_domain::parse and read
//   "<source>:<line>:<column>: <key path>: <message>"
// with the source left out when it's empty, and 0:0 when yaml-cpp has no position (as runtime_configuration::parse).

#include "avis/error.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
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
} // namespace avis::visualizer::rendering::yaml_reading

#endif
