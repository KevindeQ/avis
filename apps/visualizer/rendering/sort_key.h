#ifndef AVIS_VISUALIZER_RENDERING_SORT_KEY_H
#define AVIS_VISUALIZER_RENDERING_SORT_KEY_H

// The 64-bit draw-list sort key (docs/design/11-renderer.md §4.2, §4.3, docs/tasks/11-renderer.md §11.6).
//
//     63–62 fullscreen layer | 61–59 viewport | 58–56 viewport layer | 55–54 translucency | 53 command
//     draw items (53 = 0):    52–29 depth | 28–17 pipeline set | 16–0 material
//     command items (53 = 1): 52–48 sequence | 47–0 command data
//                             command data: 47 = 0 built-in: 46–39 opcode, 38–0 immediate
//                                           47 = 1 callback: 46–0 function address
//
// The top 10 bits are the item's *group* (key_group); with the command bit below them, they are the 11 bits shared
// by all items (11 §4.2). So a command sorts after every draw of its group and before the next group. Key
// construction and decoding are constexpr. Every field is checked against its width: a value out of range fails a debug
// check on the "renderer" logger (a compile error in a constant expression) and is truncated to its width, so it can't
// spill into the other fields.
//
// The depth field means something different per group (11 §4.3): quantize_depth gives the logarithmic view distance,
// and the *_depth_field helpers turn it into the field for opaque, translucent, overlay and fullscreen-effect items.

#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <variant>

namespace avis::visualizer::rendering
{
    class command_context; // the range walker's context for commands (11 §4.7, 11.9)

    // Bits 63–62
    enum class fullscreen_layer : std::uint8_t
    {
        scene = 0,
        fullscreen_effects = 1,
        hud = 2,
        // 3 is reserved
    };

    // Bits 58–56
    enum class viewport_layer : std::uint8_t
    {
        background = 0,
        world = 1,
        effects = 2,
        overlay = 3,
        overlay_on_top = 4,
        // 5–7 are reserved
    };

    // Bits 55–54, in draw order
    enum class translucency : std::uint8_t
    {
        opaque = 0,
        blended = 1,
        additive = 2,
        subtractive = 3,
    };

    // Built-in commands (11 §4.7)
    enum class command_opcode : std::uint8_t
    {
        clear_depth_stencil = 0,
        clear_render_target = 1,
        set_viewport_scissor = 2,
        set_stencil_reference = 3,
        set_write_mask = 4,
        reset_state = 5,
    };

    // Shadow cascades have viewport slots 1–4
    inline constexpr std::uint32_t maximum_shadow_cascades = 4;

    namespace sort_key_detail
    {
        // Fails the debug check for a field out of range. Not constexpr: reaching it in a constant expression is a
        // compile error.
        void report_out_of_range(std::string_view field, std::uint64_t value, std::uint64_t limit) noexcept;

        // `value` when it is below `limit`; otherwise the debug check fails and `value` is masked by `mask`
        constexpr std::uint64_t checked(
            const std::string_view field,
            const std::uint64_t value,
            const std::uint64_t limit,
            const std::uint64_t mask) noexcept
        {
            if (value >= limit)
            {
                report_out_of_range(field, value, limit);
                return value & mask;
            }
            return value;
        }
    } // namespace sort_key_detail

    // Bits 61–59: the viewpoint within the view. 0 is the camera, 1–4 the shadow cascades, 5–7 are reserved.
    struct viewport
    {
        std::uint8_t index = 0;

        static constexpr viewport camera() noexcept
        {
            return {};
        }

        // Cascade `cascade` (0–3)
        static constexpr viewport shadow_cascade(const std::uint32_t cascade) noexcept
        {
            return { static_cast<std::uint8_t>(
                1 + sort_key_detail::checked(
                        "viewport::shadow_cascade", cascade, maximum_shadow_cascades, maximum_shadow_cascades - 1)) };
        }

        constexpr bool is_camera() const noexcept
        {
            return index == 0;
        }

        constexpr bool is_shadow_cascade() const noexcept
        {
            return index >= 1 && index <= maximum_shadow_cascades;
        }

        // The cascade of a shadow_cascade viewport
        constexpr std::uint32_t cascade() const noexcept
        {
            return static_cast<std::uint32_t>(index) - 1;
        }

        friend constexpr bool operator==(viewport, viewport) = default;
        friend constexpr auto operator<=>(viewport, viewport) = default;
    };

    // Bit positions and widths of 11 §4.2
    namespace key_fields
    {
        struct field
        {
            std::uint32_t position;
            std::uint32_t width;

            constexpr std::uint64_t limit() const noexcept
            {
                return std::uint64_t{ 1 } << width;
            }

            // The field's value bits, unshifted
            constexpr std::uint64_t value_mask() const noexcept
            {
                return limit() - 1;
            }

            // The field's bits in the key
            constexpr std::uint64_t key_mask() const noexcept
            {
                return value_mask() << position;
            }

            constexpr std::uint64_t extract(const std::uint64_t key) const noexcept
            {
                return (key >> position) & value_mask();
            }
        };

        // Shared by every item
        inline constexpr field fullscreen_layer{ 62, 2 };
        inline constexpr field viewport{ 59, 3 };
        inline constexpr field viewport_layer{ 56, 3 };
        inline constexpr field translucency{ 54, 2 };
        inline constexpr field command{ 53, 1 };

        // Draw items
        inline constexpr field depth{ 29, 24 };
        inline constexpr field material_field{ 0, 29 }; // pipeline set + material index
        inline constexpr field pipeline_set{ 17, 12 };
        inline constexpr field material{ 0, 17 };

        // Command items
        inline constexpr field sequence{ 48, 5 };
        inline constexpr field command_data{ 0, 48 };
        inline constexpr field callback{ 47, 1 };
        inline constexpr field opcode{ 39, 8 };
        inline constexpr field immediate{ 0, 39 };
        inline constexpr field callback_address{ 0, 47 };

        constexpr bool adjacent(const field high, const field low) noexcept
        {
            return low.position + low.width == high.position;
        }

        static_assert(fullscreen_layer.position + fullscreen_layer.width == 64);
        static_assert(adjacent(fullscreen_layer, viewport));
        static_assert(adjacent(viewport, viewport_layer));
        static_assert(adjacent(viewport_layer, translucency));
        static_assert(adjacent(translucency, command));
        static_assert(fullscreen_layer.width + viewport.width + viewport_layer.width + translucency.width == 10);

        static_assert(adjacent(command, depth) && depth.width == 24);
        static_assert(adjacent(depth, material_field));
        static_assert(pipeline_set.position + pipeline_set.width == material_field.width);
        static_assert(adjacent(pipeline_set, material) && material.position == 0);
        static_assert(pipeline_set.limit() == 4096 && material.limit() == 131072);

        static_assert(adjacent(command, sequence) && sequence.limit() == 32);
        static_assert(adjacent(sequence, command_data) && command_data.position == 0);
        static_assert(adjacent(sequence, callback));
        static_assert(adjacent(callback, opcode) && adjacent(opcode, immediate) && immediate.position == 0);
        static_assert(adjacent(callback, callback_address) && callback_address.position == 0);
    } // namespace key_fields

    // The 10-bit prefix every item of a group shares, above the command bit
    struct key_group
    {
        rendering::fullscreen_layer fullscreen_layer = rendering::fullscreen_layer::scene;
        rendering::viewport viewport;
        rendering::viewport_layer viewport_layer = rendering::viewport_layer::world;
        rendering::translucency translucency = rendering::translucency::opaque;

        friend constexpr bool operator==(const key_group&, const key_group&) = default;
    };

    struct draw_key_fields
    {
        key_group group;
        std::uint32_t depth = 0; // 24 bits, per-group meaning (11 §4.3)
        std::uint32_t pipeline_set = 0; // 12 bits (11 §4.4)
        std::uint32_t material = 0; // 17 bits, the material's residency slot

        friend constexpr bool operator==(const draw_key_fields&, const draw_key_fields&) = default;
    };

    // void (*)(command_context&, const void* arguments): the arguments are at the command item's value
    using command_callback = void (*)(command_context& context, const void* arguments);

    struct builtin_command
    {
        command_opcode opcode = command_opcode::reset_state;
        std::uint64_t immediate = 0; // 39 bits

        friend constexpr bool operator==(const builtin_command&, const builtin_command&) = default;
    };

    // A callback command, by function address. Windows x64 user-mode code is below 2⁴⁷, so the address fits in 47
    // bits. ASLR moves it between runs: commands sharing a group and sequence number must not depend on their order.
    struct callback_command
    {
        std::uint64_t address = 0; // 47 bits

        // Fails the debug check when the address is at or above 2⁴⁷
        static callback_command of(const command_callback function) noexcept
        {
            return { sort_key_detail::checked(
                "callback address",
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(function)),
                key_fields::callback_address.limit(),
                key_fields::callback_address.value_mask()) };
        }

        command_callback function() const noexcept
        {
            return reinterpret_cast<command_callback>(static_cast<std::uintptr_t>(address));
        }

        friend constexpr bool operator==(const callback_command&, const callback_command&) = default;
    };

    struct command_key_fields
    {
        key_group group;
        std::uint32_t sequence = 0; // 5 bits: orders the commands of one group
        std::variant<builtin_command, callback_command> command;

        friend constexpr bool operator==(const command_key_fields&, const command_key_fields&) = default;
    };

    using decoded_key = std::variant<draw_key_fields, command_key_fields>;

    namespace sort_key_detail
    {
        constexpr std::uint64_t
            encode(const key_fields::field field, const std::string_view name, const std::uint64_t value) noexcept
        {
            return checked(name, value, field.limit(), field.value_mask()) << field.position;
        }

        constexpr std::uint64_t encode_group(const key_group& group) noexcept
        {
            return encode(
                       key_fields::fullscreen_layer,
                       "fullscreen_layer",
                       static_cast<std::uint64_t>(group.fullscreen_layer)) |
                   encode(key_fields::viewport, "viewport", group.viewport.index) |
                   encode(
                       key_fields::viewport_layer, "viewport_layer", static_cast<std::uint64_t>(group.viewport_layer)) |
                   encode(key_fields::translucency, "translucency", static_cast<std::uint64_t>(group.translucency));
        }

        constexpr key_group decode_group(const std::uint64_t key) noexcept
        {
            return { static_cast<fullscreen_layer>(key_fields::fullscreen_layer.extract(key)),
                     viewport{ static_cast<std::uint8_t>(key_fields::viewport.extract(key)) },
                     static_cast<viewport_layer>(key_fields::viewport_layer.extract(key)),
                     static_cast<translucency>(key_fields::translucency.extract(key)) };
        }
    } // namespace sort_key_detail

    constexpr std::uint64_t make_draw_key(const draw_key_fields& fields) noexcept
    {
        using namespace sort_key_detail;
        return encode_group(fields.group) | encode(key_fields::depth, "depth", fields.depth) |
               encode(key_fields::pipeline_set, "pipeline_set", fields.pipeline_set) |
               encode(key_fields::material, "material", fields.material);
    }

    constexpr std::uint64_t make_command_key(const command_key_fields& fields) noexcept
    {
        using namespace sort_key_detail;
        std::uint64_t key = encode_group(fields.group) | key_fields::command.key_mask() |
                            encode(key_fields::sequence, "sequence", fields.sequence);
        if (const auto* const builtin = std::get_if<builtin_command>(&fields.command))
        {
            key |= encode(key_fields::opcode, "opcode", static_cast<std::uint64_t>(builtin->opcode)) |
                   encode(key_fields::immediate, "immediate", builtin->immediate);
        }
        else
        {
            key |= key_fields::callback.key_mask() | encode(
                                                         key_fields::callback_address,
                                                         "callback address",
                                                         std::get<callback_command>(fields.command).address);
        }
        return key;
    }

    constexpr bool is_command_key(const std::uint64_t key) noexcept
    {
        return key_fields::command.extract(key) != 0;
    }

    constexpr key_group group_of(const std::uint64_t key) noexcept
    {
        return sort_key_detail::decode_group(key);
    }

    constexpr decoded_key decode_key(const std::uint64_t key) noexcept
    {
        const key_group group = sort_key_detail::decode_group(key);
        if (!is_command_key(key))
        {
            return draw_key_fields{ group,
                                    static_cast<std::uint32_t>(key_fields::depth.extract(key)),
                                    static_cast<std::uint32_t>(key_fields::pipeline_set.extract(key)),
                                    static_cast<std::uint32_t>(key_fields::material.extract(key)) };
        }

        command_key_fields fields{ group, static_cast<std::uint32_t>(key_fields::sequence.extract(key)), {} };
        if (key_fields::callback.extract(key) == 0)
        {
            fields.command = builtin_command{ static_cast<command_opcode>(key_fields::opcode.extract(key)),
                                              key_fields::immediate.extract(key) };
        }
        else
        {
            fields.command = callback_command{ key_fields::callback_address.extract(key) };
        }
        return fields;
    }

    // The top `width` bits of a key, for group lookups: a fullscreen layer (2 bits), plus a viewport (5), plus a
    // viewport layer (8), plus a translucency (10, a whole group)
    struct sort_key_prefix
    {
        std::uint64_t bits = 0; // in key position, lower bits zero
        std::uint32_t width = 0;

        friend constexpr bool operator==(const sort_key_prefix&, const sort_key_prefix&) = default;
    };

    constexpr sort_key_prefix key_prefix(const fullscreen_layer layer) noexcept
    {
        return { sort_key_detail::encode(
                     key_fields::fullscreen_layer, "fullscreen_layer", static_cast<std::uint64_t>(layer)),
                 key_fields::fullscreen_layer.width };
    }

    constexpr sort_key_prefix key_prefix(const fullscreen_layer layer, const viewport point) noexcept
    {
        return { key_prefix(layer).bits | sort_key_detail::encode(key_fields::viewport, "viewport", point.index),
                 64 - key_fields::viewport.position };
    }

    constexpr sort_key_prefix
        key_prefix(const fullscreen_layer layer, const viewport point, const viewport_layer view_layer) noexcept
    {
        return { key_prefix(layer, point).bits |
                     sort_key_detail::encode(
                         key_fields::viewport_layer, "viewport_layer", static_cast<std::uint64_t>(view_layer)),
                 64 - key_fields::viewport_layer.position };
    }

    constexpr sort_key_prefix key_prefix(const key_group& group) noexcept
    {
        return { sort_key_detail::encode_group(group), 64 - key_fields::translucency.position };
    }

    // The keys with `prefix`, both ends included
    struct key_range
    {
        std::uint64_t first = 0;
        std::uint64_t last = 0;

        constexpr bool contains(const std::uint64_t key) const noexcept
        {
            return key >= first && key <= last;
        }

        friend constexpr bool operator==(const key_range&, const key_range&) = default;
    };

    constexpr key_range prefix_range(const sort_key_prefix prefix) noexcept
    {
        const std::uint64_t below = prefix.width >= 64 ? 0 : ~std::uint64_t{ 0 } >> prefix.width;
        return { prefix.bits, prefix.bits | below };
    }

    // From the first key of `first` to the last key of `last`, for passes that walk consecutive groups
    // (blended … subtractive, overlay … overlay_on_top)
    constexpr key_range prefix_range(const sort_key_prefix first, const sort_key_prefix last) noexcept
    {
        return { prefix_range(first).first, prefix_range(last).last };
    }

    // Depth quantization (11 §4.3): logarithmic over the cluster depth range
    inline constexpr float depth_near = 0.1f; // metres
    inline constexpr float depth_far = 2000.0f;
    inline constexpr std::uint32_t maximum_depth = (1u << 24) - 1;

    // q = round((2²⁴ − 1) · log(d / near) / log(far / near)), clamped to [0, 2²⁴ − 1]. 0 at or below 0.1 m (and for
    // NaN), 2²⁴ − 1 at or beyond 2 km. Monotonic.
    inline std::uint32_t quantize_depth(const float view_distance) noexcept
    {
        if (!(view_distance > depth_near))
        {
            return 0;
        }
        if (view_distance >= depth_far)
        {
            return maximum_depth;
        }
        const double scale = static_cast<double>(maximum_depth) / std::log(double{ depth_far } / double{ depth_near });
        const double q = std::round(std::log(double{ view_distance } / double{ depth_near }) * scale);
        return q >= maximum_depth ? maximum_depth : static_cast<std::uint32_t>(q);
    }

    // Opaque groups (camera and shadow cascades): the band of `depth`, the top log2(bands) bits with the lower bits
    // zero, so the items of a band sort by pipeline set and material. `bands` is a power of two up to 2²⁴; 1 gives 0.
    constexpr std::uint32_t opaque_depth_field(const std::uint32_t depth, const std::uint32_t bands) noexcept
    {
        std::uint32_t band_count = bands;
        if (!std::has_single_bit(bands) || bands > (1u << 24))
        {
            // Not a power of two up to 2²⁴: no banding
            sort_key_detail::report_out_of_range("opaque depth bands", bands, (1u << 24) + 1);
            band_count = 1;
        }
        const std::uint32_t low_bits = 24 - static_cast<std::uint32_t>(std::countr_zero(band_count));
        const std::uint32_t q = static_cast<std::uint32_t>(
            sort_key_detail::checked("depth", depth, key_fields::depth.limit(), key_fields::depth.value_mask()));
        return low_bits >= 24 ? 0 : (q >> low_bits) << low_bits;
    }

    // Translucent world groups: back to front, 2²⁴ − 1 − q
    constexpr std::uint32_t translucent_depth_field(const std::uint32_t depth) noexcept
    {
        return maximum_depth - static_cast<std::uint32_t>(sort_key_detail::checked(
                                   "depth", depth, key_fields::depth.limit(), key_fields::depth.value_mask()));
    }

    // Overlay groups: layer_priority in bits 23–16 (higher draws later, on top); bits 15–0 are 0 for opaque items and
    // the back-to-front top 16 bits of `depth` for translucent ones
    constexpr std::uint32_t overlay_depth_field(
        const std::uint8_t layer_priority, const bool translucent, const std::uint32_t depth) noexcept
    {
        const std::uint32_t priority = std::uint32_t{ layer_priority } << 16;
        if (!translucent)
        {
            return priority;
        }
        const std::uint32_t q = static_cast<std::uint32_t>(
            sort_key_detail::checked("depth", depth, key_fields::depth.limit(), key_fields::depth.value_mask()));
        return priority | (0xFFFFu - (q >> 8));
    }

    // fullscreen_effects: the stacking order, later on top (debug blit: fullscreen, then picture-in-picture slots)
    constexpr std::uint32_t fullscreen_effects_depth_field(const std::uint32_t stacking_index) noexcept
    {
        return static_cast<std::uint32_t>(sort_key_detail::checked(
            "stacking index", stacking_index, key_fields::depth.limit(), key_fields::depth.value_mask()));
    }
} // namespace avis::visualizer::rendering

#endif
