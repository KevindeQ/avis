#ifndef AVIS_SCENE_SCENE_GRAPH_H
#define AVIS_SCENE_SCENE_GRAPH_H

#include "avis/scene/types.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <cstdint>
#include <limits>
#include <vector>

namespace avis::scene
{
    struct scene_node
    {
        node_handle parent;
        node_handle first_child;
        node_handle next_sibling;

        transform local;
        Eigen::Matrix4f world = Eigen::Matrix4f::Identity();

        bool dirty = true;
    };

    struct node_slot
    {
    public:
        scene_node node;

        bool alive = false;
        std::uint32_t generation = 0;
    };

    class scene_graph
    {
    public:
        void mark_dirty(node_handle handle);

        void update_transforms();

        scene_node* get_node(node_handle handle);

    private:
        bool alive(node_handle handle) const;

        std::vector<node_slot> nodes_;
        std::vector<std::uint32_t> free_list_;

        std::vector<node_handle> roots_;
    };
} // namespace avis::scene

#endif
