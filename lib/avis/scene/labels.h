#ifndef AVIS_SCENE_LABELS_H
#define AVIS_SCENE_LABELS_H

// Private: label templates (docs/design/08-scene-management.md §8.1), for scene::format_label.
//
//     "{name} · {property:perception.speed:.1f}"  →  "car 1042 · 8.3 m/s"
//
// Fields: {name}, {id}, {property:<key>} and {property:<key>:<std::format spec>}. {{ and }} are escapes. A missing key,
// an entity without properties, a bad spec or an unknown field gives "–"; a bad spec or field logs once per template.

#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace avis::scene::detail
{
    class label_formatter
    {
    public:
        // `values` may be null: every property field then gives "–"
        std::string format(
            std::string_view text_template, const components::identity& identity, const components::properties* values);

        // Parsed templates kept; for tests
        std::size_t cached_templates() const noexcept
        {
            return cache_.size();
        }

    private:
        enum class field_kind : std::uint8_t
        {
            literal,
            name,
            id,
            property,
            invalid // an unknown field or an unclosed brace
        };

        struct segment
        {
            field_kind kind = field_kind::literal;
            std::string text; // the literal text, the property key, or the invalid field
            std::string format; // "{:<spec>}", or empty for the default format
        };

        struct parsed_template
        {
            std::string source;
            std::vector<segment> segments;
            bool logged = false; // a problem was logged for this template
        };

        static parsed_template parse(std::string_view text_template);

        // Parsed templates by the hash of their text. Templates come from the configuration, so there are few; the
        // cache empties itself if that stops being true.
        std::unordered_map<std::uint64_t, parsed_template> cache_;
    };
} // namespace avis::scene::detail

#endif
