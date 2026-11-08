#include "representation/representation_system.h"

#include "representation/presentation_state.h"

#include "avis/detail/check.h"
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

        // New settings: new materials, and every entity again
        if (boxes_ != state.representation.boxes)
        {
            if (boxes_)
            {
                materials_.clear();
                for (const entity owner : scene.registry().view<display_shape>())
                {
                    changed_.push_back(owner);
                }
            }
            boxes_ = state.representation.boxes;
        }

        helpers_.update(scene, state);

        // Building parts creates entities, which raises no display_shape signal, so the list is stable here
        std::vector<entity> visiting = std::exchange(changed_, {});
        std::ranges::sort(visiting);
        const auto [first, last] = std::ranges::unique(visiting);
        visiting.erase(first, last);
        for (const entity owner : visiting)
        {
            shapes_.update(scene, owner, state.representation);
        }
        if (changed_.empty())
        {
            visiting.clear();
            changed_.swap(visiting); // keeps the capacity
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
        helpers_.clear();
        changed_.clear();
        boxes_.reset();
    }

    void representation_system::attach(scene::scene& scene)
    {
        scene_ = &scene;
        owner_thread_ = std::this_thread::get_id();
        entt::registry& registry = scene.registry();
        registry.on_construct<display_shape>().connect<&representation_system::on_shape_changed>(*this);
        registry.on_update<display_shape>().connect<&representation_system::on_shape_changed>(*this);
        registry.on_destroy<display_shape>().connect<&representation_system::on_shape_changed>(*this);

        // What was there before
        for (const entity owner : registry.view<display_shape>())
        {
            changed_.push_back(owner);
        }
    }

    void representation_system::disconnect() noexcept
    {
        if (scene_ == nullptr)
        {
            return;
        }
        entt::registry& registry = scene_->registry();
        registry.on_construct<display_shape>().disconnect(this);
        registry.on_update<display_shape>().disconnect(this);
        registry.on_destroy<display_shape>().disconnect(this);
        scene_ = nullptr;
    }

    void representation_system::on_shape_changed(entt::registry&, const entity entity)
    {
        changed_.push_back(entity);
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
