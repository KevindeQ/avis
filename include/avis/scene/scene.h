#ifndef AVIS_SCENE_SCENE_H
#define AVIS_SCENE_SCENE_H

// The scene (docs/design/08-scene-management.md §2, §9.5): one EnTT registry and one scene graph, kept consistent.
// It creates and destroys entities with their nodes, keeps the id index, owns identity, node_reference and
// previous_world, ties the lifetime of mesh and material handles to their components, and records tree changes for
// the GUI. Only the simulation thread uses it: the thread that constructed it, checked in debug builds.
//
// Tasks 10.9–10.11 add the entity, hierarchy, transform, tick and snapshot members; viewing aids and statistics follow
// in task 10.12.

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/scene_graph.h"
#include "avis/scene/snapshot.h"
#include "avis/scene/types.h"

#include <Eigen/Geometry>
#include <entt/entity/registry.hpp>
#include <entt/entity/storage.hpp>

#include <array>
#include <chrono>
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

        // ---- hierarchy and transforms ----

        // Moves the entity under `parent`, or makes it a root when `parent` is null. With keep_world, the local
        // transform changes so the world transform stays (exact only under a parent with uniform scale, §4). Fails
        // with invalid_parent when the entity or the parent isn't alive or has no node, and cycle when the parent is
        // in the entity's own subtree; the tree is then unchanged. Records a `moved` change.
        result<void> set_parent(entity child, entity parent, bool keep_world);

        // Stores the local TRS; the world transform follows at the next update_world_transforms. A discontinuity
        // marks the node and its subtree as having jumped, so they don't interpolate (§4.1). Debug builds assert unit
        // scale on entities with driver_entity (§4.3).
        void set_local(entity entity, const transform& local, bool discontinuity = false);

        // An entity without a node is a debug check and gives the identity. world() needs an update after the last
        // change (a debug check in the scene graph).
        const transform& local(entity entity) const;
        const Eigen::Affine3f& world(entity entity) const;
        Eigen::Affine3f relative_transform(entity from, entity to) const; // world(from)⁻¹ · world(to)

        // ---- per tick (§9.6) ----

        // Propagates the changed transforms. Each node that jumped, itself or through an ancestor, gets
        // previous_world = world, emplaced when missing, so it doesn't interpolate across the jump.
        void update_world_transforms();

        // Fills `target` with the instances and lights in stable id order, the environment, the scene version and the
        // current version of every referenced asset (§6.3). Leaves the step fields and publish_sequence to the
        // caller and the buffer. An entity without previous_world (created this tick) gets world_previous =
        // world_current and instance_flags::history_invalid. A steady state allocates nothing once `target` has
        // grown. Debug builds assert that every transform change was propagated (§9.6).
        void make_snapshot(snapshot& target);

        // previous_world = world for every entity with a node, emplaced when missing: an entity created this tick
        // interpolates from its second snapshot onwards. Records the tick's timings.
        //
        // Debug builds check the tick order: end_tick twice with nothing in between, and end_tick or make_snapshot
        // after a transform change that update_world_transforms hasn't propagated. A republish (make_snapshot
        // without end_tick) is allowed.
        void end_tick();

        // ---- environment ----

        // Copied into every snapshot. A presentation setting: the Visualizer sets it from the scenario (D-42).
        void set_environment(const environment_settings& environment);
        const environment_settings& environment() const noexcept;

        // ---- UI ----

        // Since the last call (§7)
        std::vector<tree_change> take_tree_changes();

        // Increments on each structural change: create, destroy, set_parent, clear
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

        // The entity's node; an invalid handle, after a debug check, when it's dead or has no node
        node_handle node_of(entity entity, const char* operation) const noexcept;

        // previous_world = world, emplaced when missing
        void store_previous_world(entity entity, const Eigen::Affine3f& world);

        // ---- tick order (§9.6) ----

        // Something that can change world transforms: create_entity, set_parent, set_local
        void note_transform_change() noexcept;

        // For make_snapshot: a debug check that every transform change was propagated
        void check_snapshot_order(const char* operation) const noexcept;

        // ---- snapshots ----

        // draw_order_ sorted by identity::id, rebuilt after a structural change or a mesh or material add or remove
        void update_draw_order();
        void collect_assets(snapshot& target);
        void add_instances(snapshot& target);
        void add_lights(snapshot& target);

        // ---- signal handlers ----
        template<typename Component>
        void on_owned_write(entt::registry& registry, entity entity); // debug builds
        void on_identity_construct(entt::registry& registry, entity entity);
        void on_identity_destroy(entt::registry& registry, entity entity);
        void on_represents_construct(entt::registry& registry, entity entity);
        void on_represents_destroy(entt::registry& registry, entity entity);
        void on_draw_set_change(entt::registry& registry, entity entity); // mesh or material added or removed
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

        // Tick order and timings
        bool transforms_pending_ = false; // a change since the last update_world_transforms
        bool tick_active_ = true; // a change or an update since the last end_tick
        std::chrono::nanoseconds tick_transform_time_{ 0 }; // update_world_transforms and end_tick, this tick
        std::chrono::nanoseconds last_transform_time_{ 0 }; // the same, for the last finished tick

        // Snapshots: the order cache, scratch handle lists kept for their capacity, and what the last one held
        environment_settings environment_;
        std::vector<entity> draw_order_; // view<node_reference, mesh, material>, sorted by id
        std::uint64_t draw_order_version_ = 0; // scene_version_ when draw_order_ was built
        bool draw_order_dirty_ = true;
        std::vector<mesh_handle> mesh_scratch_;
        std::vector<material_handle> material_scratch_;
        std::vector<texture_handle> texture_scratch_;
        std::chrono::nanoseconds last_snapshot_time_{ 0 };
        std::size_t last_snapshot_instances_ = 0;
        std::size_t last_snapshot_lights_ = 0;

        int owner_writes_ = 0; // > 0 while scene writes the components it owns
        bool destroying_ = false; // while scene destroys entities: no `renamed` change for a lost `represents`
    };
} // namespace avis::scene

#endif
