#include "representation/geometry_representation.h"

#include "avis/log.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/error.h"

#include <format>
#include <utility>
#include <xxhash.h>

namespace avis::visualizer::representation
{
    namespace
    {
        using namespace avis::scene::components;
        using scene::entity;

        constexpr const char* logger_name = "visualizer";

        template<typename Stream>
        std::uint64_t hash_stream(const Stream& stream, const std::uint64_t seed) noexcept
        {
            const std::uint64_t size = stream.size();
            const std::uint64_t sized = XXH3_64bits_withSeed(&size, sizeof(size), seed);
            return stream.empty() ? sized :
                                    XXH3_64bits_withSeed(stream.data(), stream.size() * sizeof(stream[0]), sized);
        }

        template<typename Value>
        std::uint64_t hash_value(const Value& value, const std::uint64_t seed) noexcept
        {
            return XXH3_64bits_withSeed(&value, sizeof(value), seed);
        }

        bool has_colors(const geometry_chunk& chunk) noexcept
        {
            return !chunk.colors.empty();
        }

        // Smooth normals for lit triangles that came without any: each triangle adds its area-weighted normal
        std::vector<Eigen::Vector3f> smooth_normals(const geometry_chunk& chunk)
        {
            std::vector<Eigen::Vector3f> normals(chunk.positions.size(), Eigen::Vector3f::Zero());
            const auto add = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c)
            {
                const Eigen::Vector3f face =
                    (chunk.positions[b] - chunk.positions[a]).cross(chunk.positions[c] - chunk.positions[a]);
                normals[a] += face;
                normals[b] += face;
                normals[c] += face;
            };
            if (chunk.indices.empty())
            {
                for (std::uint32_t first = 0; first + 2 < chunk.positions.size(); first += 3)
                {
                    add(first, first + 1, first + 2);
                }
            }
            else
            {
                for (std::size_t first = 0; first + 2 < chunk.indices.size(); first += 3)
                {
                    add(chunk.indices[first], chunk.indices[first + 1], chunk.indices[first + 2]);
                }
            }
            for (Eigen::Vector3f& normal : normals)
            {
                const float length = normal.norm();
                normal = length > 0.0f ? Eigen::Vector3f(normal / length) : Eigen::Vector3f::UnitZ();
            }
            return normals;
        }

        avis::error invalid_chunk(const geometry_chunk& chunk, const std::string_view reason)
        {
            return scene::make_error(
                scene::error_kind::invalid_mesh, std::format("belief geometry chunk {}: {}", chunk.id, reason));
        }

        material_key material_of(const geometry_chunk& chunk)
        {
            const geometry_style& style = chunk.style;
            const bool lines =
                chunk.topology == geometry_topology::lines || chunk.topology == geometry_topology::line_strip;
            const bool points = chunk.topology == geometry_topology::points;
            material_key key{ .role = material_role::geometry,
                              .base_color = style.style_color,
                              .opacity = style.opacity,
                              .lit = style.lit,
                              .vertex_colors = has_colors(chunk) };
            if (key.vertex_colors)
            {
                key.base_color = { .alpha = style.style_color.alpha }; // the vertex colours replace the colour
            }
            if (lines)
            {
                key.line_width = style.line_width;
            }
            if (points)
            {
                key.point_size = style.point_size;
            }
            return key;
        }

        scene::draw_layer layer_of(const geometry_style& style) noexcept
        {
            return style.layer == scene::draw_layer::overlay ? scene::draw_layer::overlay : scene::draw_layer::world;
        }
    } // namespace

    result<scene::mesh_data> build_chunk_mesh(const geometry_chunk& chunk)
    {
        const std::size_t count = chunk.positions.size();
        if (!chunk.normals.empty() && chunk.normals.size() != count)
        {
            return std::unexpected(invalid_chunk(chunk, "the normals don't match the positions"));
        }
        if (!chunk.colors.empty() && chunk.colors.size() != count)
        {
            return std::unexpected(invalid_chunk(chunk, "the colours don't match the positions"));
        }
        for (const std::uint32_t index : chunk.indices)
        {
            if (index >= count)
            {
                return std::unexpected(invalid_chunk(chunk, std::format("index {} is out of range", index)));
            }
        }
        const std::size_t corners = chunk.indices.empty() ? count : chunk.indices.size();
        if (chunk.topology == geometry_topology::triangles && corners % 3 != 0)
        {
            return std::unexpected(invalid_chunk(chunk, "the triangles aren't complete"));
        }
        if (chunk.topology == geometry_topology::lines && corners % 2 != 0)
        {
            return std::unexpected(invalid_chunk(chunk, "the lines aren't complete"));
        }

        scene::mesh_topology topology = scene::mesh_topology::triangles;
        switch (chunk.topology)
        {
        case geometry_topology::triangles:
            topology = scene::mesh_topology::triangles;
            break;
        case geometry_topology::lines:
        case geometry_topology::line_strip:
            topology = scene::mesh_topology::lines;
            break;
        case geometry_topology::points:
            topology = scene::mesh_topology::points;
            break;
        }
        const bool points = topology == scene::mesh_topology::points;

        std::vector<Eigen::Vector3f> computed;
        const std::vector<Eigen::Vector3f>* normals = chunk.normals.empty() ? nullptr : &chunk.normals;
        if (normals == nullptr && chunk.style.lit && topology == scene::mesh_topology::triangles)
        {
            computed = smooth_normals(chunk);
            normals = &computed;
        }

        // A line strip through its indices, or its vertices in order, as a line list
        std::vector<std::uint32_t> strip;
        if (chunk.topology == geometry_topology::line_strip)
        {
            if (chunk.indices.empty())
            {
                strip.resize(count);
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    strip[index] = index;
                }
            }
            else
            {
                strip = chunk.indices;
            }
            if (strip.size() < 2)
            {
                return scene::mesh_builder{ topology }.build(); // nothing to draw
            }
        }

        scene::mesh_builder builder{ topology, points };
        const auto add_vertex = [&](const std::uint32_t index)
        {
            builder.add_vertex(chunk.positions[index]);
            if (normals != nullptr)
            {
                builder.normal((*normals)[index]);
            }
            if (!chunk.colors.empty())
            {
                builder.color(chunk.colors[index]);
            }
        };

        if (points && !chunk.indices.empty())
        {
            // Indexed points: the vertices the indices name, in their order
            builder.reserve(chunk.indices.size(), 0);
            for (const std::uint32_t index : chunk.indices)
            {
                add_vertex(index);
            }
        }
        else
        {
            builder.reserve(count, strip.empty() ? chunk.indices.size() : 2 * (strip.size() - 1));
            for (std::uint32_t index = 0; index < count; ++index)
            {
                add_vertex(index);
            }
            if (!strip.empty())
            {
                for (std::size_t segment = 1; segment < strip.size(); ++segment)
                {
                    builder.add_index(strip[segment - 1]).add_index(strip[segment]);
                }
            }
            else if (!points)
            {
                for (const std::uint32_t index : chunk.indices)
                {
                    builder.add_index(index);
                }
            }
        }

        result<scene::mesh_data> data = std::move(builder).build();
        if (data && points)
        {
            shuffle_progressive(*data);
        }
        return data;
    }

    std::uint64_t hash_chunk(const geometry_chunk& chunk) noexcept
    {
        std::uint64_t hash = hash_value(chunk.topology, 0);
        hash = hash_value(chunk.origin.x(), hash);
        hash = hash_value(chunk.origin.y(), hash);
        hash = hash_value(chunk.origin.z(), hash);
        hash = hash_stream(chunk.positions, hash);
        hash = hash_stream(chunk.indices, hash);
        hash = hash_stream(chunk.normals, hash);
        hash = hash_stream(chunk.colors, hash);
        const geometry_style& style = chunk.style;
        for (const float value : { style.style_color.red,
                                   style.style_color.green,
                                   style.style_color.blue,
                                   style.style_color.alpha,
                                   style.opacity,
                                   style.line_width,
                                   style.point_size })
        {
            hash = hash_value(value, hash);
        }
        hash = hash_value(style.lit, hash);
        return hash_value(style.layer, hash);
    }

    bool geometry_representation::update(scene::scene& scene, const entity owner)
    {
        if (!scene.alive(owner))
        {
            return owners_.erase(owner) != 0; // its helpers were children: they went with it
        }

        const belief_geometry* geometry = scene.registry().try_get<belief_geometry>(owner);
        if (geometry == nullptr)
        {
            const auto found = owners_.find(owner);
            if (found == owners_.end())
            {
                return false;
            }
            release_all(scene, found->second);
            owners_.erase(found);
            return true;
        }

        bool changed = false;
        owner_state& chunks = owners_[owner];

        // Removed chunks, then new and changed ones (both lists are sorted by id)
        for (auto record = chunks.begin(); record != chunks.end();)
        {
            if (geometry->find(record->first) == nullptr)
            {
                release_dynamic_mesh(scene, record->second.mesh);
                record = chunks.erase(record);
                changed = true;
            }
            else
            {
                ++record;
            }
        }
        for (const geometry_chunk& chunk : geometry->chunks())
        {
            const std::uint64_t hash = hash_chunk(chunk);
            const auto found = chunks.find(chunk.id);
            if (found != chunks.end() && found->second.hash == hash)
            {
                continue;
            }
            chunk_state& state = found != chunks.end() ? found->second : chunks[chunk.id];
            state.hash = hash;
            changed = true;
            result<scene::mesh_data> data = build_chunk_mesh(chunk);
            if (!data)
            {
                log::get(logger_name)
                    ->warn(
                        "representation: {:#x} has belief geometry that can't be drawn: {}",
                        scene.registry().get<identity>(owner).id,
                        data.error());
                release_dynamic_mesh(scene, state.mesh);
                state.mesh.pending.reset();
                state.drawable = false; // until it changes again
                continue;
            }
            state.drawable = true;
            state.mesh.set_pending(std::move(*data));
            state.mesh.changed = true;
        }
        if (chunks.empty())
        {
            owners_.erase(owner);
        }
        return changed;
    }

    void geometry_representation::collect(const scene::scene& scene, std::vector<budget_item>& items)
    {
        collected_.clear();
        for (const auto& [owner, chunks] : owners_)
        {
            for (const auto& [id, state] : chunks)
            {
                if (state.drawable)
                {
                    items.push_back(state.mesh.item(scene, budget_kind::chunk));
                    collected_.emplace_back(owner, id);
                }
            }
        }
    }

    void geometry_representation::execute(scene::scene& scene, const std::size_t index, const budget_decision& decision)
    {
        if (index >= collected_.size() || decision.action == budget_action::keep)
        {
            return;
        }
        const auto [owner, id] = collected_[index];
        const auto found = owners_.find(owner);
        if (found == owners_.end() || !found->second.contains(id))
        {
            return;
        }
        chunk_state& state = found->second.at(id);
        if (decision.action == budget_action::release)
        {
            release_dynamic_mesh(scene, state.mesh);
            return;
        }

        if (const result<void> built = build(scene, owner, id, state, decision.vertices); !built)
        {
            log::get(logger_name)
                ->error(
                    "representation: belief geometry chunk {} of {:#x} could not be built: {}",
                    id,
                    scene.registry().get<identity>(owner).id,
                    built.error());
        }
    }

    result<void> geometry_representation::build(
        scene::scene& scene,
        const entity owner,
        const std::uint32_t id,
        chunk_state& state,
        const std::uint32_t vertices)
    {
        const belief_geometry* geometry = scene.registry().try_get<belief_geometry>(owner);
        const geometry_chunk* chunk = geometry != nullptr ? geometry->find(id) : nullptr;
        if (chunk == nullptr)
        {
            return {}; // gone since update(): the next update releases it
        }

        result<scene::mesh_data> data = state.mesh.pending ? std::move(*state.mesh.pending) : build_chunk_mesh(*chunk);
        state.mesh.pending.reset();
        if (!data)
        {
            return std::unexpected(std::move(data.error()));
        }
        const result<scene::material_handle> material = materials_.get(material_of(*chunk));
        if (!material)
        {
            return std::unexpected(material.error());
        }

        scene::transform local;
        local.position = chunk->origin.cast<float>();
        return build_dynamic_mesh(
            scene,
            state.mesh,
            std::move(*data),
            vertices,
            { .materials = { *material }, .layer = layer_of(chunk->style), .flags = overlay_flags, .local = local },
            owner,
            owner,
            std::format("chunk {}", id));
    }

    void geometry_representation::finish() noexcept
    {
        for (auto& [owner, chunks] : owners_)
        {
            for (auto& [id, state] : chunks)
            {
                state.mesh.pending.reset();
            }
        }
    }

    std::uint64_t geometry_representation::used_bytes(const scene::scene& scene) const noexcept
    {
        std::uint64_t bytes = 0;
        for (const auto& [owner, chunks] : owners_)
        {
            for (const auto& [id, state] : chunks)
            {
                bytes += state.mesh.exists(scene) ? state.mesh.current_bytes : 0;
            }
        }
        return bytes;
    }

    entity geometry_representation::chunk_helper(const entity owner, const std::uint32_t chunk) const noexcept
    {
        const dynamic_mesh* mesh = chunk_mesh(owner, chunk);
        return mesh != nullptr ? mesh->helper : entt::null;
    }

    const dynamic_mesh*
        geometry_representation::chunk_mesh(const entity owner, const std::uint32_t chunk) const noexcept
    {
        const auto found = owners_.find(owner);
        if (found == owners_.end())
        {
            return nullptr;
        }
        const auto record = found->second.find(chunk);
        return record != found->second.end() ? &record->second.mesh : nullptr;
    }

    void geometry_representation::release_all(scene::scene& scene, owner_state& chunks)
    {
        for (auto& [id, state] : chunks)
        {
            release_dynamic_mesh(scene, state.mesh);
        }
        chunks.clear();
    }
} // namespace avis::visualizer::representation
