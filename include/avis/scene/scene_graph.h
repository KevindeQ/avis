#ifndef AVIS_SCENE_SCENE_GRAPH_H
#define AVIS_SCENE_SCENE_GRAPH_H

// The scene graph (docs/design/08-scene-management.md §4, §9.3): a forest of nodes owning local TRS transforms and
// cached world transforms, each carrying an entity as payload. Generic: it knows entities only as a payload. `scene`
// is its only user for entities (§2), but it can be used and tested on its own.
//
// Handles are generational: destroying a node bumps its slot's generation, so old handles fail alive(). Every call
// taking a node_handle checks it; a stale handle is a debug check (AVIS_CHECK on the "scene" logger), and in release
// builds setters do nothing and getters return the identity transform, entt::null or a null handle.
//
// Not thread-safe. The simulation thread owns it (§2).

#include "avis/error.h"
#include "avis/scene/types.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace avis::scene
{
    class scene_graph
    {
    public:
        scene_graph();
        scene_graph(const scene_graph&) = delete;
        scene_graph& operator=(const scene_graph&) = delete;
        scene_graph(scene_graph&&) noexcept;
        scene_graph& operator=(scene_graph&&) noexcept;
        ~scene_graph();

        // ---- structure ----

        // Links the node as the first child of `parent`, or as a root when `parent` is null. A dead parent is a debug
        // check and gives a null handle.
        node_handle create_node(entity payload, node_handle parent = {}, const transform& local = {});

        // Destroys the subtree, children first; appends each destroyed node's payload to `destroyed`, children before
        // their parent
        void destroy_node(node_handle node, std::vector<entity>& destroyed);

        // Moves `node` under `parent`, or makes it a root when `parent` is null, and marks it dirty. Fails with
        // invalid_parent for a dead parent and cycle for a parent inside the node's own subtree, leaving the tree
        // unchanged. With keep_world, the local transform becomes the closest TRS to world(parent)⁻¹ · world(node),
        // which is exact only when the parent's world scale is uniform (§4); otherwise a warning is logged.
        result<void> reparent(node_handle node, node_handle parent, bool keep_world);

        // Destroys every node. Handles from before stay stale.
        void clear() noexcept;

        bool alive(node_handle node) const noexcept;
        entity payload(node_handle node) const noexcept;
        node_handle parent(node_handle node) const noexcept;
        std::span<const node_handle> roots() const noexcept; // in no particular order

        // Depth-first, parent before children. `visit` returns false to skip a node's children (a visit returning
        // void visits everything). The graph must not change during the walk.
        template<std::invocable<node_handle> Visit>
        void for_each_in_subtree(node_handle node, Visit&& visit) const;

        // ---- transforms ----

        // Stores the TRS and marks the node dirty; with discontinuity, also as having jumped (§4.1)
        void set_local(node_handle node, const transform& local, bool discontinuity = false) noexcept;
        const transform& local(node_handle node) const noexcept;

        // As of the last update_transforms; a debug check when the node or an ancestor is dirty
        const Eigen::Affine3f& world(node_handle node) const noexcept;

        // Incremental: only dirty nodes and their subtrees, skipping subtrees with nothing dirty. Calls jumped(node)
        // for every node that jumped since the last call, itself or through an ancestor (§4.1), after its world is
        // updated. Clears the dirty and jumped bits, and touches every node at most once. May run more than once per
        // tick. `jumped` must not change the graph.
        template<std::invocable<node_handle> Jumped>
        void update_transforms(Jumped&& jumped);

        // The same, ignoring jumps; it still clears them
        void update_transforms();

        std::size_t size() const noexcept; // live nodes

        // Nodes the last update_transforms visited, for statistics and tests
        std::size_t last_update_visits() const noexcept;

    private:
        static constexpr std::uint32_t npos = std::numeric_limits<std::uint32_t>::max();

        struct node_data
        {
            std::uint32_t parent = npos;
            std::uint32_t first_child = npos;
            std::uint32_t next_sibling = npos;
            std::uint32_t previous_sibling = npos;
            std::uint32_t root_position = npos; // index in roots_ while the node is a root

            transform local;
            Eigen::Affine3f world = Eigen::Affine3f::Identity();
            entity payload{};

            std::uint32_t generation = 0;
            bool alive = false;
            bool dirty = false; // local changed since the last update
            bool jumped = false; // a discontinuity since the last update
            bool pending = false; // this node or a descendant is dirty
            bool queued = false; // a root waiting in pending_roots_
        };

        // One step of update_transforms' walk
        struct update_entry
        {
            Eigen::Affine3f parent_world;
            std::uint32_t index;
            bool ancestor_dirty;
            bool ancestor_jumped;
        };

        // Checks a handle (AVIS_CHECK) and returns whether it's alive
        bool check_alive(node_handle node, const char* operation) const noexcept;

        node_handle handle_of(std::uint32_t index) const noexcept;

        // Marks the node and its ancestors pending, up to the first one already pending, and queues a newly pending
        // root for the next update
        void mark_pending(std::uint32_t index);

        void link(std::uint32_t index, std::uint32_t parent);
        void unlink(std::uint32_t index) noexcept;

        // The world transform from the locals up the chain, whatever the dirty bits say
        Eigen::Affine3f compute_world(std::uint32_t index) const noexcept;

        std::vector<node_data> nodes_;
        std::vector<std::uint32_t> free_list_;
        std::vector<node_handle> roots_;
        std::vector<node_handle> pending_roots_; // roots with something dirty below, in the order they became so
        std::vector<update_entry> update_stack_; // members, so a steady state doesn't allocate
        std::vector<std::uint32_t> subtree_scratch_;
        std::size_t size_ = 0;
        std::size_t last_update_visits_ = 0;
    };

    template<std::invocable<node_handle> Visit>
    void scene_graph::for_each_in_subtree(const node_handle node, Visit&& visit) const
    {
        if (!check_alive(node, "for_each_in_subtree"))
        {
            return;
        }

        // Pre-order through the links, so the walk needs no stack
        const std::uint32_t start = node.index;
        std::uint32_t current = start;
        while (true)
        {
            bool descend = true;
            if constexpr (std::is_void_v<std::invoke_result_t<Visit&, node_handle>>)
            {
                visit(handle_of(current));
            }
            else
            {
                descend = static_cast<bool>(visit(handle_of(current)));
            }

            if (descend && nodes_[current].first_child != npos)
            {
                current = nodes_[current].first_child;
                continue;
            }

            // Up until a node with a next sibling, without leaving the subtree
            while (current != start && nodes_[current].next_sibling == npos)
            {
                current = nodes_[current].parent;
            }
            if (current == start)
            {
                return;
            }
            current = nodes_[current].next_sibling;
        }
    }

    template<std::invocable<node_handle> Jumped>
    void scene_graph::update_transforms(Jumped&& jumped)
    {
        last_update_visits_ = 0;

        // pending_roots_ may hold roots destroyed or reparented since they were queued
        for (const node_handle root : pending_roots_)
        {
            if (root.index >= nodes_.size())
            {
                continue;
            }
            node_data& root_data = nodes_[root.index];
            if (!root_data.alive || root_data.generation != root.generation || !root_data.queued)
            {
                continue; // destroyed since, or a duplicate
            }
            root_data.queued = false;
            if (root_data.parent != npos || !root_data.pending)
            {
                continue; // reparented under another node since, which queued that node's root instead
            }

            update_stack_.push_back(update_entry{
                .parent_world = Eigen::Affine3f::Identity(),
                .index = root.index,
                .ancestor_dirty = false,
                .ancestor_jumped = false,
            });
            while (!update_stack_.empty())
            {
                const update_entry entry = update_stack_.back();
                update_stack_.pop_back();
                ++last_update_visits_;

                node_data& data = nodes_[entry.index];
                const bool dirty = entry.ancestor_dirty || data.dirty;
                const bool has_jumped = entry.ancestor_jumped || data.jumped;
                if (dirty)
                {
                    data.world = entry.parent_world * data.local.to_affine();
                }
                data.dirty = false;
                data.jumped = false;
                data.pending = false;

                if (has_jumped)
                {
                    jumped(handle_of(entry.index));
                }

                // Every child of a dirty node needs its world again; below a clean node, only pending subtrees
                for (std::uint32_t child = data.first_child; child != npos; child = nodes_[child].next_sibling)
                {
                    if (dirty || nodes_[child].pending)
                    {
                        update_stack_.push_back(update_entry{
                            .parent_world = data.world,
                            .index = child,
                            .ancestor_dirty = dirty,
                            .ancestor_jumped = has_jumped,
                        });
                    }
                }
            }
        }
        pending_roots_.clear();
    }
} // namespace avis::scene

#endif
