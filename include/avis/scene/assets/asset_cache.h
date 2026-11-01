#ifndef AVIS_SCENE_ASSETS_ASSET_CACHE_H
#define AVIS_SCENE_ASSETS_ASSET_CACHE_H

// The CPU-only asset cache (docs/design/08-scene-management.md §5.1, §5.2, §8.5, §9.4): versioned, immutable meshes,
// materials and textures behind reference-counted generational handles.
//
// Thread-safety: every member is thread-safe. A std::shared_mutex guards the slot tables: current() and retain() take
// it shared, create_*, update_* and the last release() exclusive. The byte counters are atomics, because the
// shared_ptr deleters that decrement them run wherever the last snapshot or upload lets go, including the render
// thread; they stay valid after the cache is destroyed.
//
// A stale handle is a debug check (AVIS_CHECK on the "scene" logger). In release builds it makes retain() and
// release() do nothing and current() return an empty versioned.

#include "avis/error.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/assets/texture_data.h"

#include <cstdint>
#include <memory>

namespace avis::io
{
    class io_context;
} // namespace avis::io

namespace avis::scene
{
    struct gltf_asset_set;

    template<typename T>
    struct versioned
    {
        std::uint64_t version = 0; // from 1, never repeated for a handle (§5.2); 0 = none
        std::shared_ptr<const T> data;
    };

    using versioned_mesh = versioned<mesh_data>;
    using versioned_material = versioned<material_description>;
    using versioned_texture = versioned<texture_data>;

    struct asset_statistics
    {
        std::uint32_t meshes = 0;
        std::uint32_t materials = 0;
        std::uint32_t textures = 0;
        std::uint64_t current_geometry_bytes = 0; // current mesh versions (§8.5)
        std::uint64_t retained_geometry_bytes = 0; // superseded versions still held by snapshots or uploads
    };

    class asset_cache
    {
    public:
        // Reads go through the IO layer (07); load_gltf needs it
        explicit asset_cache(io::io_context& io);

        // A cache for procedural assets only, without file loading
        asset_cache();

        // Asserts that every slot is free: only the cache's own references remain
        ~asset_cache();

        asset_cache(const asset_cache&) = delete;
        asset_cache& operator=(const asset_cache&) = delete;
        asset_cache(asset_cache&&) = delete;
        asset_cache& operator=(asset_cache&&) = delete;

        // Validates the data and computes its bounds (invalid_mesh otherwise), then stores an immutable copy as
        // version 1, with one reference. Never deduplicates.
        result<mesh_handle> create_mesh(mesh_data data);

        // Stores a new immutable copy as version + 1. The earlier version lives on while something holds it. A
        // failure keeps the current version. May change the topology, attributes and size.
        result<void> update_mesh(mesh_handle mesh, mesh_data data);

        // Each valid texture handle must be alive (invalid_material otherwise); the material retains its textures
        // and releases them when it's freed or updated
        result<material_handle> create_material(material_description description);
        result<void> update_material(material_handle material, material_description description);

        result<texture_handle> create_texture(texture_data data);

        versioned_mesh current(mesh_handle mesh) const;
        versioned_material current(material_handle material) const;
        versioned_texture current(texture_handle texture) const;

        // Reference counting: create_* returns one reference; components hold their own (§2). The last release
        // frees the slot (the handle becomes stale); the CPU data lives on while a snapshot holds it.
        void retain(mesh_handle mesh) noexcept;
        void retain(material_handle material) noexcept;
        void retain(texture_handle texture) noexcept;
        void release(mesh_handle mesh) noexcept;
        void release(material_handle material) noexcept; // a freed material releases its textures
        void release(texture_handle texture) noexcept;
        void release_all(const gltf_asset_set& set) noexcept;

        // The counts and the byte counters of §8.5
        asset_statistics statistics() const noexcept;

        // The cache's IO context, or nullptr for a cache without file loading
        io::io_context* io() const noexcept;

    private:
        struct state;

        io::io_context* io_ = nullptr;
        std::unique_ptr<state> state_;
    };
} // namespace avis::scene

#endif
