#include "rendering/yaml_reading.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <system_error>

namespace avis::visualizer::rendering::yaml_reading
{
    namespace
    {
        std::string_view kind_of(const YAML::Node& node)
        {
            switch (node.Type())
            {
            case YAML::NodeType::Null:
                return "null";
            case YAML::NodeType::Scalar:
                return "a scalar";
            case YAML::NodeType::Sequence:
                return "a sequence";
            case YAML::NodeType::Map:
                return "a mapping";
            case YAML::NodeType::Undefined:
            default:
                return "nothing";
            }
        }
    } // namespace

    error reader::error_at(const YAML::Mark& mark, const std::string_view path, const std::string_view message) const
    {
        // yaml-cpp counts from 0 and uses -1 when it has no position
        const int line = mark.line >= 0 ? mark.line + 1 : 0;
        const int column = mark.column >= 0 ? mark.column + 1 : 0;
        std::string text = source_name_.empty() ? std::format("{}:{}: ", line, column) :
                                                  std::format("{}:{}:{}: ", source_name_, line, column);
        if (!path.empty())
        {
            text += std::format("{}: ", path);
        }
        text += message;
        return error{ .domain = error_domain::parse, .code = 0, .message = std::move(text) };
    }

    error reader::error_at(const YAML::Node& node, const std::string_view path, const std::string_view message) const
    {
        return error_at(node.IsDefined() ? node.Mark() : YAML::Mark::null_mark(), path, message);
    }

    result<YAML::Node>
        reader::required(const YAML::Node& map, const std::string_view key, const std::string_view map_path) const
    {
        const YAML::Node node = map[std::string{ key }];
        if (!node.IsDefined())
        {
            return std::unexpected{ error_at(map, child_path(map_path, key), "missing required key") };
        }
        return node;
    }

    result<void> reader::check_keys(
        const YAML::Node& map,
        const std::string_view map_path,
        const std::initializer_list<std::string_view> allowed) const
    {
        for (const auto& item : map)
        {
            const YAML::Node& key = item.first;
            if (!key.IsScalar())
            {
                return std::unexpected{ error_at(key, map_path, "expected a string key") };
            }
            if (std::ranges::find(allowed, std::string_view{ key.Scalar() }) == allowed.end())
            {
                return std::unexpected{ error_at(key, child_path(map_path, key.Scalar()), "unknown key") };
            }
        }
        return {};
    }

    result<void> reader::expect_map(const YAML::Node& node, const std::string_view path) const
    {
        if (!node.IsMap())
        {
            return std::unexpected{ error_at(node, path, std::format("expected a mapping, got {}", kind_of(node))) };
        }
        return {};
    }

    result<void> reader::expect_sequence(const YAML::Node& node, const std::string_view path) const
    {
        if (!node.IsSequence())
        {
            return std::unexpected{ error_at(node, path, std::format("expected a sequence, got {}", kind_of(node))) };
        }
        return {};
    }

    result<std::string> reader::text(const YAML::Node& node, const std::string_view path) const
    {
        if (!node.IsScalar())
        {
            return std::unexpected{ error_at(node, path, std::format("expected a string, got {}", kind_of(node))) };
        }
        return node.Scalar();
    }

    result<std::vector<std::string>> reader::text_sequence(const YAML::Node& node, const std::string_view path) const
    {
        if (auto checked = expect_sequence(node, path); !checked)
        {
            return std::unexpected{ std::move(checked).error() };
        }
        std::vector<std::string> values;
        values.reserve(node.size());
        for (std::size_t index = 0; index < node.size(); ++index)
        {
            auto value = text(node[index], element_path(path, index));
            if (!value)
            {
                return std::unexpected{ std::move(value).error() };
            }
            values.push_back(std::move(*value));
        }
        return values;
    }

    result<std::uint64_t> reader::unsigned_integer(const YAML::Node& node, const std::string_view path) const
    {
        if (node.IsScalar())
        {
            const std::string& scalar = node.Scalar();
            std::uint64_t value = 0;
            const auto [end, status] = std::from_chars(scalar.data(), scalar.data() + scalar.size(), value);
            if (!scalar.empty() && status == std::errc{} && end == scalar.data() + scalar.size())
            {
                return value;
            }
            if (status == std::errc::result_out_of_range)
            {
                return std::unexpected{ error_at(node, path, std::format("'{}' is out of range", scalar)) };
            }
            return std::unexpected{
                error_at(node, path, std::format("expected an unsigned integer, got '{}'", scalar)),
            };
        }
        return std::unexpected{
            error_at(node, path, std::format("expected an unsigned integer, got {}", kind_of(node))),
        };
    }

    result<std::uint32_t> reader::unsigned_integer_32(const YAML::Node& node, const std::string_view path) const
    {
        auto value = unsigned_integer(node, path);
        if (!value)
        {
            return std::unexpected{ std::move(value).error() };
        }
        if (*value > std::numeric_limits<std::uint32_t>::max())
        {
            return std::unexpected{ error_at(
                node, path, std::format("{} is out of range (at most 2^32 - 1)", *value)) };
        }
        return static_cast<std::uint32_t>(*value);
    }

    result<double> reader::number(const YAML::Node& node, const std::string_view path) const
    {
        if (node.IsScalar())
        {
            const std::string& scalar = node.Scalar();
            double value = 0.0;
            const auto [end, status] = std::from_chars(scalar.data(), scalar.data() + scalar.size(), value);
            if (!scalar.empty() && status == std::errc{} && end == scalar.data() + scalar.size() &&
                std::isfinite(value))
            {
                return value;
            }
            return std::unexpected{ error_at(node, path, std::format("expected a number, got '{}'", scalar)) };
        }
        return std::unexpected{ error_at(node, path, std::format("expected a number, got {}", kind_of(node))) };
    }

    result<bool> reader::flag(const YAML::Node& node, const std::string_view path) const
    {
        if (node.IsScalar())
        {
            bool value = false;
            if (YAML::convert<bool>::decode(node, value))
            {
                return value;
            }
            return std::unexpected{
                error_at(node, path, std::format("expected true or false, got '{}'", node.Scalar())),
            };
        }
        return std::unexpected{ error_at(node, path, std::format("expected true or false, got {}", kind_of(node))) };
    }

    std::string child_path(const std::string_view parent, const std::string_view key)
    {
        return parent.empty() ? std::string{ key } : std::format("{}.{}", parent, key);
    }

    std::string element_path(const std::string_view parent, const std::size_t index)
    {
        return std::format("{}[{}]", parent, index);
    }
} // namespace avis::visualizer::rendering::yaml_reading
