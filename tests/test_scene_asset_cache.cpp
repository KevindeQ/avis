// The asset cache: versions, reference counts, byte counters, materials and textures, concurrency
// (docs/tasks/10-scene-management.md §10.5)

#include "helpers/recorded_assertions.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/gltf.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/error.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace avis;

namespace
{
    // A triangle list with `triangles` triangles, offset along x so versions differ
    scene::mesh_data make_mesh(const int triangles, const float offset = 0.0f)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        for (int vertex = 0; vertex < triangles * 3; ++vertex)
        {
            builder.add_vertex({ offset + static_cast<float>(vertex), 0.0f, static_cast<float>(vertex % 3) })
                .normal(Eigen::Vector3f::UnitZ());
        }
        result<scene::mesh_data> built = std::move(builder).build();
        if (!built)
        {
            throw std::logic_error{ "make_mesh: the builder failed" }; // no Catch2 assertion: workers call this too
        }
        return std::move(*built);
    }

    scene::texture_data make_texture(const std::uint32_t size)
    {
        scene::texture_data texture;
        texture.width = size;
        texture.height = size;
        texture.mips.emplace_back(std::size_t{ size } * size * 4);
        return texture;
    }
} // namespace

TEST_CASE("update_mesh bumps the version and leaves earlier versions intact", "[scene][unit]")
{
    scene::asset_cache cache;
    const result<scene::mesh_handle> created = cache.create_mesh(make_mesh(1));
    REQUIRE(created);
    const scene::mesh_handle mesh = *created;

    const scene::versioned_mesh first = cache.current(mesh);
    CHECK(first.version == 1);
    REQUIRE(first.data != nullptr);
    CHECK(first.data->positions.size() == 3);
    CHECK_FALSE(first.data->bounds.empty()); // computed by the cache
    CHECK(first.data->bounds.maximum.x() == 2.0f);

    REQUIRE(cache.update_mesh(mesh, make_mesh(2, 100.0f)));
    const scene::versioned_mesh second = cache.current(mesh);
    CHECK(second.version == 2);
    CHECK(second.data->positions.size() == 6);
    CHECK(second.data->bounds.minimum.x() == 100.0f);

    // The earlier version is unchanged
    CHECK(first.data->positions.size() == 3);
    CHECK(first.data->positions[0].x() == 0.0f);

    // A failed update keeps the current version
    scene::mesh_data broken = make_mesh(1);
    broken.indices = { 7 };
    broken.primitives[0].index_count = 1;
    const result<void> failed = cache.update_mesh(mesh, std::move(broken));
    REQUIRE_FALSE(failed);
    CHECK(scene::is_error(failed.error(), scene::error_kind::invalid_mesh));
    CHECK(cache.current(mesh).version == 2);

    REQUIRE(cache.update_mesh(mesh, make_mesh(1)));
    CHECK(cache.current(mesh).version == 3);

    // An invalid mesh is never stored
    scene::mesh_data empty_primitives = make_mesh(1);
    empty_primitives.primitives.clear();
    const result<scene::mesh_handle> rejected = cache.create_mesh(std::move(empty_primitives));
    REQUIRE_FALSE(rejected);
    CHECK(scene::is_error(rejected.error(), scene::error_kind::invalid_mesh));
    CHECK(cache.statistics().meshes == 1);

    cache.release(mesh);
}

TEST_CASE("a mesh version's bytes move from current to retained and go when the last holder lets go", "[scene][unit]")
{
    scene::asset_cache cache;
    const scene::asset_statistics start = cache.statistics();
    CHECK(start.current_geometry_bytes == 0);
    CHECK(start.retained_geometry_bytes == 0);

    const scene::mesh_data first_data = make_mesh(2);
    const std::uint64_t first_bytes = first_data.byte_size();
    const scene::mesh_handle mesh = *cache.create_mesh(first_data);
    CHECK(cache.statistics().current_geometry_bytes == first_bytes);

    // Two outside holders, standing in for snapshot slots
    scene::versioned_mesh slot_a = cache.current(mesh);
    scene::versioned_mesh slot_b = cache.current(mesh);

    const scene::mesh_data second_data = make_mesh(5);
    const std::uint64_t second_bytes = second_data.byte_size();
    REQUIRE(cache.update_mesh(mesh, second_data));
    CHECK(cache.statistics().current_geometry_bytes == second_bytes);
    CHECK(cache.statistics().retained_geometry_bytes == first_bytes);

    slot_a = {};
    CHECK(cache.statistics().retained_geometry_bytes == first_bytes); // slot_b still holds it
    slot_b = cache.current(mesh); // the slot is reused for the new version
    CHECK(cache.statistics().retained_geometry_bytes == 0);

    // The last release supersedes the current version; a snapshot keeps it alive
    cache.release(mesh);
    CHECK(cache.statistics().meshes == 0);
    CHECK(cache.statistics().current_geometry_bytes == 0);
    CHECK(cache.statistics().retained_geometry_bytes == second_bytes);
    CHECK(slot_b.data->positions.size() == 15);
    slot_b = {};
    CHECK(cache.statistics().retained_geometry_bytes == start.retained_geometry_bytes);
}

TEST_CASE("versions outliving the cache don't touch it", "[scene][unit]")
{
    scene::versioned_mesh survivor;
    {
        scene::asset_cache cache;
        const scene::mesh_handle mesh = *cache.create_mesh(make_mesh(1));
        survivor = cache.current(mesh);
        cache.release(mesh);
    }
    CHECK(survivor.data->positions.size() == 3);
    survivor = {}; // its deleter decrements counters that outlive the cache
}

TEST_CASE("reference counts free a slot on the last release and stale handles are caught", "[scene][unit]")
{
    scene::asset_cache cache;
    const scene::mesh_handle mesh = *cache.create_mesh(make_mesh(1));
    cache.retain(mesh);
    cache.release(mesh);
    CHECK(cache.statistics().meshes == 1);
    CHECK(cache.current(mesh).data != nullptr);
    cache.release(mesh);
    CHECK(cache.statistics().meshes == 0);

    {
        tests::recorded_assertions assertions;
        const scene::versioned_mesh stale = cache.current(mesh);
        CHECK(stale.data == nullptr);
        CHECK(stale.version == 0);
        cache.retain(mesh);
        cache.release(mesh);
        CHECK(cache.statistics().meshes == 0);
#ifndef NDEBUG
        CHECK(assertions.count() == 3);
        CHECK(assertions.contains("asset_cache::current: stale or null asset handle"));
#endif
    }

    // The slot is reused with a higher generation, and the old handle stays stale
    const scene::mesh_handle reused = *cache.create_mesh(make_mesh(1));
    CHECK(reused.index == mesh.index);
    CHECK(reused.generation > mesh.generation);
    CHECK(cache.current(reused).version == 1);
    {
        tests::recorded_assertions assertions;
        CHECK(cache.current(mesh).data == nullptr);
    }
    cache.release(reused);
}

TEST_CASE("materials check and hold their textures", "[scene][unit]")
{
    scene::asset_cache cache;
    const scene::texture_handle base = *cache.create_texture(make_texture(4));
    const scene::texture_handle normal = *cache.create_texture(make_texture(2));
    CHECK(cache.statistics().textures == 2);
    CHECK(cache.current(base).data->byte_size() == 64);
    CHECK(cache.statistics().current_geometry_bytes == 0); // texture bytes aren't geometry bytes

    SECTION("a dead texture fails with invalid_material")
    {
        const scene::texture_handle dead = *cache.create_texture(make_texture(1));
        cache.release(dead);

        scene::material_description description;
        description.debug_name = "broken";
        description.normal_texture = dead;
        const result<scene::material_handle> failed = cache.create_material(description);
        REQUIRE_FALSE(failed);
        CHECK(scene::is_error(failed.error(), scene::error_kind::invalid_material));
        CHECK(failed.error().message.contains("broken"));
        CHECK(cache.statistics().materials == 0);
    }

    SECTION("a freed material releases its textures")
    {
        scene::material_description description;
        description.base_color_texture = base;
        description.emissive_texture = base; // twice: retained and released twice
        description.normal_texture = normal;
        const scene::material_handle material = *cache.create_material(description);
        CHECK(cache.current(material).data->base_color_texture == base);

        cache.release(base); // the material still holds it
        CHECK(cache.statistics().textures == 2);
        CHECK(cache.current(base).data != nullptr);

        cache.release(material);
        CHECK(cache.statistics().materials == 0);
        CHECK(cache.statistics().textures == 1); // base went with the material; normal is still the test's
        tests::recorded_assertions assertions;
        CHECK(cache.current(base).data == nullptr);
    }

    SECTION("update_material swaps the textures it holds")
    {
        scene::material_description description;
        description.base_color_texture = base;
        const scene::material_handle material = *cache.create_material(description);
        cache.release(base);

        const scene::versioned_material first = cache.current(material);
        description.base_color_texture = normal;
        description.debug_name = "second";
        REQUIRE(cache.update_material(material, description));
        CHECK(cache.current(material).version == 2);
        CHECK(cache.current(material).data->debug_name == "second");
        CHECK(first.data->base_color_texture == base); // the earlier version is unchanged
        CHECK(cache.statistics().textures == 1); // base was freed by the update

        // A failed update changes nothing
        const scene::texture_handle dead = *cache.create_texture(make_texture(1));
        cache.release(dead);
        description.occlusion_texture = dead;
        const result<void> failed = cache.update_material(material, description);
        REQUIRE_FALSE(failed);
        CHECK(scene::is_error(failed.error(), scene::error_kind::invalid_material));
        CHECK(cache.current(material).version == 2);

        cache.release(material);
        CHECK(cache.statistics().textures == 1); // normal: the test's own reference
    }

    cache.release(normal);
    if (cache.statistics().textures == 1)
    {
        cache.release(base);
    }
    CHECK(cache.statistics().textures == 0);
}

TEST_CASE("release_all releases every handle of a set", "[scene][unit]")
{
    scene::asset_cache cache;
    scene::gltf_asset_set set;
    set.textures.push_back(*cache.create_texture(make_texture(2)));
    scene::material_description description;
    description.base_color_texture = set.textures[0];
    set.materials.push_back(*cache.create_material(description));
    set.meshes.push_back(*cache.create_mesh(make_mesh(1)));
    set.meshes.push_back(*cache.create_mesh(make_mesh(2)));

    cache.release_all(set);
    const scene::asset_statistics after = cache.statistics();
    CHECK(after.meshes == 0);
    CHECK(after.materials == 0);
    CHECK(after.textures == 0);
    CHECK(after.current_geometry_bytes == 0);
    CHECK(after.retained_geometry_bytes == 0);
}

TEST_CASE("the destructor reports live assets", "[scene][unit]")
{
    tests::recorded_assertions assertions;
    {
        scene::asset_cache cache;
        [[maybe_unused]] const scene::mesh_handle leaked = *cache.create_mesh(make_mesh(1));
    }
#ifndef NDEBUG
    CHECK(assertions.count() == 1);
    CHECK(assertions.contains("asset_cache destroyed with live assets: 1 meshes"));
#endif

    assertions.clear();
    {
        scene::asset_cache cache;
        cache.release(*cache.create_mesh(make_mesh(1)));
    }
    CHECK(assertions.count() == 0);
}

TEST_CASE("concurrent creates, updates, currents, retains and releases keep exact counts", "[scene][unit]")
{
    scene::asset_cache cache;

    // Old versions handed to the dropper thread, standing in for snapshots letting go elsewhere
    std::mutex drop_mutex;
    std::vector<scene::versioned_mesh> to_drop;
    std::atomic<bool> workers_done{ false };
    std::atomic<int> failures{ 0 }; // Catch2 assertions aren't thread-safe, so workers count instead

    struct owned
    {
        std::map<scene::mesh_handle, int> meshes; // handle → references this thread holds
        std::map<scene::material_handle, int> materials;
        std::map<scene::texture_handle, int> textures;
    };
    std::vector<owned> threads_owned(4);

    const auto worker = [&](const unsigned seed, owned& mine) {
        std::mt19937 random{ seed };
        std::uniform_int_distribution<int> operation{ 0, 9 };
        std::uniform_int_distribution<int> size{ 1, 8 };
        const auto pick = [&random](auto& map) {
            std::uniform_int_distribution<std::size_t> index{ 0, map.size() - 1 };
            return std::next(map.begin(), static_cast<std::ptrdiff_t>(index(random)));
        };
        const auto hand_over = [&](scene::versioned_mesh version) {
            const std::lock_guard lock{ drop_mutex };
            to_drop.push_back(std::move(version));
        };

        for (int step = 0; step < 10'000; ++step)
        {
            switch (operation(random))
            {
            case 0:
                mine.meshes[*cache.create_mesh(make_mesh(size(random)))] = 1;
                break;
            case 1:
                if (!mine.meshes.empty())
                {
                    const scene::mesh_handle mesh = pick(mine.meshes)->first;
                    hand_over(cache.current(mesh));
                    if (!cache.update_mesh(mesh, make_mesh(size(random))))
                    {
                        failures.fetch_add(1);
                    }
                }
                break;
            case 2:
                if (!mine.meshes.empty())
                {
                    hand_over(cache.current(pick(mine.meshes)->first));
                }
                break;
            case 3:
                if (!mine.meshes.empty())
                {
                    auto entry = pick(mine.meshes);
                    cache.retain(entry->first);
                    ++entry->second;
                }
                break;
            case 4:
                if (!mine.meshes.empty())
                {
                    auto entry = pick(mine.meshes);
                    cache.release(entry->first);
                    if (--entry->second == 0)
                    {
                        mine.meshes.erase(entry);
                    }
                }
                break;
            case 5:
                mine.textures[*cache.create_texture(make_texture(2))] = 1;
                break;
            case 6:
            {
                scene::material_description description;
                if (!mine.textures.empty())
                {
                    description.base_color_texture = pick(mine.textures)->first;
                }
                mine.materials[*cache.create_material(description)] = 1;
                break;
            }
            case 7:
                if (!mine.materials.empty())
                {
                    scene::material_description description;
                    if (!mine.textures.empty())
                    {
                        description.normal_texture = pick(mine.textures)->first;
                    }
                    if (!cache.update_material(pick(mine.materials)->first, description))
                    {
                        failures.fetch_add(1);
                    }
                }
                break;
            case 8:
                if (!mine.materials.empty())
                {
                    auto entry = pick(mine.materials);
                    cache.release(entry->first);
                    mine.materials.erase(entry);
                }
                break;
            default:
                if (!mine.textures.empty())
                {
                    auto entry = pick(mine.textures);
                    cache.release(entry->first);
                    mine.textures.erase(entry);
                }
                break;
            }
        }
    };

    std::thread dropper{ [&] {
        std::vector<scene::versioned_mesh> batch;
        while (true)
        {
            const bool last = workers_done.load(std::memory_order_acquire);
            {
                const std::lock_guard lock{ drop_mutex };
                batch.swap(to_drop);
            }
            batch.clear(); // the deleters run here, on this thread
            if (last)
            {
                break;
            }
            std::this_thread::yield();
        }
    } };

    {
        std::vector<std::jthread> workers;
        for (unsigned index = 0; index < 4; ++index)
        {
            workers.emplace_back(worker, 1000 + index, std::ref(threads_owned[index]));
        }
    }
    workers_done.store(true, std::memory_order_release);
    dropper.join();
    REQUIRE(to_drop.empty());
    CHECK(failures.load() == 0);

    // Recompute from what the threads hold
    std::uint32_t meshes = 0;
    std::uint32_t materials = 0;
    std::uint64_t current_bytes = 0;
    std::set<scene::texture_handle> textures;
    for (const owned& mine : threads_owned)
    {
        meshes += static_cast<std::uint32_t>(mine.meshes.size());
        materials += static_cast<std::uint32_t>(mine.materials.size());
        for (const auto& [mesh, references] : mine.meshes)
        {
            current_bytes += cache.current(mesh).data->byte_size();
        }
        for (const auto& [texture, references] : mine.textures)
        {
            textures.insert(texture);
        }
        for (const auto& [material, references] : mine.materials)
        {
            const scene::material_description& description = *cache.current(material).data;
            for (const scene::texture_handle texture : { description.base_color_texture, description.normal_texture })
            {
                if (texture.valid())
                {
                    textures.insert(texture); // held by the material even if the thread released it
                }
            }
        }
    }
    const scene::asset_statistics statistics = cache.statistics();
    CHECK(statistics.meshes == meshes);
    CHECK(statistics.materials == materials);
    CHECK(statistics.textures == textures.size());
    CHECK(statistics.current_geometry_bytes == current_bytes);
    CHECK(statistics.retained_geometry_bytes == 0);

    // Releasing everything leaves an empty cache
    for (const owned& mine : threads_owned)
    {
        for (const auto& [mesh, references] : mine.meshes)
        {
            for (int count = 0; count < references; ++count)
            {
                cache.release(mesh);
            }
        }
        for (const auto& [material, references] : mine.materials)
        {
            cache.release(material);
        }
        for (const auto& [texture, references] : mine.textures)
        {
            cache.release(texture);
        }
    }
    const scene::asset_statistics empty = cache.statistics();
    CHECK(empty.meshes == 0);
    CHECK(empty.materials == 0);
    CHECK(empty.textures == 0);
    CHECK(empty.current_geometry_bytes == 0);
    CHECK(empty.retained_geometry_bytes == 0);
}
