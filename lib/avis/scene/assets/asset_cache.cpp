#include "avis/scene/assets/asset_cache.h"

#include "avis/detail/check.h"
#include "avis/scene/assets/gltf.h"
#include "avis/scene/error.h"
#include "mesh_validation.h"

#include <array>
#include <atomic>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace avis::scene
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";

        // The §8.5 byte counters. Shared with every mesh version's deleter, so a snapshot that outlives the cache
        // still decrements valid counters.
        struct geometry_counters
        {
            std::atomic<std::uint64_t> current{ 0 };
            std::atomic<std::uint64_t> retained{ 0 };
        };

        // One mesh version and its control block: the data, its size, and which counter holds it
        struct mesh_version
        {
            mesh_data data;
            std::uint64_t bytes = 0;
            std::atomic<bool> retained{ false }; // set once a newer version or the last release supersedes it
            std::shared_ptr<geometry_counters> counters;
        };

        struct new_mesh_version
        {
            std::shared_ptr<const mesh_data> data;
            mesh_version* block = nullptr;
        };

        // Counts the version as current and wraps it in a shared_ptr whose deleter takes it off whichever counter
        // holds it when the last reference goes, on whatever thread that is
        new_mesh_version make_mesh_version(mesh_data data, std::shared_ptr<geometry_counters> counters)
        {
            const std::uint64_t bytes = data.byte_size();
            auto* block = new mesh_version{ .data = std::move(data), .bytes = bytes, .counters = std::move(counters) };
            block->counters->current.fetch_add(bytes, std::memory_order_relaxed);

            // If the shared_ptr can't be created, it calls the deleter, which takes the bytes off again
            std::shared_ptr<mesh_version> owner{ block, [](mesh_version* version) noexcept {
                                                    std::atomic<std::uint64_t>& counter =
                                                        version->retained.load(std::memory_order_acquire) ?
                                                            version->counters->retained :
                                                            version->counters->current;
                                                    counter.fetch_sub(version->bytes, std::memory_order_relaxed);
                                                    delete version;
                                                } };
            return { .data = std::shared_ptr<const mesh_data>{ owner, &block->data }, .block = block };
        }

        // Moves a version from the current counter to the retained one. Called under the exclusive lock while the
        // cache still holds the version, so its deleter can't have run yet.
        void supersede(mesh_version* block) noexcept
        {
            if (block == nullptr)
            {
                return;
            }
            block->counters->retained.fetch_add(block->bytes, std::memory_order_relaxed);
            block->counters->current.fetch_sub(block->bytes, std::memory_order_relaxed);
            block->retained.store(true, std::memory_order_release);
        }

        template<typename T>
        struct slot
        {
            std::uint32_t generation = 1; // handles use 0 as invalid
            std::atomic<std::uint32_t> references{ 0 };
            std::uint64_t version_counter = 0;
            versioned<T> current;
            mesh_version* block = nullptr; // meshes only: the current version's control block
            bool alive = false;
        };

        template<typename T>
        struct slot_table
        {
            std::deque<slot<T>> slots; // a deque, so slots never move (they hold atomics)
            std::vector<std::uint32_t> free_list;
            std::atomic<std::uint32_t> live{ 0 };

            template<typename Handle>
            slot<T>* find(const Handle handle) noexcept
            {
                if (!handle.valid() || handle.index >= slots.size())
                {
                    return nullptr;
                }
                slot<T>& candidate = slots[handle.index];
                return candidate.alive && candidate.generation == handle.generation ? &candidate : nullptr;
            }

            // Under the exclusive lock: a live slot with one reference and version 1
            std::uint32_t allocate(versioned<T> first)
            {
                std::uint32_t index;
                if (!free_list.empty())
                {
                    index = free_list.back();
                    free_list.pop_back();
                }
                else
                {
                    index = static_cast<std::uint32_t>(slots.size());
                    slots.emplace_back();
                }
                slot<T>& allocated = slots[index];
                allocated.alive = true;
                allocated.references.store(1, std::memory_order_relaxed);
                allocated.version_counter = first.version;
                allocated.current = std::move(first);
                live.fetch_add(1, std::memory_order_relaxed);
                return index;
            }

            // Under the exclusive lock: drops the cache's version and bumps the generation, so handles go stale
            void free(const std::uint32_t index) noexcept
            {
                slot<T>& freed = slots[index];
                freed.alive = false;
                freed.current = {};
                freed.block = nullptr;
                freed.version_counter = 0;
                freed.references.store(0, std::memory_order_relaxed);
                freed.generation = freed.generation == std::numeric_limits<std::uint32_t>::max() ?
                                       1 :
                                       freed.generation + 1;
                free_list.push_back(index);
                live.fetch_sub(1, std::memory_order_relaxed);
            }
        };

        std::array<texture_handle, 5> textures_of(const material_description& description) noexcept
        {
            return { description.base_color_texture,
                     description.metallic_roughness_texture,
                     description.normal_texture,
                     description.occlusion_texture,
                     description.emissive_texture };
        }

        void report_stale(const char* const operation) noexcept
        {
            try
            {
                AVIS_CHECK(logger_name, false, std::format("asset_cache::{}: stale or null asset handle", operation));
            }
            catch (...)
            {
                AVIS_CHECK(logger_name, false, "asset_cache: stale or null asset handle");
            }
        }

        avis::error stale_error(const char* const operation)
        {
            return avis::make_error(
                generic_error_kind::logic_error, std::format("asset_cache::{}: stale or null asset handle", operation));
        }
    } // namespace

    struct asset_cache::state
    {
        mutable std::shared_mutex mutex;
        slot_table<mesh_data> meshes;
        slot_table<material_description> materials;
        slot_table<texture_data> textures;
        std::shared_ptr<geometry_counters> counters = std::make_shared<geometry_counters>();

        slot_table<mesh_data>& table(mesh_handle) noexcept
        {
            return meshes;
        }
        slot_table<material_description>& table(material_handle) noexcept
        {
            return materials;
        }
        slot_table<texture_data>& table(texture_handle) noexcept
        {
            return textures;
        }

        // Under the exclusive lock: frees a slot whose count reached 0, cascading to a material's textures
        void free(const mesh_handle mesh) noexcept
        {
            supersede(meshes.slots[mesh.index].block);
            meshes.free(mesh.index);
        }

        void free(const material_handle material) noexcept
        {
            const std::array<texture_handle, 5> textures_used =
                textures_of(*materials.slots[material.index].current.data);
            materials.free(material.index);
            for (const texture_handle texture : textures_used)
            {
                release_locked(texture);
            }
        }

        void free(const texture_handle texture) noexcept
        {
            textures.free(texture.index);
        }

        // Under the exclusive lock: one release, freeing the slot when it was the last reference
        template<typename Handle>
        void release_locked(const Handle handle) noexcept
        {
            auto* found = table(handle).find(handle);
            if (found != nullptr && found->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                free(handle);
            }
        }

        // Under the exclusive lock: every valid texture is alive
        bool textures_alive(const material_description& description) noexcept
        {
            for (const texture_handle texture : textures_of(description))
            {
                if (texture.valid() && textures.find(texture) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        void retain_textures(const material_description& description) noexcept
        {
            for (const texture_handle texture : textures_of(description))
            {
                if (texture.valid())
                {
                    textures.find(texture)->references.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // A copy of the current version, or an empty one for a stale handle
        template<typename Handle>
        auto current(const Handle handle)
        {
            auto& found_table = table(handle);
            const std::shared_lock lock{ mutex };
            const auto* found = found_table.find(handle);
            using versioned_type = std::remove_cvref_t<decltype(found->current)>;
            return found != nullptr ? found->current : versioned_type{};
        }

        template<typename Handle>
        bool retain(const Handle handle) noexcept
        {
            const std::shared_lock lock{ mutex };
            auto* found = table(handle).find(handle);
            if (found == nullptr)
            {
                return false;
            }
            found->references.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        template<typename Handle>
        bool release(const Handle handle) noexcept
        {
            {
                const std::shared_lock lock{ mutex };
                auto* found = table(handle).find(handle);
                if (found == nullptr)
                {
                    return false;
                }
                // Only the holder of the last reference goes on to the exclusive lock; nobody may retain through a
                // reference they don't hold, so the count can't rise again from 0
                if (found->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
                {
                    return true;
                }
            }

            const std::unique_lock lock{ mutex };
            auto* found = table(handle).find(handle);
            if (found != nullptr && found->references.load(std::memory_order_acquire) == 0)
            {
                free(handle);
            }
            return true;
        }
    };

    asset_cache::asset_cache(io::io_context& io) : io_{ &io }, state_{ std::make_unique<state>() } {}

    asset_cache::asset_cache() : state_{ std::make_unique<state>() } {}

    asset_cache::~asset_cache()
    {
        const asset_statistics remaining = statistics();
        const std::uint32_t live = remaining.meshes + remaining.materials + remaining.textures;
        if (live != 0)
        {
            try
            {
                AVIS_CHECK(
                    logger_name,
                    false,
                    std::format(
                        "asset_cache destroyed with live assets: {} meshes, {} materials, {} textures still referenced",
                        remaining.meshes,
                        remaining.materials,
                        remaining.textures));
            }
            catch (...)
            {
                AVIS_CHECK(logger_name, false, "asset_cache destroyed with live assets");
            }
        }
    }

    result<mesh_handle> asset_cache::create_mesh(mesh_data data)
    {
        if (result<void> validated = detail::validate_and_bound(data); !validated)
        {
            return std::unexpected{ std::move(validated.error()) };
        }

        new_mesh_version version = make_mesh_version(std::move(data), state_->counters);
        const std::unique_lock lock{ state_->mutex };
        const std::uint32_t index = state_->meshes.allocate({ .version = 1, .data = std::move(version.data) });
        state_->meshes.slots[index].block = version.block;
        return mesh_handle{ .index = index, .generation = state_->meshes.slots[index].generation };
    }

    result<void> asset_cache::update_mesh(const mesh_handle mesh, mesh_data data)
    {
        if (result<void> validated = detail::validate_and_bound(data); !validated)
        {
            return validated;
        }

        new_mesh_version version = make_mesh_version(std::move(data), state_->counters);
        std::shared_ptr<const mesh_data> previous; // dropped after the lock, where its deleter may run
        {
            const std::unique_lock lock{ state_->mutex };
            slot<mesh_data>* found = state_->meshes.find(mesh);
            if (found == nullptr)
            {
                // The new version is dropped with `version`, which takes its bytes off again
                return std::unexpected{ stale_error("update_mesh") };
            }
            supersede(found->block);
            previous = std::move(found->current.data);
            found->current = { .version = ++found->version_counter, .data = std::move(version.data) };
            found->block = version.block;
        }
        return {};
    }

    result<material_handle> asset_cache::create_material(material_description description)
    {
        auto stored = std::make_shared<const material_description>(std::move(description));
        const std::unique_lock lock{ state_->mutex };
        if (!state_->textures_alive(*stored))
        {
            return std::unexpected{ make_error(
                error_kind::invalid_material,
                std::format("material '{}' uses a texture that isn't alive", stored->debug_name)) };
        }
        state_->retain_textures(*stored);
        const std::uint32_t index = state_->materials.allocate({ .version = 1, .data = std::move(stored) });
        return material_handle{ .index = index, .generation = state_->materials.slots[index].generation };
    }

    result<void> asset_cache::update_material(const material_handle material, material_description description)
    {
        auto stored = std::make_shared<const material_description>(std::move(description));
        std::shared_ptr<const material_description> previous;
        {
            const std::unique_lock lock{ state_->mutex };
            slot<material_description>* found = state_->materials.find(material);
            if (found == nullptr)
            {
                return std::unexpected{ stale_error("update_material") };
            }
            if (!state_->textures_alive(*stored))
            {
                return std::unexpected{ make_error(
                    error_kind::invalid_material,
                    std::format("material '{}' uses a texture that isn't alive", stored->debug_name)) };
            }

            // Retain the new textures before releasing the old ones, so one used by both stays alive
            state_->retain_textures(*stored);
            previous = std::move(found->current.data);
            found->current = { .version = ++found->version_counter, .data = std::move(stored) };
            for (const texture_handle texture : textures_of(*previous))
            {
                if (texture.valid())
                {
                    state_->release_locked(texture);
                }
            }
        }
        return {};
    }

    result<texture_handle> asset_cache::create_texture(texture_data data)
    {
        auto stored = std::make_shared<const texture_data>(std::move(data));
        const std::unique_lock lock{ state_->mutex };
        const std::uint32_t index = state_->textures.allocate({ .version = 1, .data = std::move(stored) });
        return texture_handle{ .index = index, .generation = state_->textures.slots[index].generation };
    }

    versioned_mesh asset_cache::current(const mesh_handle mesh) const
    {
        versioned_mesh found = state_->current(mesh);
        if (found.data == nullptr)
        {
            report_stale("current");
        }
        return found;
    }

    versioned_material asset_cache::current(const material_handle material) const
    {
        versioned_material found = state_->current(material);
        if (found.data == nullptr)
        {
            report_stale("current");
        }
        return found;
    }

    versioned_texture asset_cache::current(const texture_handle texture) const
    {
        versioned_texture found = state_->current(texture);
        if (found.data == nullptr)
        {
            report_stale("current");
        }
        return found;
    }

    void asset_cache::retain(const mesh_handle mesh) noexcept
    {
        if (!state_->retain(mesh))
        {
            report_stale("retain");
        }
    }

    void asset_cache::retain(const material_handle material) noexcept
    {
        if (!state_->retain(material))
        {
            report_stale("retain");
        }
    }

    void asset_cache::retain(const texture_handle texture) noexcept
    {
        if (!state_->retain(texture))
        {
            report_stale("retain");
        }
    }

    void asset_cache::release(const mesh_handle mesh) noexcept
    {
        if (!state_->release(mesh))
        {
            report_stale("release");
        }
    }

    void asset_cache::release(const material_handle material) noexcept
    {
        if (!state_->release(material))
        {
            report_stale("release");
        }
    }

    void asset_cache::release(const texture_handle texture) noexcept
    {
        if (!state_->release(texture))
        {
            report_stale("release");
        }
    }

    void asset_cache::release_all(const gltf_asset_set& set) noexcept
    {
        for (const mesh_handle mesh : set.meshes)
        {
            release(mesh);
        }
        for (const material_handle material : set.materials)
        {
            release(material);
        }
        for (const texture_handle texture : set.textures)
        {
            release(texture);
        }
    }

    asset_statistics asset_cache::statistics() const noexcept
    {
        return asset_statistics{
            .meshes = state_->meshes.live.load(std::memory_order_relaxed),
            .materials = state_->materials.live.load(std::memory_order_relaxed),
            .textures = state_->textures.live.load(std::memory_order_relaxed),
            .current_geometry_bytes = state_->counters->current.load(std::memory_order_relaxed),
            .retained_geometry_bytes = state_->counters->retained.load(std::memory_order_relaxed),
        };
    }

    io::io_context* asset_cache::io() const noexcept
    {
        return io_;
    }
} // namespace avis::scene
