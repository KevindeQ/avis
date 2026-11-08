#include "representation/dynamic_mesh.h"

#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"

#include <format>
#include <utility>
#include <vector>

namespace avis::visualizer::representation
{
    namespace
    {
        std::uint64_t splitmix64(std::uint64_t& state) noexcept
        {
            std::uint64_t z = (state += 0x9e37'79b9'7f4a'7c15ull);
            z = (z ^ (z >> 30)) * 0xbf58'476d'1ce4'e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d0'49bb'1331'11ebull;
            return z ^ (z >> 31);
        }

        // Uniform in [0, bound), without modulo bias worth caring about for a shuffle (Lemire's multiply-shift)
        std::uint32_t below(std::uint64_t& state, const std::uint32_t bound) noexcept
        {
            return static_cast<std::uint32_t>(((splitmix64(state) >> 32) * bound) >> 32);
        }

        template<typename Stream>
        void permute(Stream& stream, const std::vector<std::uint32_t>& order)
        {
            if (stream.empty())
            {
                return;
            }
            Stream permuted;
            permuted.reserve(order.size());
            for (const std::uint32_t index : order)
            {
                permuted.push_back(stream[index]);
            }
            stream = std::move(permuted);
        }

        template<typename Stream>
        void truncate(Stream& stream, const std::size_t size)
        {
            if (stream.size() > size)
            {
                stream.resize(size);
            }
        }
    } // namespace

    void dynamic_mesh::set_pending(scene::mesh_data data)
    {
        progressive = data.progressive;
        full_bytes = data.byte_size();
        full_vertices = static_cast<std::uint32_t>(data.positions.size());
        pending = std::move(data);
    }

    budget_item dynamic_mesh::item(
        const scene::scene& scene,
        const budget_kind kind,
        const std::uint64_t sensor,
        const std::int64_t age) const noexcept
    {
        const bool present = exists(scene);
        return { .kind = kind,
                 .progressive = progressive,
                 .full_bytes = full_bytes,
                 .full_vertices = full_vertices,
                 .exists = present,
                 .current_bytes = present ? current_bytes : 0,
                 .current_vertices = present ? current_vertices : 0,
                 .changed = changed || !present,
                 .sensor = sensor,
                 .age = age };
    }

    void shuffle_progressive(scene::mesh_data& data, const std::uint64_t seed)
    {
        const auto count = static_cast<std::uint32_t>(data.positions.size());
        std::vector<std::uint32_t> order(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            order[index] = index;
        }
        std::uint64_t state = seed;
        for (std::uint32_t index = count; index > 1; --index)
        {
            std::swap(order[index - 1], order[below(state, index)]);
        }
        permute(data.positions, order);
        permute(data.normals, order);
        permute(data.tangents, order);
        permute(data.tangent_signs, order);
        permute(data.uv0, order);
        permute(data.color0, order);
        data.progressive = true;
    }

    void truncate_progressive(scene::mesh_data& data, const std::uint32_t vertices)
    {
        if (vertices >= data.positions.size())
        {
            return;
        }
        truncate(data.positions, vertices);
        truncate(data.normals, vertices);
        truncate(data.tangents, vertices);
        truncate(data.tangent_signs, vertices);
        truncate(data.uv0, vertices);
        truncate(data.color0, vertices);
        data.primitives.resize(1);
        data.primitives.front() = { .vertex_count = vertices };
    }

    result<void> build_dynamic_mesh(
        scene::scene& scene,
        dynamic_mesh& mesh,
        scene::mesh_data data,
        const std::uint32_t vertices,
        const part_specification& part,
        const scene::entity parent,
        const scene::entity owner,
        const std::string& label)
    {
        if (data.progressive)
        {
            truncate_progressive(data, vertices);
        }
        const std::uint64_t bytes = data.byte_size();
        const auto count = static_cast<std::uint32_t>(data.positions.size());

        part_specification specification = part;
        if (mesh.exists(scene))
        {
            specification.mesh = scene.registry().get<scene::components::mesh>(mesh.helper).asset;
            if (result<void> updated = scene.assets().update_mesh(specification.mesh, std::move(data)); !updated)
            {
                return updated;
            }
        }
        else
        {
            mesh.helper = entt::null;
            result<scene::mesh_handle> created = scene.assets().create_mesh(std::move(data));
            if (!created)
            {
                return std::unexpected(std::move(created.error()));
            }
            const result<scene::entity> helper = scene.create_entity(
                { .name = std::format("{} · {}", scene.registry().get<scene::components::identity>(owner).name, label),
                  .category = scene::entity_category::helper,
                  .parent = parent,
                  .local = part.local,
                  .represents = owner });
            if (!helper)
            {
                scene.assets().release(*created);
                return std::unexpected(helper.error());
            }
            mesh.helper = *helper;
            specification.mesh = *created;
            apply_part(scene, mesh.helper, specification);
            scene.assets().release(*created); // the helper holds it now
        }
        apply_part(scene, mesh.helper, specification);

        mesh.current_bytes = bytes;
        mesh.current_vertices = count;
        mesh.changed = false;
        return {};
    }

    void release_dynamic_mesh(scene::scene& scene, dynamic_mesh& mesh)
    {
        if (mesh.exists(scene))
        {
            static_cast<void>(scene.destroy_entity(mesh.helper));
        }
        mesh.helper = entt::null;
        mesh.current_bytes = 0;
        mesh.current_vertices = 0;
        mesh.changed = true;
    }
} // namespace avis::visualizer::representation
