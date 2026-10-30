#include "avis/scene/scene_graph.h"

namespace avis::scene
{
    bool node_handle::valid() const
    {
        return index != std::numeric_limits<std::uint32_t>::max();
    }

    void scene_graph::mark_dirty(node_handle handle) {}

    void scene_graph::update_transforms()
    {
        std::vector<node_handle> stack;
        for (auto root : roots_)
        {
            stack.push_back(root);

            while (!stack.empty())
            {
                auto current = stack.back();
                stack.pop_back();

                auto* node = get_node(current);
                if (!node)
                {
                    continue;
                }

                if (node->dirty)
                {
                    Eigen::Matrix4f local = node->local.to_matrix();

                    if (node->parent.valid())
                    {
                        auto* parent = get_node(node->parent);
                        node->world = parent->world * local;
                    }
                    else
                    {
                        node->world = local;
                    }

                    node->dirty = false;
                }

                auto child = node->first_child;
                while (child.valid())
                {
                    stack.push_back(child);
                    child = get_node(child)->next_sibling;
                }
            }
        }
    }

    scene_node* scene_graph::get_node(node_handle handle)
    {
        if (!alive(handle))
        {
            return nullptr;
        }

        return &nodes_[handle.index].node;
    }

    bool scene_graph::alive(node_handle handle) const
    {
        if (handle.index >= nodes_.size())
        {
            return false;
        }

        const auto& slot = nodes_[handle.index];
        return slot.alive && slot.generation == handle.generation;
    }
} // namespace avis::scene
