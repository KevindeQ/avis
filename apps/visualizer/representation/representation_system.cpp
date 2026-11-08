#include "representation/representation_system.h"

#include "representation/presentation_state.h"

#include "avis/detail/check.h"
#include "avis/log.h"
#include "avis/scene/components/driver.h"

#include <algorithm>
#include <utility>

namespace avis::visualizer::representation
{
    namespace
    {
        using namespace avis::scene::components;
        using scene::entity;

        constexpr const char* logger_name = "visualizer";

        // At most one "budget full" warning this often
        constexpr std::chrono::seconds skip_warning_interval{ 5 };

        template<typename Component, auto Handler, typename Owner>
        void connect(entt::registry& registry, Owner& owner)
        {
            registry.on_construct<Component>().template connect<Handler>(owner);
            registry.on_update<Component>().template connect<Handler>(owner);
            registry.on_destroy<Component>().template connect<Handler>(owner);
        }

        template<typename Component, typename Owner>
        void disconnect_from(entt::registry& registry, Owner& owner)
        {
            registry.on_construct<Component>().disconnect(&owner);
            registry.on_update<Component>().disconnect(&owner);
            registry.on_destroy<Component>().disconnect(&owner);
        }
    } // namespace

    result<std::unique_ptr<representation_system>> representation_system::create(scene::asset_cache& assets)
    {
        result<std::unique_ptr<shared_assets>> shared = shared_assets::create(assets);
        if (!shared)
        {
            return std::unexpected(std::move(shared.error()));
        }
        return std::unique_ptr<representation_system>{ new representation_system{ assets, std::move(*shared) } };
    }

    representation_system::representation_system(scene::asset_cache& assets, std::unique_ptr<shared_assets> shared) :
        shared_{ std::move(shared) },
        materials_{ assets },
        shapes_{ *shared_, materials_ },
        geometry_{ materials_ },
        lidar_{ materials_ },
        helpers_{ *shared_ }
    {}

    representation_system::~representation_system()
    {
        disconnect();
    }

    void representation_system::update(scene::scene& scene, const presentation_state& state)
    {
        if (scene_ == nullptr)
        {
            attach(scene);
        }
        if (!check_thread() || !AVIS_CHECK(logger_name, scene_ == &scene, "representation_system: another scene"))
        {
            return;
        }
        const representation_settings& settings = state.representation;

        // New box settings: new materials, and every shape again
        if (boxes_ != settings.boxes)
        {
            if (boxes_)
            {
                materials_.clear();
                for (const entity owner : scene.registry().view<display_shape>())
                {
                    shapes_changed_.push_back(owner);
                }
            }
            boxes_ = settings.boxes;
        }

        helpers_.update(scene, state);

        // Building parts creates entities, which raises none of these signals, so the lists are stable here
        for (const entity owner : take_unique(shapes_changed_))
        {
            shapes_.update(scene, owner, settings);
        }

        bool replan = budget_ != settings.geometry;
        budget_ = settings.geometry;
        replan |= lidar_.restyle(scene, state.lidar, settings.lidar);
        for (const entity owner : take_unique(geometry_changed_))
        {
            replan |= geometry_.update(scene, owner);
        }
        for (const entity owner : take_unique(lidar_changed_))
        {
            replan |= lidar_.update(scene, owner);
        }
        if (replan)
        {
            plan_budget(scene, std::uint64_t{ settings.geometry.budget_mib } << 20);
        }
    }

    void representation_system::detach(scene::scene& scene) noexcept
    {
        if (scene_ != &scene)
        {
            return;
        }
        disconnect();
        shapes_.clear();
        geometry_.clear();
        lidar_.clear();
        helpers_.clear();
        shapes_changed_.clear();
        geometry_changed_.clear();
        lidar_changed_.clear();
        boxes_.reset();
        budget_.reset();
        reduction_ = {};
    }

    void representation_system::attach(scene::scene& scene)
    {
        scene_ = &scene;
        owner_thread_ = std::this_thread::get_id();
        entt::registry& registry = scene.registry();
        connect<display_shape, &representation_system::on_shape_changed>(registry, *this);
        connect<belief_geometry, &representation_system::on_geometry_changed>(registry, *this);
        connect<lidar_points, &representation_system::on_lidar_changed>(registry, *this);

        // What was there before
        for (const entity owner : registry.view<display_shape>())
        {
            shapes_changed_.push_back(owner);
        }
        for (const entity owner : registry.view<belief_geometry>())
        {
            geometry_changed_.push_back(owner);
        }
        for (const entity owner : registry.view<lidar_points>())
        {
            lidar_changed_.push_back(owner);
        }
    }

    void representation_system::disconnect() noexcept
    {
        if (scene_ == nullptr)
        {
            return;
        }
        entt::registry& registry = scene_->registry();
        disconnect_from<display_shape>(registry, *this);
        disconnect_from<belief_geometry>(registry, *this);
        disconnect_from<lidar_points>(registry, *this);
        scene_ = nullptr;
    }

    void representation_system::on_shape_changed(entt::registry&, const entity entity)
    {
        shapes_changed_.push_back(entity);
    }

    void representation_system::on_geometry_changed(entt::registry&, const entity entity)
    {
        geometry_changed_.push_back(entity);
    }

    void representation_system::on_lidar_changed(entt::registry&, const entity entity)
    {
        lidar_changed_.push_back(entity);
    }

    void representation_system::plan_budget(scene::scene& scene, const std::uint64_t budget_bytes)
    {
        items_.clear();
        geometry_.collect(scene, items_);
        const std::size_t chunks = items_.size();
        lidar_.collect(scene, items_);

        const geometry_plan plan = plan_geometry(items_, budget_bytes);
        for (const std::uint32_t index : plan.order)
        {
            if (index < chunks)
            {
                geometry_.execute(scene, index, plan.decisions[index]);
            }
            else
            {
                lidar_.execute(scene, index - chunks, plan.decisions[index]);
            }
        }
        geometry_.finish();
        lidar_.finish();

        reduction_ = plan.reduction;
        reduction_.used_bytes = geometry_.used_bytes(scene) + lidar_.used_bytes(scene);

        const auto now = std::chrono::steady_clock::now();
        if (reduction_.skipped != 0 && now >= last_skip_warning_ + skip_warning_interval)
        {
            last_skip_warning_ = now;
            log::get(logger_name)
                ->warn(
                    "representation: the geometry budget ({} MiB) is full: {} mesh(es) skipped after dropping {} "
                    "sweep(s) and thinning points to {:.0f}%; raise representation.geometry.budget_mib",
                    budget_bytes >> 20,
                    reduction_.skipped,
                    reduction_.sweeps_dropped,
                    reduction_.prefix_fraction * 100.0f);
        }
    }

    std::vector<entity> representation_system::take_unique(std::vector<entity>& changed)
    {
        std::vector<entity> visiting = std::exchange(changed, {});
        std::ranges::sort(visiting);
        const auto [first, last] = std::ranges::unique(visiting);
        visiting.erase(first, last);
        return visiting;
    }

    bool representation_system::check_thread() const noexcept
    {
        if (std::this_thread::get_id() == owner_thread_)
        {
            return true;
        }
        try
        {
            return AVIS_CHECK(logger_name, false, "representation_system: called off the thread that owns it");
        }
        catch (...)
        {
            return false;
        }
    }
} // namespace avis::visualizer::representation
