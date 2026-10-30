#include "avis/graphics/pipeline_cache.h"

#include "avis/graphics/error.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace avis::graphics
{
    namespace
    {
        // The packed cache key: each field written on its own, so padding never reaches the hash
        class key_writer
        {
        public:
            template<class T>
                requires(std::is_integral_v<T> || std::is_enum_v<T> || std::is_floating_point_v<T>)
            void put(const T value) noexcept
            {
                static_assert(sizeof(T) <= 8);
                // Floats by their bits: 0.0 and -0.0 are different keys, which only costs a duplicate PSO
                std::memcpy(bytes_.data() + size_, &value, sizeof(T));
                size_ += sizeof(T);
            }

            template<class Tag>
            void put(const rhi::handle<Tag> handle) noexcept
            {
                put<std::uint64_t>(std::hash<rhi::handle<Tag>>{}(handle));
            }

            void put(const std::optional<rhi::root_signature_handle>& signature) noexcept
            {
                put<std::uint8_t>(signature ? 1 : 0);
                if (signature)
                {
                    put(*signature);
                }
            }

            std::uint64_t hash() const noexcept
            {
                return XXH3_64bits(bytes_.data(), size_);
            }

        private:
            std::array<std::byte, 256> bytes_{}; // a graphics key is 131 bytes
            std::size_t size_ = 0;
        };

        // A new field in these structures changes their size and fails here: add it to the packing below, then
        // update the size
        static_assert(sizeof(rhi::rasterizer_state) == 20, "pack the new rasterizer_state field in pack_state");
        static_assert(sizeof(rhi::depth_state) == 3, "pack the new depth_state field in pack_state");
        static_assert(sizeof(rhi::blend_state) == 8, "pack the new blend_state field in pack_state");
        static_assert(sizeof(rhi::graphics_pipeline_state) == 104, "pack the new graphics_pipeline_state field");

        void pack_state(key_writer& key, const rhi::graphics_pipeline_state& state) noexcept
        {
            key.put(state.rasterizer.cull);
            key.put(state.rasterizer.fill);
            key.put(state.rasterizer.front_counter_clockwise);
            key.put(state.rasterizer.depth_bias);
            key.put(state.rasterizer.slope_scaled_depth_bias);
            key.put(state.rasterizer.depth_bias_clamp);
            key.put(state.rasterizer.depth_clip);

            key.put(state.depth.test);
            key.put(state.depth.write);
            key.put(state.depth.compare);

            for (const rhi::blend_state& blend : state.blend)
            {
                key.put(blend.enable);
                key.put(blend.source_color);
                key.put(blend.destination_color);
                key.put(blend.color_operation);
                key.put(blend.source_alpha);
                key.put(blend.destination_alpha);
                key.put(blend.alpha_operation);
                key.put(blend.write_mask);
            }

            key.put(state.render_target_count);
            for (const rhi::format format : state.render_target_formats)
            {
                key.put(format);
            }
            key.put(state.depth_format);
            key.put(state.topology);
        }

        std::uint64_t key_of(const shader_library& shaders, const graphics_pipeline_description& description) noexcept
        {
            key_writer key;
            key.put<std::uint8_t>(1); // graphics
            key.put(shaders.serial());
            key.put(description.vertex_shader.hash);
            key.put(description.pixel_shader.hash);
            key.put(description.root_signature);
            pack_state(key, description.state);
            return key.hash();
        }

        std::uint64_t key_of(const shader_library& shaders, const compute_pipeline_description& description) noexcept
        {
            key_writer key;
            key.put<std::uint8_t>(2); // compute
            key.put(shaders.serial());
            key.put(description.compute_shader.hash);
            key.put(description.root_signature);
            return key.hash();
        }

        std::string_view stage_name(const shader_stage stage) noexcept
        {
            switch (stage)
            {
            case shader_stage::vertex:
                return "vertex";
            case shader_stage::pixel:
                return "pixel";
            case shader_stage::compute:
                return "compute";
            }
            return "unknown";
        }

        // "pipeline 'name'" for messages, or "pipeline" without a debug name
        std::string subject(const std::string_view debug_name)
        {
            return debug_name.empty() ? std::string{ "pipeline" } : std::format("pipeline '{}'", debug_name);
        }

        // Looks a shader up for one slot of a pipeline and checks its stage
        result<shader_library::entry> resolve(
            const shader_library& shaders,
            const shader_id id,
            const shader_stage slot,
            const std::string_view debug_name)
        {
            const auto entry = shaders.find(id);
            if (!entry)
            {
                // An unknown id has no string to name, only its hash
                return std::unexpected(make_error(
                    error_kind::shader_not_found,
                    std::format(
                        "{}: no {} shader with id hash {:016x} in shader library '{}'",
                        subject(debug_name),
                        stage_name(slot),
                        id.hash,
                        shaders.name())));
            }
            if (entry->stage != slot)
            {
                return std::unexpected(make_error(
                    error_kind::shader_stage_mismatch,
                    std::format(
                        "{}: shader '{}' is a {} shader, used as the {} shader",
                        subject(debug_name),
                        entry->id,
                        stage_name(entry->stage),
                        stage_name(slot))));
            }
            return *entry;
        }

        // A registry error with the pipeline's name in front
        avis::error for_pipeline(avis::error error, const std::string_view debug_name)
        {
            error.message = std::format("{}: {}", subject(debug_name), error.message);
            return error;
        }

        // The named signature checked against the stages with bindings, or the first compatible candidate
        result<rhi::root_signature_handle> resolve_root_signature(
            const root_signature_registry& registry,
            const std::optional<rhi::root_signature_handle> named,
            const std::span<const shader_library::entry> stages,
            const std::string_view debug_name)
        {
            std::vector<root_signature_registry::stage_bindings> bindings;
            bindings.reserve(stages.size());
            for (const shader_library::entry& stage : stages)
            {
                if (stage.bindings != nullptr)
                {
                    bindings.push_back({ .stage = stage.stage, .bindings = stage.bindings });
                }
                else if (!named)
                {
                    return std::unexpected(make_error(
                        error_kind::shader_bindings_missing,
                        std::format(
                            "{}: shader '{}' has no bindings, so the pipeline must name its root signature",
                            subject(debug_name),
                            stage.id)));
                }
            }

            if (named)
            {
                if (auto checked = registry.check_compatible(*named, bindings); !checked)
                {
                    return std::unexpected(for_pipeline(std::move(checked.error()), debug_name));
                }
                return *named;
            }
            auto found = registry.find_compatible(bindings);
            if (!found)
            {
                return std::unexpected(for_pipeline(std::move(found.error()), debug_name));
            }
            return *found;
        }

        // The shared-locked lookup, and the insert that keeps the first handle for a key (D-105)
        template<class Handle>
        std::optional<Handle> find_cached(
            std::shared_mutex& mutex, const std::unordered_map<std::uint64_t, Handle>& map, const std::uint64_t key)
        {
            const std::shared_lock lock{ mutex };
            if (const auto found = map.find(key); found != map.end())
            {
                return found->second;
            }
            return std::nullopt;
        }

        template<class Handle>
        Handle insert_cached(
            std::shared_mutex& mutex,
            std::unordered_map<std::uint64_t, Handle>& map,
            const std::uint64_t key,
            const Handle handle)
        {
            const std::unique_lock lock{ mutex };
            // Another worker may have created the same pipeline meanwhile: its handle wins, ours stays alive unused
            return map.try_emplace(key, handle).first->second;
        }
    } // namespace

    pipeline_cache::pipeline_cache(const rhi::pipelines pipelines, const root_signature_registry& root_signatures) :
        pipelines_{ pipelines },
        root_signatures_{ root_signatures }
    {}

    pipeline_cache::~pipeline_cache() = default;

    result<rhi::graphics_pipeline_handle>
        pipeline_cache::get_or_create(const shader_library& shaders, const graphics_pipeline_description& description)
    {
        const std::uint64_t key = key_of(shaders, description);
        if (const auto cached = find_cached(mutex_, graphics_, key))
        {
            return *cached;
        }

        std::array<shader_library::entry, 2> stages{};
        std::size_t stage_count = 0;
        auto vertex = resolve(shaders, description.vertex_shader, shader_stage::vertex, description.debug_name);
        if (!vertex)
        {
            return std::unexpected(std::move(vertex.error()));
        }
        stages[stage_count++] = *vertex;

        std::span<const std::byte> pixel_bytecode; // empty = depth-only
        if (description.pixel_shader != shader_id{})
        {
            auto pixel = resolve(shaders, description.pixel_shader, shader_stage::pixel, description.debug_name);
            if (!pixel)
            {
                return std::unexpected(std::move(pixel.error()));
            }
            stages[stage_count++] = *pixel;
            pixel_bytecode = pixel->bytecode;
        }

        auto signature = resolve_root_signature(
            root_signatures_,
            description.root_signature,
            std::span{ stages.data(), stage_count },
            description.debug_name);
        if (!signature)
        {
            return std::unexpected(std::move(signature.error()));
        }

        auto pipeline = pipelines_.create_pipeline(
            rhi::graphics_pipeline_description{
                .root_signature = *signature,
                .vertex_shader = vertex->bytecode,
                .pixel_shader = pixel_bytecode,
                .state = description.state,
                .debug_name = description.debug_name,
            });
        if (!pipeline)
        {
            return std::unexpected(std::move(pipeline.error()));
        }
        return insert_cached(mutex_, graphics_, key, *pipeline);
    }

    result<rhi::compute_pipeline_handle>
        pipeline_cache::get_or_create(const shader_library& shaders, const compute_pipeline_description& description)
    {
        const std::uint64_t key = key_of(shaders, description);
        if (const auto cached = find_cached(mutex_, compute_, key))
        {
            return *cached;
        }

        auto compute = resolve(shaders, description.compute_shader, shader_stage::compute, description.debug_name);
        if (!compute)
        {
            return std::unexpected(std::move(compute.error()));
        }
        auto signature = resolve_root_signature(
            root_signatures_, description.root_signature, std::span{ &*compute, 1 }, description.debug_name);
        if (!signature)
        {
            return std::unexpected(std::move(signature.error()));
        }

        auto pipeline = pipelines_.create_pipeline(
            rhi::compute_pipeline_description{
                .root_signature = *signature,
                .compute_shader = compute->bytecode,
                .debug_name = description.debug_name,
            });
        if (!pipeline)
        {
            return std::unexpected(std::move(pipeline.error()));
        }
        return insert_cached(mutex_, compute_, key, *pipeline);
    }

    std::size_t pipeline_cache::size() const
    {
        const std::shared_lock lock{ mutex_ };
        return graphics_.size() + compute_.size();
    }
} // namespace avis::graphics
