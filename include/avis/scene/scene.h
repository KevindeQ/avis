#ifndef AVIS_SCENE_SCENE_H
#define AVIS_SCENE_SCENE_H

// The scene (docs/design/08-scene-management.md §2, §9.5): one EnTT registry and one scene graph, kept consistent.
// It creates and destroys entities with their nodes, keeps the id index, owns identity, node_reference and
// previous_world, ties the lifetime of mesh and material handles to their components, and records tree changes for
// the GUI. Only the simulation thread uses it: the thread that constructed it, checked in debug builds.
//
// Task 10.9 adds the entity members; hierarchy, transforms, the tick sequence, snapshots, viewing aids and statistics
// follow in tasks 10.10–10.12.

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/scene_graph.h"
#include "avis/scene/types.h"

#include <entt/entity/registry.hpp>
#include <entt/entity/storage.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace avis::scene
{
    struct entity_description
    {
        object_id id = 0; // 0 = allocate a helper id
        std::string name;
        entity_category category = entity_category::helper;
        entity parent = entt::null; // null = a root
        transform local;
        bool spatial = true; // false: no node (scenario settings, environment, §2)
    };

    struct destroyed_entity
    {
        entity handle = entt::null; // no longer valid
        object_id id = 0;
    };

    enum class tree_change_kind : std::uint8_t
    {
        added,
        removed, // one per destroyed subtree, for its root: the GUI drops the subtree
        moved,
        renamed // also when an entity gains or loses `represents`, which changes `hidden`
    };

    // For the GUI's scene tree (§7)
    struct tree_change
    {
        tree_change_kind kind = tree_change_kind::added;
        object_id id = 0;
        object_id parent = 0; // 0 = root
        std::string name;
        entity_category category = entity_category::helper;
        bool hidden = false; // helpers with `represents`
    };

    class scene
    {
    public:
        // The cache must outlive the scene
        explicit scene(asset_cache& assets);
        scene(const scene&) = delete;
        scene& operator=(const scene&) = delete;
        scene(scene&&) = delete;
        scene& operator=(scene&&) = delete;
        ~scene(); // destroys every entity, releasing their assets

        // ---- entities ----

        // Fails with duplicate_id when the id is taken, and invalid_parent when the parent isn't alive, has no node,
        // or the entity isn't spatial. Spatial entities get a node with `local`, but no previous_world until the
        // tick ends (§6.3).
        result<entity> create_entity(const entity_description& description);

        // The whole subtree, root included, children before parents (§2). A non-spatial entity destroys only itself.
        std::vector<destroyed_entity> destroy_entity(entity entity);

        // Every entity, helpers included; the start of a run (§3.1). Resets the helper ids.
        void clear();

        bool alive(entity entity) const noexcept;
        std::optional<entity> find(object_id id) const noexcept;

        // Follows `represents` to the owner. The entity itself when it has none or its owner is dead.
        entity selection_target(entity entity) const noexcept;

        // The next unused id in the helper range (types.h)
        object_id allocate_helper_id() noexcept;

        // Components other than the ones scene owns (§9.2): identity, node_reference and previous_world are written
        // only by scene, and debug builds assert on any other write. Emplacing, replacing or patching mesh and
        // material retains their handles and releases the previous ones; removing them releases them.
        entt::registry& registry() noexcept;
        const entt::registry& registry() const noexcept;

        asset_cache& assets() noexcept;

        // ---- UI ----

        // Since the last call (§7)
        std::vector<tree_change> take_tree_changes();

        // Increments on each structural change: create, destroy, clear
        std::uint64_t scene_version() const noexcept;

    private:
        // Held while scene writes identity, node_reference or previous_world (scene.cpp)
        class owner_scope;

        // Counters for scene_statistics (§8.5), kept by the identity, label and trail signals
        struct counters
        {
            std::size_t entities = 0;
            std::array<std::size_t, entity_category_count> per_category{};
            std::size_t labels = 0;
            std::size_t trails = 0;
        };

        bool check_thread(const char* operation) const noexcept;
        bool check_alive(entity entity, const char* operation) const noexcept;

        object_id id_of(entity entity) const noexcept; // 0 without identity
        object_id parent_id_of(entity entity) const noexcept; // 0 for a root or a non-spatial entity
        void record_change(tree_change_kind kind, entity entity);

        // Destroys every entity under the owner scope; records nothing
        void destroy_all();

        // ---- signal handlers ----
        template<typename Component>
        void on_owned_write(entt::registry& registry, entity entity); // debug builds
        void on_identity_construct(entt::registry& registry, entity entity);
        void on_identity_destroy(entt::registry& registry, entity entity);
        void on_represents_construct(entt::registry& registry, entity entity);
        void on_represents_destroy(entt::registry& registry, entity entity);
        void on_mesh_construct(entt::registry& registry, entity entity);
        void on_mesh_update(entt::registry& registry, entity entity);
        void on_mesh_destroy(entt::registry& registry, entity entity);
        void on_material_construct(entt::registry& registry, entity entity);
        void on_material_update(entt::registry& registry, entity entity);
        void on_material_destroy(entt::registry& registry, entity entity);
        void on_label_construct(entt::registry& registry, entity entity);
        void on_label_destroy(entt::registry& registry, entity entity);
        void on_trail_construct(entt::registry& registry, entity entity);
        void on_trail_destroy(entt::registry& registry, entity entity);

        asset_cache& assets_;
        std::thread::id owner_thread_;

        entt::registry registry_;
        scene_graph graph_;
        std::unordered_map<object_id, entity> ids_;
        object_id next_helper_id_;

        // The handles each entity's mesh and material retained. EnTT's on_update runs after the new value is stored,
        // so releasing the previous handles needs this copy.
        entt::storage<mesh_handle> mesh_shadow_;
        entt::storage<std::vector<material_handle>> material_shadow_;

        std::vector<tree_change> tree_changes_;
        std::uint64_t scene_version_ = 0;
        counters counters_;

        int owner_writes_ = 0; // > 0 while scene writes the components it owns
        bool destroying_ = false; // while scene destroys entities: no `renamed` change for a lost `represents`
    };
} // namespace avis::scene

#endif
