#include "labels.h"

#include "avis/log.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/scene.h"

#include <format>
#include <functional>
#include <iterator>
#include <type_traits>
#include <variant>

namespace avis::scene
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";

        // What a field that can't be filled shows
        constexpr std::string_view missing = "–";

        // The cache is cleared past this many templates
        constexpr std::size_t maximum_cached_templates = 256;

        constexpr std::string_view property_prefix = "property:";

        // Appends `value` with `format` ("{:<spec>}", or empty for "{}"). False, with `output` unchanged, when the spec
        // doesn't apply to the value.
        template<typename Value>
        bool append_formatted(std::string& output, const std::string& format, const Value& value)
        {
            const std::size_t size = output.size();
            try
            {
                std::vformat_to(
                    std::back_inserter(output), format.empty() ? "{}" : format, std::make_format_args(value));
                return true;
            }
            catch (const std::format_error&)
            {
                output.resize(size);
                return false;
            }
        }

        // Numbers get the spec; strings and booleans ignore it; a vector is "(x, y, z)" with the spec per element.
        // An integer whose spec only suits floating point (".1f") is formatted as a double.
        bool append_value(std::string& output, const std::string& format, const components::property_value& value)
        {
            return std::visit(
                [&](const auto& held) -> bool
                {
                    using held_type = std::decay_t<decltype(held)>;
                    if constexpr (std::is_same_v<held_type, bool>)
                    {
                        output += held ? "true" : "false";
                        return true;
                    }
                    else if constexpr (std::is_same_v<held_type, std::string>)
                    {
                        output += held;
                        return true;
                    }
                    else if constexpr (std::is_same_v<held_type, std::int64_t>)
                    {
                        return append_formatted(output, format, held) ||
                               append_formatted(output, format, static_cast<double>(held));
                    }
                    else if constexpr (std::is_same_v<held_type, double>)
                    {
                        return append_formatted(output, format, held);
                    }
                    else // Eigen::Vector3d
                    {
                        const std::size_t size = output.size();
                        output += '(';
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            if (axis > 0)
                            {
                                output += ", ";
                            }
                            const double element = held[axis];
                            if (!append_formatted(output, format, element))
                            {
                                output.resize(size);
                                return false;
                            }
                        }
                        output += ')';
                        return true;
                    }
                },
                value);
        }
    } // namespace

    namespace detail
    {
        std::string label_formatter::format(
            const std::string_view text_template,
            const components::identity& identity,
            const components::properties* values)
        {
            const std::uint64_t key = std::hash<std::string_view>{}(text_template);
            auto cached = cache_.find(key);
            parsed_template uncached;
            parsed_template* parsed = nullptr;
            if (cached != cache_.end() && cached->second.source == text_template)
            {
                parsed = &cached->second;
            }
            else if (cached != cache_.end())
            {
                uncached = parse(text_template); // a hash collision: parse it every time
                parsed = &uncached;
            }
            else
            {
                if (cache_.size() >= maximum_cached_templates)
                {
                    cache_.clear();
                }
                parsed = &cache_.emplace(key, parse(text_template)).first->second;
            }

            std::string output;
            output.reserve(text_template.size() + 16);
            bool problem = false;
            for (const segment& part : parsed->segments)
            {
                switch (part.kind)
                {
                case field_kind::literal:
                    output += part.text;
                    break;
                case field_kind::name:
                    output += identity.name;
                    break;
                case field_kind::id:
                    std::format_to(std::back_inserter(output), "{}", identity.id);
                    break;
                case field_kind::property:
                    {
                        const components::property_entry* entry = values != nullptr ? values->find(part.text) : nullptr;
                        if (entry == nullptr)
                        {
                            output += missing; // not a problem with the template: the Driver didn't send it
                        }
                        else if (append_value(output, part.format, entry->value))
                        {
                            if (!entry->unit.empty())
                            {
                                output += ' ';
                                output += entry->unit;
                            }
                        }
                        else
                        {
                            output += missing;
                            problem = true;
                        }
                        break;
                    }
                case field_kind::invalid:
                    output += missing;
                    problem = true;
                    break;
                }
            }

            if (problem && !parsed->logged)
            {
                parsed->logged = true;
                log::get(logger_name)
                    ->warn(
                        "format_label: the template \"{}\" has an unknown field, an unclosed brace or a format spec "
                        "that doesn't suit its property; those fields show \"{}\"",
                        parsed->source,
                        missing);
            }
            return output;
        }

        label_formatter::parsed_template label_formatter::parse(const std::string_view text_template)
        {
            parsed_template parsed{ .source = std::string{ text_template } };
            std::string literal;
            const auto flush = [&]
            {
                if (!literal.empty())
                {
                    parsed.segments.push_back({ .kind = field_kind::literal, .text = std::move(literal) });
                    literal.clear();
                }
            };

            std::size_t position = 0;
            while (position < text_template.size())
            {
                const char character = text_template[position];
                const bool doubled = position + 1 < text_template.size() && text_template[position + 1] == character;
                if (character == '}')
                {
                    literal += '}'; // "}}" is the escape; a lone '}' is kept as it is
                    position += doubled ? 2 : 1;
                    continue;
                }
                if (character != '{')
                {
                    literal += character;
                    ++position;
                    continue;
                }
                if (doubled)
                {
                    literal += '{';
                    position += 2;
                    continue;
                }

                flush();
                const std::size_t close = text_template.find('}', position + 1);
                if (close == std::string_view::npos)
                {
                    parsed.segments.push_back(
                        { .kind = field_kind::invalid, .text = std::string{ text_template.substr(position) } });
                    break;
                }

                const std::string_view field = text_template.substr(position + 1, close - position - 1);
                position = close + 1;
                if (field == "name")
                {
                    parsed.segments.push_back({ .kind = field_kind::name });
                }
                else if (field == "id")
                {
                    parsed.segments.push_back({ .kind = field_kind::id });
                }
                else if (field.starts_with(property_prefix) && field.size() > property_prefix.size())
                {
                    const std::string_view rest = field.substr(property_prefix.size());
                    const std::size_t colon = rest.find(':');
                    segment property{ .kind = field_kind::property, .text = std::string{ rest.substr(0, colon) } };
                    if (colon != std::string_view::npos)
                    {
                        property.format = std::format("{{:{}}}", rest.substr(colon + 1));
                    }
                    parsed.segments.push_back(std::move(property));
                }
                else
                {
                    parsed.segments.push_back({ .kind = field_kind::invalid, .text = std::string{ field } });
                }
            }
            flush();
            return parsed;
        }
    } // namespace detail

    std::string scene::format_label(const entity entity) const
    {
        check_thread("format_label");
        if (!check_alive(entity, "format_label"))
        {
            return {};
        }

        const auto& identity = registry_.get<components::identity>(entity);
        const auto* text = registry_.try_get<components::label>(entity);
        return labels_->format(
            text != nullptr ? std::string_view{ text->text_template } : std::string_view{},
            identity,
            registry_.try_get<components::properties>(entity));
    }

    std::string scene::format_label(const std::string_view text_template, const entity entity) const
    {
        check_thread("format_label");
        if (!check_alive(entity, "format_label"))
        {
            return {};
        }
        return labels_->format(
            text_template,
            registry_.get<components::identity>(entity),
            registry_.try_get<components::properties>(entity));
    }
} // namespace avis::scene
