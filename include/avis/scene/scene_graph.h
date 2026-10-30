#ifndef AVIS_SCENE_SCENE_GRAPH_H
#define AVIS_SCENE_SCENE_GRAPH_H

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <cstdint>
#include <limits>
#include <vector>

namespace avis::scene
{
    struct transform
    {
    public:
        inline Eigen::Matrix4f to_matrix() const
        {
            Eigen::Affine3f transform = Eigen::Affine3f::Identity();
            transform.scale(scale);
            transform.rotate(rotation);
            transform.translate(position);

            return transform.matrix();
        }

        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
        Eigen::Vector3f scale = Eigen::Vector3f::Ones();
    };

    struct node_handle
    {
    public:
        auto operator<=>(const node_handle&) const = default;

        bool valid() const;

        std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;
    };

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
