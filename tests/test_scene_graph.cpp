// Scene graph: structure, reparenting, transform propagation, discontinuities (docs/tasks/10-scene-management.md §10.3)

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"

#include "avis/scene/error.h"
#include "avis/scene/scene_graph.h"

#include <catch2/catch_test_macros.hpp>

#include <entt/entity/entity.hpp>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <random>
#include <vector>

using namespace avis;

namespace
{
    scene::entity make_entity(const std::uint32_t value)
    {
        return static_cast<scene::entity>(value);
    }

    scene::transform random_transform(std::mt19937& random)
    {
        std::uniform_real_distribution<float> position{ -10.0f, 10.0f };
        std::uniform_real_distribution<float> scale{ 0.5f, 1.5f };
        std::normal_distribution<float> axis{ 0.0f, 1.0f };
        return scene::transform{
            .position = { position(random), position(random), position(random) },
            .rotation = Eigen::Quaternionf{ axis(random), axis(random), axis(random), axis(random) }.normalized(),
            .scale = { scale(random), scale(random), scale(random) },
        };
    }

    // World transform from the locals up the chain, without the graph's cache
    Eigen::Affine3f brute_force_world(const scene::scene_graph& graph, scene::node_handle node)
    {
        Eigen::Affine3f world = Eigen::Affine3f::Identity();
        for (; node.valid(); node = graph.parent(node))
        {
            world = graph.local(node).to_affine() * world;
        }
        return world;
    }

    std::vector<scene::node_handle> children_of(const scene::scene_graph& graph, const scene::node_handle node)
    {
        std::vector<scene::node_handle> children;
        graph.for_each_in_subtree(node, [&](const scene::node_handle visited) {
            if (visited != node)
            {
                children.push_back(visited);
                return false;
            }
            return true;
        });
        std::ranges::sort(children);
        return children;
    }

    // A random forest: each node's parent is an earlier node, or none for about 5% of them
    std::vector<scene::node_handle> build_random_tree(scene::scene_graph& graph, std::mt19937& random, const int count)
    {
        std::vector<scene::node_handle> nodes;
        nodes.reserve(static_cast<std::size_t>(count));
        std::uniform_real_distribution<float> chance{ 0.0f, 1.0f };
        for (int index = 0; index < count; ++index)
        {
            scene::node_handle parent;
            if (!nodes.empty() && chance(random) > 0.05f)
            {
                std::uniform_int_distribution<std::size_t> pick{ 0, nodes.size() - 1 };
                parent = nodes[pick(random)];
            }
            nodes.push_back(graph.create_node(make_entity(static_cast<std::uint32_t>(index)), parent,
                                              random_transform(random)));
        }
        return nodes;
    }
} // namespace

TEST_CASE("destroying a subtree returns its payloads children first", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle a = graph.create_node(make_entity(1));
    const scene::node_handle b = graph.create_node(make_entity(2), a);
    const scene::node_handle c = graph.create_node(make_entity(3), a);
    const scene::node_handle d = graph.create_node(make_entity(4), b);
    const scene::node_handle other = graph.create_node(make_entity(5));
    REQUIRE(graph.size() == 5);
    CHECK(graph.roots().size() == 2);
    CHECK(graph.parent(d) == b);
    CHECK(graph.payload(c) == make_entity(3));

    // for_each_in_subtree visits parents before children
    std::vector<scene::node_handle> order;
    graph.for_each_in_subtree(a, [&](const scene::node_handle node) { order.push_back(node); });
    REQUIRE(order.size() == 4);
    const auto position = [&](const scene::node_handle node) { return std::ranges::find(order, node) - order.begin(); };
    CHECK(position(a) == 0);
    CHECK(position(b) < position(d));

    std::vector<scene::entity> destroyed;
    graph.destroy_node(a, destroyed);
    REQUIRE(destroyed.size() == 4);
    const auto destroyed_position = [&](const std::uint32_t value) {
        return std::ranges::find(destroyed, make_entity(value)) - destroyed.begin();
    };
    CHECK(destroyed_position(4) < destroyed_position(2)); // d before its parent b
    CHECK(destroyed_position(2) < destroyed_position(1));
    CHECK(destroyed_position(3) < destroyed_position(1));
    CHECK(destroyed.back() == make_entity(1));

    for (const scene::node_handle node : { a, b, c, d })
    {
        CHECK_FALSE(graph.alive(node));
    }
    CHECK(graph.alive(other));
    CHECK(graph.size() == 1);
    REQUIRE(graph.roots().size() == 1);
    CHECK(graph.roots()[0] == other);

    // A freed slot is reused with a higher generation, so the old handle stays stale
    const scene::node_handle reused = graph.create_node(make_entity(6));
    const bool reuses_a_slot = reused.index == a.index || reused.index == b.index || reused.index == c.index ||
                               reused.index == d.index;
    CHECK(reuses_a_slot);
    CHECK(reused.generation > 0);
    CHECK(graph.alive(reused));
    for (const scene::node_handle node : { a, b, c, d })
    {
        CHECK_FALSE(graph.alive(node));
    }
}

TEST_CASE("destroying a middle node keeps its siblings linked", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle root = graph.create_node(make_entity(1));
    const scene::node_handle first = graph.create_node(make_entity(2), root);
    const scene::node_handle middle = graph.create_node(make_entity(3), root);
    const scene::node_handle last = graph.create_node(make_entity(4), root);

    std::vector<scene::entity> destroyed;
    graph.destroy_node(middle, destroyed);
    CHECK(destroyed == std::vector{ make_entity(3) });

    std::vector<scene::node_handle> expected{ first, last };
    std::ranges::sort(expected);
    CHECK(children_of(graph, root) == expected);
}

TEST_CASE("for_each_in_subtree skips the children of a node when visit returns false", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle root = graph.create_node(make_entity(1));
    const scene::node_handle skipped = graph.create_node(make_entity(2), root);
    graph.create_node(make_entity(3), skipped);
    const scene::node_handle kept = graph.create_node(make_entity(4), root);
    const scene::node_handle grandchild = graph.create_node(make_entity(5), kept);

    std::vector<scene::node_handle> visited;
    graph.for_each_in_subtree(root, [&](const scene::node_handle node) {
        visited.push_back(node);
        return node != skipped;
    });
    std::ranges::sort(visited);
    std::vector<scene::node_handle> expected{ root, skipped, kept, grandchild };
    std::ranges::sort(expected);
    CHECK(visited == expected);
}

TEST_CASE("clear destroys every node and leaves old handles stale", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle root = graph.create_node(make_entity(1));
    const scene::node_handle child = graph.create_node(make_entity(2), root);
    graph.clear();
    CHECK(graph.size() == 0);
    CHECK(graph.roots().empty());
    CHECK_FALSE(graph.alive(root));
    CHECK_FALSE(graph.alive(child));

    const scene::node_handle again = graph.create_node(make_entity(3));
    CHECK(graph.alive(again));
    CHECK_FALSE(graph.alive(root));
    graph.update_transforms();
    CHECK(graph.last_update_visits() == 1);
}

TEST_CASE("reparent with keep_world keeps the world transform under a uniformly scaled parent", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle parent = graph.create_node(
        make_entity(1),
        {},
        scene::transform{
            .position = { 1.0f, -2.0f, 3.0f },
            .rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ 0.7f, Eigen::Vector3f{ 1, 2, 3 }.normalized() } },
            .scale = Eigen::Vector3f::Constant(2.0f),
        });
    const scene::node_handle grandparent_free = graph.create_node(make_entity(2));
    const scene::node_handle node = graph.create_node(
        make_entity(3),
        grandparent_free,
        scene::transform{
            .position = { 5.0f, 6.0f, -7.0f },
            .rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ -1.2f, Eigen::Vector3f::UnitY() } },
            .scale = { 1.0f, 0.5f, 3.0f },
        });
    graph.update_transforms();
    const Eigen::Affine3f before = graph.world(node);

    tests::log_capture warnings{ "scene" };
    REQUIRE(graph.reparent(node, parent, true));
    CHECK(graph.parent(node) == parent);
    CHECK(children_of(graph, grandparent_free).empty());
    graph.update_transforms();
    CHECK((graph.world(node).matrix() - before.matrix()).cwiseAbs().maxCoeff() < 1e-5f);
    CHECK_FALSE(warnings.contains("isn't uniform"));

    // A null parent makes the node a root, still in place
    REQUIRE(graph.reparent(node, {}, true));
    CHECK_FALSE(graph.parent(node).valid());
    CHECK(std::ranges::find(graph.roots(), node) != graph.roots().end());
    graph.update_transforms();
    CHECK((graph.world(node).matrix() - before.matrix()).cwiseAbs().maxCoeff() < 1e-5f);

    // Without keep_world, the local transform stays and the world follows the parent
    const scene::transform local = graph.local(node);
    REQUIRE(graph.reparent(node, parent, false));
    graph.update_transforms();
    CHECK(graph.local(node).position == local.position);
    CHECK(graph.world(node).isApprox(graph.world(parent) * local.to_affine(), 1e-6f));
}

TEST_CASE("reparent under a non-uniformly scaled parent keeps the closest TRS and warns", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle parent = graph.create_node(
        make_entity(1),
        {},
        scene::transform{
            .position = { 0.0f, 1.0f, 0.0f },
            .rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ 0.4f, Eigen::Vector3f::UnitZ() } },
            .scale = { 1.0f, 2.0f, 3.0f },
        });
    const scene::node_handle node = graph.create_node(
        make_entity(2),
        {},
        scene::transform{
            .position = { 2.0f, 0.0f, 1.0f },
            .rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ 0.9f, Eigen::Vector3f::UnitX() } },
        });
    graph.update_transforms();
    const Eigen::Affine3f parent_world = graph.world(parent);
    const Eigen::Affine3f node_world = graph.world(node);

    tests::log_capture warnings{ "scene" };
    REQUIRE(graph.reparent(node, parent, true));
    CHECK(warnings.contains("isn't uniform"));

    const scene::transform expected = scene::transform::from_affine(parent_world.inverse() * node_world);
    const scene::transform& local = graph.local(node);
    CHECK(local.position.isApprox(expected.position, 1e-5f));
    CHECK(local.scale.isApprox(expected.scale, 1e-5f));
    CHECK(local.rotation.toRotationMatrix().isApprox(expected.rotation.toRotationMatrix(), 1e-5f));

    graph.update_transforms();
    CHECK(graph.world(node).matrix().allFinite());
    // The position is exact even when the linear part can't be
    CHECK(graph.world(node).translation().isApprox(node_world.translation(), 1e-5f));
}

TEST_CASE("reparent rejects a cycle and a dead parent and leaves the tree unchanged", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle root = graph.create_node(make_entity(1));
    const scene::node_handle child = graph.create_node(make_entity(2), root);
    const scene::node_handle grandchild = graph.create_node(make_entity(3), child);
    const scene::node_handle dead = graph.create_node(make_entity(4));
    std::vector<scene::entity> destroyed;
    graph.destroy_node(dead, destroyed);
    graph.update_transforms();

    const auto cycle = graph.reparent(root, grandchild, false);
    REQUIRE_FALSE(cycle);
    CHECK(scene::is_error(cycle.error(), scene::error_kind::cycle));

    const auto self = graph.reparent(child, child, true);
    REQUIRE_FALSE(self);
    CHECK(scene::is_error(self.error(), scene::error_kind::cycle));

    const auto dead_parent = graph.reparent(child, dead, false);
    REQUIRE_FALSE(dead_parent);
    CHECK(scene::is_error(dead_parent.error(), scene::error_kind::invalid_parent));

    CHECK_FALSE(graph.parent(root).valid());
    CHECK(graph.parent(child) == root);
    CHECK(graph.parent(grandchild) == child);
    CHECK(graph.roots().size() == 1);
    CHECK(children_of(graph, root) == std::vector{ child });
    graph.update_transforms();
    CHECK(graph.last_update_visits() == 0); // nothing was marked dirty
}

TEST_CASE("update_transforms matches a brute-force recomputation on a random tree", "[scene][unit]")
{
    std::mt19937 random{ 20261101 };
    scene::scene_graph graph;
    const std::vector<scene::node_handle> nodes = build_random_tree(graph, random, 10'000);
    graph.update_transforms();
    CHECK(graph.last_update_visits() == nodes.size());

    std::uniform_int_distribution<std::size_t> pick{ 0, nodes.size() - 1 };
    for (int round = 0; round < 3; ++round)
    {
        for (int change = 0; change < 1000; ++change)
        {
            graph.set_local(nodes[pick(random)], random_transform(random));
        }
        graph.update_transforms();

        std::size_t mismatches = 0;
        for (const scene::node_handle node : nodes)
        {
            if (!graph.world(node).matrix().isApprox(brute_force_world(graph, node).matrix(), 1e-4f))
            {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }

    // Nothing dirty: no node visited
    graph.update_transforms();
    CHECK(graph.last_update_visits() == 0);

    // One leaf changed: only its path from the root is visited
    scene::node_handle leaf = nodes.back();
    std::size_t depth = 0;
    for (scene::node_handle node = leaf; node.valid(); node = graph.parent(node))
    {
        ++depth;
    }
    graph.set_local(leaf, random_transform(random));
    graph.update_transforms();
    CHECK(graph.last_update_visits() == depth); // only the path from its root
    CHECK(graph.world(leaf).isApprox(brute_force_world(graph, leaf), 1e-4f));
}

TEST_CASE("a discontinuity reports the node and its subtree once", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle parent = graph.create_node(make_entity(1));
    const scene::node_handle child = graph.create_node(make_entity(2), parent);
    const scene::node_handle grandchild = graph.create_node(make_entity(3), child);
    const scene::node_handle sibling = graph.create_node(make_entity(4), parent);
    const scene::node_handle unrelated = graph.create_node(make_entity(5));
    graph.update_transforms();

    scene::transform moved;
    moved.position = { 100.0f, 0.0f, 0.0f };
    graph.set_local(parent, moved, true);
    graph.set_local(unrelated, moved); // moves, but continuously
    graph.set_local(grandchild, scene::transform{}); // dirty under a jumped ancestor: still reported once

    std::map<scene::node_handle, int> reports;
    graph.update_transforms([&](const scene::node_handle node) { ++reports[node]; });
    CHECK(reports == std::map<scene::node_handle, int>{ { parent, 1 }, { child, 1 }, { grandchild, 1 }, { sibling, 1 } });
    CHECK(graph.world(grandchild).translation().x() == 100.0f);

    reports.clear();
    graph.update_transforms([&](const scene::node_handle node) { ++reports[node]; });
    CHECK(reports.empty());

    // The overload without a callback clears jumps too
    graph.set_local(child, moved, true);
    graph.update_transforms();
    graph.update_transforms([&](const scene::node_handle node) { ++reports[node]; });
    CHECK(reports.empty());
}

TEST_CASE("stale handles and dirty reads are debug checks", "[scene][unit]")
{
    scene::scene_graph graph;
    const scene::node_handle parent = graph.create_node(make_entity(1));
    const scene::node_handle child = graph.create_node(make_entity(2), parent);
    const scene::node_handle stale = graph.create_node(make_entity(3));
    std::vector<scene::entity> destroyed;
    graph.destroy_node(stale, destroyed);
    graph.update_transforms();

    tests::recorded_assertions assertions;
    tests::log_capture errors{ "scene" }; // failed checks log too

    scene::transform moved;
    moved.position = { 1.0f, 2.0f, 3.0f };
    graph.set_local(stale, moved);
#ifndef NDEBUG
    CHECK(assertions.contains("set_local: stale or null node handle"));
#endif
    // Setters on a stale handle do nothing, and getters return defaults
    CHECK(graph.local(stale).position.isZero());
    CHECK(graph.world(stale).matrix().isIdentity());
    CHECK(graph.payload(stale) == entt::null);
    CHECK_FALSE(graph.parent(stale).valid());
    CHECK(graph.size() == 2);

    assertions.clear();
    graph.set_local(parent, moved);
    [[maybe_unused]] const Eigen::Affine3f& dirty_parent = graph.world(parent);
    [[maybe_unused]] const Eigen::Affine3f& dirty_ancestor = graph.world(child);
#ifndef NDEBUG
    CHECK(assertions.count() == 2);
    CHECK(assertions.contains("world: the node or an ancestor is dirty"));
#endif

    assertions.clear();
    graph.update_transforms();
    [[maybe_unused]] const Eigen::Affine3f& updated = graph.world(child);
    CHECK(assertions.count() == 0);
}

TEST_CASE("10,000 nodes with 10% dirty update in under 0.2 ms", "[scene][unit][!benchmark][.]")
{
    std::mt19937 random{ 42 };
    scene::scene_graph graph;
    const std::vector<scene::node_handle> nodes = build_random_tree(graph, random, 10'000);
    graph.update_transforms();

    std::uniform_int_distribution<std::size_t> pick{ 0, nodes.size() - 1 };
    std::vector<scene::transform> transforms;
    for (int index = 0; index < 1000; ++index)
    {
        transforms.push_back(random_transform(random));
    }

    constexpr int iterations = 200;
    std::chrono::nanoseconds total{};
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        for (const scene::transform& local : transforms)
        {
            graph.set_local(nodes[pick(random)], local);
        }
        const auto start = std::chrono::steady_clock::now();
        graph.update_transforms();
        total += std::chrono::steady_clock::now() - start;
    }

    const double average = std::chrono::duration<double, std::milli>(total).count() / iterations;
    WARN("average update of 10,000 nodes with 10% dirty: " << average << " ms, " << graph.last_update_visits()
                                                          << " nodes visited in the last one");
#ifdef NDEBUG
    CHECK(average < 0.2);
#endif
}
