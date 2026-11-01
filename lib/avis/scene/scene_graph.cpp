#include "avis/scene/scene_graph.h"

#include "avis/detail/check.h"
#include "avis/log.h"
#include "avis/scene/error.h"

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

namespace avis::scene
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";

        // True when the linear part is a rotation times a uniform scale: Lᵀ · L = s² · I
        bool has_uniform_scale(const Eigen::Affine3f& affine) noexcept
        {
            const Eigen::Matrix3f gram = affine.linear().transpose() * affine.linear();
            const float scale_squared = gram.trace() / 3.0f;
            return (gram - scale_squared * Eigen::Matrix3f::Identity()).cwiseAbs().maxCoeff() <=
                   1e-5f * std::max(scale_squared, 1.0f);
        }

        const transform& identity_transform() noexcept
        {
            static const transform identity{};
            return identity;
        }

        const Eigen::Affine3f& identity_affine() noexcept
        {
            static const Eigen::Affine3f identity = Eigen::Affine3f::Identity();
            return identity;
        }
    } // namespace

    scene_graph::scene_graph() = default;
    scene_graph::scene_graph(scene_graph&&) noexcept = default;
    scene_graph& scene_graph::operator=(scene_graph&&) noexcept = default;
    scene_graph::~scene_graph() = default;

    node_handle scene_graph::create_node(const entity payload, const node_handle parent, const transform& local)
    {
        if (parent.valid() && !check_alive(parent, "create_node: parent"))
        {
            return {};
        }

        std::uint32_t index;
        if (!free_list_.empty())
        {
            index = free_list_.back();
            free_list_.pop_back();
        }
        else
        {
            index = static_cast<std::uint32_t>(nodes_.size());
            nodes_.emplace_back();
        }

        node_data& data = nodes_[index];
        data.local = local;
        data.world = Eigen::Affine3f::Identity();
        data.payload = payload;
        data.alive = true;
        data.dirty = true;
        ++size_;

        link(index, parent.valid() ? parent.index : npos);
        mark_pending(index);
        return handle_of(index);
    }

    void scene_graph::destroy_node(const node_handle node, std::vector<entity>& destroyed)
    {
        if (!check_alive(node, "destroy_node"))
        {
            return;
        }

        // Pre-order collection; its reverse lists every node after all of its descendants
        subtree_scratch_.clear();
        for_each_in_subtree(node, [this](const node_handle visited) { subtree_scratch_.push_back(visited.index); });

        unlink(node.index);
        for (const std::uint32_t index : std::views::reverse(subtree_scratch_))
        {
            node_data& data = nodes_[index];
            destroyed.push_back(data.payload);

            const std::uint32_t generation = data.generation + 1;
            data = node_data{};
            data.generation = generation;
            free_list_.push_back(index);
            --size_;
        }
        subtree_scratch_.clear();
    }

    result<void> scene_graph::reparent(const node_handle node, const node_handle parent, const bool keep_world)
    {
        if (!check_alive(node, "reparent"))
        {
            return std::unexpected{ make_error(error_kind::invalid_parent, "reparent: the node isn't alive") };
        }
        if (parent.valid() && !alive(parent))
        {
            return std::unexpected{ make_error(error_kind::invalid_parent, "reparent: the new parent isn't alive") };
        }
        if (parent.valid())
        {
            for (std::uint32_t ancestor = parent.index; ancestor != npos; ancestor = nodes_[ancestor].parent)
            {
                if (ancestor == node.index)
                {
                    return std::unexpected{ make_error(
                        error_kind::cycle, "reparent: the new parent is in the node's own subtree") };
                }
            }
        }

        if (keep_world)
        {
            const Eigen::Affine3f node_world = compute_world(node.index);
            if (parent.valid())
            {
                const Eigen::Affine3f parent_world = compute_world(parent.index);
                if (!has_uniform_scale(parent_world))
                {
                    log::get(logger_name)
                        ->warn(
                            "reparent: the new parent's world scale isn't uniform, so the node keeps the closest TRS "
                            "to its world transform, not the transform itself");
                }
                if (std::abs(parent_world.linear().determinant()) > 1e-12f)
                {
                    nodes_[node.index].local = transform::from_affine(parent_world.inverse() * node_world);
                }
                else
                {
                    log::get(logger_name)
                        ->warn("reparent: the new parent's world transform is singular; the local transform is kept");
                }
            }
            else
            {
                nodes_[node.index].local = transform::from_affine(node_world);
            }
        }

        unlink(node.index);
        link(node.index, parent.valid() ? parent.index : npos);
        nodes_[node.index].dirty = true;
        mark_pending(node.index);
        return {};
    }

    void scene_graph::clear() noexcept
    {
        for (std::uint32_t index = 0; index < nodes_.size(); ++index)
        {
            node_data& data = nodes_[index];
            if (data.alive)
            {
                const std::uint32_t generation = data.generation + 1;
                data = node_data{};
                data.generation = generation;
                free_list_.push_back(index);
            }
        }
        roots_.clear();
        pending_roots_.clear();
        update_stack_.clear();
        size_ = 0;
    }

    bool scene_graph::alive(const node_handle node) const noexcept
    {
        return node.index < nodes_.size() && nodes_[node.index].alive &&
               nodes_[node.index].generation == node.generation;
    }

    entity scene_graph::payload(const node_handle node) const noexcept
    {
        if (!check_alive(node, "payload"))
        {
            return entt::null;
        }
        return nodes_[node.index].payload;
    }

    node_handle scene_graph::parent(const node_handle node) const noexcept
    {
        if (!check_alive(node, "parent"))
        {
            return {};
        }
        const std::uint32_t parent_index = nodes_[node.index].parent;
        return parent_index == npos ? node_handle{} : handle_of(parent_index);
    }

    std::span<const node_handle> scene_graph::roots() const noexcept
    {
        return roots_;
    }

    void scene_graph::set_local(const node_handle node, const transform& local, const bool discontinuity) noexcept
    {
        if (!check_alive(node, "set_local"))
        {
            return;
        }
        node_data& data = nodes_[node.index];
        data.local = local;
        data.dirty = true;
        data.jumped = data.jumped || discontinuity;
        mark_pending(node.index);
    }

    const transform& scene_graph::local(const node_handle node) const noexcept
    {
        if (!check_alive(node, "local"))
        {
            return identity_transform();
        }
        return nodes_[node.index].local;
    }

    const Eigen::Affine3f& scene_graph::world(const node_handle node) const noexcept
    {
        if (!check_alive(node, "world"))
        {
            return identity_affine();
        }
#ifndef NDEBUG
        bool stale = false;
        for (std::uint32_t ancestor = node.index; ancestor != npos; ancestor = nodes_[ancestor].parent)
        {
            stale = stale || nodes_[ancestor].dirty;
        }
        AVIS_CHECK(
            logger_name, !stale, "world: the node or an ancestor is dirty; call update_transforms before reading it");
#endif
        return nodes_[node.index].world;
    }

    void scene_graph::update_transforms()
    {
        update_transforms([](node_handle) {});
    }

    std::size_t scene_graph::size() const noexcept
    {
        return size_;
    }

    std::size_t scene_graph::last_update_visits() const noexcept
    {
        return last_update_visits_;
    }

    bool scene_graph::check_alive(const node_handle node, const char* const operation) const noexcept
    {
        if (alive(node))
        {
            return true;
        }
        try
        {
            return AVIS_CHECK(logger_name, false, std::format("{}: stale or null node handle", operation));
        }
        catch (...)
        {
            return AVIS_CHECK(logger_name, false, "scene_graph: stale or null node handle");
        }
    }

    node_handle scene_graph::handle_of(const std::uint32_t index) const noexcept
    {
        return node_handle{ .index = index, .generation = nodes_[index].generation };
    }

    void scene_graph::mark_pending(const std::uint32_t index)
    {
        nodes_[index].pending = true;
        std::uint32_t current = index;
        while (true)
        {
            const std::uint32_t parent_index = nodes_[current].parent;
            if (parent_index == npos)
            {
                node_data& root = nodes_[current];
                if (!root.queued)
                {
                    root.queued = true;
                    pending_roots_.push_back(handle_of(current));
                }
                return;
            }
            if (nodes_[parent_index].pending)
            {
                return;
            }
            nodes_[parent_index].pending = true;
            current = parent_index;
        }
    }

    void scene_graph::link(const std::uint32_t index, const std::uint32_t parent_index)
    {
        node_data& data = nodes_[index];
        data.parent = parent_index;
        data.previous_sibling = npos;
        if (parent_index == npos)
        {
            data.next_sibling = npos;
            data.root_position = static_cast<std::uint32_t>(roots_.size());
            roots_.push_back(handle_of(index));
            return;
        }

        node_data& parent_data = nodes_[parent_index];
        data.next_sibling = parent_data.first_child;
        if (parent_data.first_child != npos)
        {
            nodes_[parent_data.first_child].previous_sibling = index;
        }
        parent_data.first_child = index;
    }

    void scene_graph::unlink(const std::uint32_t index) noexcept
    {
        node_data& data = nodes_[index];
        if (data.parent == npos)
        {
            // Swap-remove from roots_
            const std::uint32_t position = data.root_position;
            const node_handle last = roots_.back();
            roots_[position] = last;
            nodes_[last.index].root_position = position;
            roots_.pop_back();
            data.root_position = npos;
            return;
        }

        if (data.previous_sibling != npos)
        {
            nodes_[data.previous_sibling].next_sibling = data.next_sibling;
        }
        else
        {
            nodes_[data.parent].first_child = data.next_sibling;
        }
        if (data.next_sibling != npos)
        {
            nodes_[data.next_sibling].previous_sibling = data.previous_sibling;
        }
        data.parent = npos;
        data.next_sibling = npos;
        data.previous_sibling = npos;
    }

    Eigen::Affine3f scene_graph::compute_world(const std::uint32_t index) const noexcept
    {
        Eigen::Affine3f world = nodes_[index].local.to_affine();
        for (std::uint32_t ancestor = nodes_[index].parent; ancestor != npos; ancestor = nodes_[ancestor].parent)
        {
            world = nodes_[ancestor].local.to_affine() * world;
        }
        return world;
    }
} // namespace avis::scene
