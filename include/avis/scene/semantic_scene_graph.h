#ifndef AVIS_SCENE_SEMANTIC_SCENE_GRAPH_H
#define AVIS_SCENE_SEMANTIC_SCENE_GRAPH_H

#include <entt/entt.hpp>
#include <h3api.h>

#include <memory>
#include <vector>
#include <unordered_map>

namespace avis::scene
{
    class tree_node;

    class tree_node_handle
    {
    public:
        tree_node_handle(tree_node* node);

        bool operator==(const tree_node_handle& other);
        bool operator!=(const tree_node_handle& other);

    private:
        void* ptr_handle_;
    };

    class tree_node
    {
    public:
        tree_node(entt::registry& registry, entt::entity entity);

        void add_child(std::unique_ptr<tree_node> child);
        void remove_child(std::unique_ptr<tree_node> child);

        tree_node_handle as_handle();

    protected:
        entt::registry& registry_;
        entt::entity entity_;

    private:
        std::vector<std::unique_ptr<tree_node>> children_;
    };

    class semantic_scene_graph
    {
    public:
        tree_node_handle add_root_node(H3Index center_location);

    private:
        std::unordered_map<H3Index, tree_node> root_nodes_;

        entt::registry component_registry_;
    };
} // namespace avis::scene

#endif
