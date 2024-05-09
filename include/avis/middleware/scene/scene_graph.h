#ifndef MIDDLEWARE_SCENE_SCENE_GRAPH_H
#define MIDDLEWARE_SCENE_SCENE_GRAPH_H

#include "avis/core/common.h"

#include "Eigen/Dense"

namespace scene
{
    class scene_node
    {
    public:
    private:
    };

    class group_node : public scene_node
    {
    public:
    private:
        std::vector<scene_node*> children;
    };

    class transform_node : public scene_node
    {
    public:
    private:
        Eigen::Vector3f position;
        Eigen::Quaternionf rotation;
        Eigen::Vector3f scale;
    };

    class geometry_node : public scene_node
    {
    public:
    private:
    };

    class scene_graph
    {
    public:
    private:
        std::unique_ptr<scene_node*> root_node;
    };
} // namespace scene

#endif
