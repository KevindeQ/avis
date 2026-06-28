#include "avis/scene/semantic_scene_graph.h"

namespace avis::scene
{
    tree_node_handle::tree_node_handle(tree_node* node) : ptr_handle_{ static_cast<void*>(node) } {}

    bool tree_node_handle::operator==(const tree_node_handle& other)
    {
        return ptr_handle_ == other.ptr_handle_;
    }

    bool tree_node_handle::operator!=(const tree_node_handle& other)
    {
        return !(*this == other);
    }

    tree_node_handle semantic_scene_graph::add_root_node(H3Index center_location)
    {
        if (root_nodes_.contains(center_location))
        {
            throw std::runtime_error{ "A root node already exists for the given center location" };
        }

        auto entity = component_registry_.create();
        auto [it, inserted] = root_nodes_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(center_location),
            std::forward_as_tuple(component_registry_, entity));
        if (!inserted)
        {
            throw std::runtime_error{ "Failed to create new root node" };
        }

        return it->second.as_handle();
    }

    tree_node::tree_node(entt::registry& registry, entt::entity entity) : registry_{ registry }, entity_{ entity } {}

    void tree_node::add_child(std::unique_ptr<tree_node> child) {}

    void tree_node::remove_child(std::unique_ptr<tree_node> child) {}

    tree_node_handle tree_node::as_handle()
    {
        return tree_node_handle{ this };
    }
} // namespace avis::scene
