#include "simulation/simulation_thread.h"

#include "representation/representation_system.h"
#include "representation/scene_representation.h"

#include "avis/detail/check.h"
#include "avis/log.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/components/vehicle.h"
#include "avis/threading/thread_pool.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <type_traits>
#include <utility>

namespace avis::visualizer::simulation
{
    namespace
    {
        using namespace avis::scene::components;
        using scene::entity;

        constexpr const char* logger_name = "visualizer";

        // The Driver's extent of an entity (08 §3.1, transform_update.extent); none without a display shape
        std::optional<Eigen::Vector3f> extent_of(const scene::scene& world, const entity target)
        {
            const display_shape* shape = world.registry().try_get<display_shape>(target);
            return shape != nullptr ? std::optional{ shape->extent } : std::nullopt;
        }

        const char* command_name(const simulation_command& command) noexcept
        {
            constexpr const char* names[] = { "start", "pause", "resume", "single step", "stop", "set speed" };
            return command.index() < std::size(names) ? names[command.index()] : "presentation";
        }
    } // namespace

    // ---- simulation_loop ----

    simulation_loop::simulation_loop(
        scene::asset_cache& assets,
        scene::snapshot_buffer& snapshots,
        gui_sink& sink,
        const simulation_settings& settings,
        const clock::time_point now,
        representation::scene_representation* const representation) :
        snapshots_{ snapshots },
        sink_{ sink },
        representation_{ representation },
        owner_thread_{ std::this_thread::get_id() },
        scene_{ assets },
        presentation_{ representation::presentation_state::from_settings(
            settings.representation, settings.environment) }
    {
        scene_.set_environment(presentation_.environment);
        anchor_ = { .t_base = avis::simulation::simulation_duration{ 0 }, .wall_base = now, .rate = 0.0 };
        if (settings.demo_scene)
        {
            demo_.emplace();
            next_tick_ = now;
        }
        next_statistics_ = now;

        create_helpers();
        republish();
    }

    simulation_loop::~simulation_loop()
    {
        if (representation_ != nullptr)
        {
            representation_->detach(scene_);
        }
    }

    void simulation_loop::handle(const simulation_command& command, const clock::time_point now)
    {
        if (!check_thread("handle"))
        {
            return;
        }
        if (is_simulation_command(command))
        {
            log::get(logger_name)
                ->info("simulation: '{}' needs a connected Driver, which comes with Part B", command_name(command));
            return;
        }

        std::visit(
            [&]<typename Command>(const Command& value)
            {
                if constexpr (std::is_same_v<Command, select_entity>)
                {
                    select(value.id);
                }
                else if constexpr (std::is_same_v<Command, clear_selection>)
                {
                    select(0);
                }
                else if constexpr (std::is_same_v<Command, set_entity_visibility>)
                {
                    presentation_.entity_visible[value.id] = value.visible;
                    visibility_dirty_ = true;
                }
                else if constexpr (std::is_same_v<Command, set_category_visibility>)
                {
                    presentation_.category_visible[static_cast<std::size_t>(value.category)] = value.visible;
                    visibility_dirty_ = true;
                }
                else if constexpr (std::is_same_v<Command, set_helper_visibility>)
                {
                    (value.helper == representation::helper_kind::grid ? presentation_.grid :
                                                                         presentation_.axes_gizmo) = value.visible;
                }
                else if constexpr (std::is_same_v<Command, set_representation_settings>)
                {
                    presentation_.representation = value.settings;
                }
                else if constexpr (std::is_same_v<Command, set_category_labels>)
                {
                    presentation_.labels[static_cast<std::size_t>(value.category)] = value.enabled;
                }
                else if constexpr (std::is_same_v<Command, set_category_trails>)
                {
                    presentation_.trails[static_cast<std::size_t>(value.category)] = value.enabled;
                }
                else if constexpr (std::is_same_v<Command, set_entity_trail>)
                {
                    presentation_.entity_trails[value.id] = value.enabled;
                }
                else if constexpr (std::is_same_v<Command, set_staleness>)
                {
                    presentation_.stale = value.enabled;
                }
                else if constexpr (std::is_same_v<Command, set_visualization>)
                {
                    presentation_.environment = value.environment;
                    scene_.set_environment(value.environment);
                }
                else if constexpr (std::is_same_v<Command, reset_scene>)
                {
                    reset();
                }
                // The simulation commands returned above
            },
            command);
        mark_dirty(now);
    }

    void simulation_loop::update(const clock::time_point now)
    {
        if (!check_thread("update"))
        {
            return;
        }

        // One tick per call at most: a late loop skips ticks instead of catching up in a burst (03 §4.2)
        if (demo_ && now >= next_tick_)
        {
            run_tick(now);
            next_tick_ += demo_scene::tick_interval;
            if (next_tick_ <= now)
            {
                next_tick_ = now + demo_scene::tick_interval;
            }
        }

        if (dirty_ && now >= dirty_since_ + republish_interval)
        {
            republish();
        }

        post_gui_updates(now);
    }

    simulation_loop::clock::time_point simulation_loop::next_due() const noexcept
    {
        clock::time_point due = next_statistics_;
        if (demo_)
        {
            due = std::min(due, next_tick_);
        }
        if (dirty_)
        {
            due = std::min(due, dirty_since_ + republish_interval);
        }
        if (selected_pending_)
        {
            due = std::min(due, last_selected_post_ + selected_interval);
        }
        return due;
    }

    void simulation_loop::run_tick(const clock::time_point now)
    {
        // Step 7 of the last tick, deferred to here: end_tick sets previous_world = world, so running it right after
        // the publish would give a republish between two ticks world_previous = world_current (03 §4.4)
        if (tick_open_)
        {
            scene_.end_tick();
            tick_open_ = false;
        }

        ++step_;
        const avis::simulation::simulation_duration time =
            avis::simulation::time_of_step(step_, demo_scene::tick_interval);

        demo_->tick(scene_, step_, time); // 1: the scene source's messages
        scene_.update_world_transforms(); // 2
        scene_.update_trails(time); // 3

        // The anchor of a step committed while running (03 §5.2), at the requested speed of 1
        time_previous_ = avis::simulation::time_of_step(step_ - 1, demo_scene::tick_interval);
        time_current_ = time;
        anchor_ = { .t_base = time_previous_, .wall_base = now, .rate = 1.0 };

        present(); // 4
        scene_.update_world_transforms(); // 5
        publish(); // 6
        tick_open_ = true; // 7 runs at the start of the next tick
        after_publish();
    }

    void simulation_loop::republish()
    {
        // Commands may have changed the scene (Reset destroys entities): the transforms are brought up to date first,
        // so the representation reads current worlds. Nodes that didn't change aren't touched, and previous_world
        // stays as the last tick left it.
        scene_.update_world_transforms();
        present(); // 4
        scene_.update_world_transforms(); // 5
        publish(); // 6
        after_publish();
    }

    void simulation_loop::present()
    {
        if (representation_ != nullptr)
        {
            representation_->update(scene_, presentation_);
        }
        apply_visibility();
        apply_selection_flags();
    }

    void simulation_loop::publish()
    {
        scene::snapshot& target = snapshots_.begin_write();
        target.step = step_;
        target.time_previous = time_previous_;
        target.time_current = time_current_;
        target.anchor = anchor_;
        scene_.make_snapshot(target);
        snapshots_.publish();
        ++publishes_;
        dirty_ = false; // a tick's publish includes the pending presentation changes (03 §4.4)
    }

    void simulation_loop::after_publish()
    {
        std::vector<scene::tree_change> changes = scene_.take_tree_changes();
        if (!changes.empty())
        {
            sink_.tree_changed(std::move(changes));
        }

        // The selected entity's values may have changed with the tick
        if (presentation_.selected != 0)
        {
            selected_pending_ = true;
        }

        // The follow target: the selected vehicle, otherwise the primary one (12 §5). Posted when it changes and when
        // its extent does; its world bounds change with every turn, so they don't decide.
        std::optional<entity> target;
        if (const std::optional<entity> selected =
                presentation_.selected != 0 ? scene_.find(presentation_.selected) : std::nullopt;
            selected && scene_.registry().all_of<vehicle>(*selected))
        {
            target = selected;
        }
        else
        {
            for (const auto [candidate, state] : scene_.registry().view<vehicle>().each())
            {
                if (state.primary)
                {
                    target = candidate;
                    break;
                }
            }
        }
        const scene::object_id target_id = target ? scene_.registry().get<identity>(*target).id : 0;
        const std::optional<Eigen::Vector3f> extent = target ? extent_of(scene_, *target) : std::nullopt;
        if (!follow_posted_ || follow_id_ != target_id || follow_extent_ != extent)
        {
            follow_posted_ = true;
            follow_id_ = target_id;
            follow_extent_ = extent;
            sink_.follow_target_changed(
                { .id = target_id, .bounds = target ? scene_.world_bounds(*target) : std::nullopt });
        }
    }

    void simulation_loop::post_gui_updates(const clock::time_point now)
    {
        if (selected_pending_ && now >= last_selected_post_ + selected_interval)
        {
            const std::optional<entity> selected =
                presentation_.selected != 0 ? scene_.find(presentation_.selected) : std::nullopt;
            sink_.selected_entity_changed(selected ? std::optional{ scene_.reflect(*selected) } : std::nullopt);
            last_selected_post_ = now;
            selected_pending_ = false;
        }

        if (now >= next_statistics_)
        {
            sink_.statistics_changed(scene_.statistics());
            next_statistics_ += statistics_interval;
            if (next_statistics_ <= now)
            {
                next_statistics_ = now + statistics_interval;
            }
        }
    }

    void simulation_loop::create_helpers()
    {
        const result<entity> helpers =
            scene_.create_entity({ .name = helpers_name, .category = scene::entity_category::helper });
        if (!helpers)
        {
            log::get(logger_name)->error("simulation: the helpers group could not be created: {}", helpers.error());
        }
    }

    void simulation_loop::reset()
    {
        entt::registry& registry = scene_.registry();

        // Destroying a parent destroys its subtree, so later entries may already be gone
        const auto driver_entities = registry.view<driver_entity>();
        std::vector<entity> doomed(driver_entities.begin(), driver_entities.end());
        for (const entity target : doomed)
        {
            if (scene_.alive(target))
            {
                static_cast<void>(scene_.destroy_entity(target));
            }
        }

        // Helpers outside their owner's subtree (trails live under `helpers`, 08 §8.2)
        doomed.clear();
        for (const auto [helper, owner] : registry.view<represents>().each())
        {
            if (!scene_.alive(owner.owner))
            {
                doomed.push_back(helper);
            }
        }
        for (const entity target : doomed)
        {
            if (scene_.alive(target))
            {
                static_cast<void>(scene_.destroy_entity(target));
            }
        }

        // A new run starts with no per-entity toggles
        presentation_.entity_visible.clear();
        presentation_.entity_trails.clear();
        visibility_dirty_ = true;
    }

    void simulation_loop::select(const scene::object_id id)
    {
        scene::object_id resolved = 0;
        if (id != 0)
        {
            if (const std::optional<entity> found = scene_.find(id))
            {
                const entity owner = scene_.selection_target(*found);
                resolved = scene_.registry().get<identity>(owner).id;
            }
            else
            {
                log::get(logger_name)->debug("simulation: entity {:#x} can't be selected: it doesn't exist", id);
            }
        }
        if (resolved != presentation_.selected)
        {
            presentation_.selected = resolved;
            selected_pending_ = true;
        }
    }

    void simulation_loop::apply_selection_flags()
    {
        entt::registry& registry = scene_.registry();
        for (const entity flagged : flagged_)
        {
            if (scene_.alive(flagged) && registry.all_of<render_options>(flagged))
            {
                registry.patch<render_options>(
                    flagged,
                    [](render_options& options)
                    { options.flags &= ~avis::flags<scene::instance_flags>{ scene::instance_flags::selected }; });
            }
        }
        flagged_.clear();

        if (presentation_.selected == 0)
        {
            return;
        }
        const std::optional<entity> selected = scene_.find(presentation_.selected);
        if (!selected)
        {
            // Destroyed by the Driver, a tick or Reset: the selection clears (12 §6)
            presentation_.selected = 0;
            selected_pending_ = true;
            return;
        }

        const auto flag = [&](const entity target)
        {
            if (!registry.all_of<mesh>(target))
            {
                return;
            }
            static_cast<void>(registry.get_or_emplace<render_options>(target));
            registry.patch<render_options>(
                target, [](render_options& options) { options.flags |= scene::instance_flags::selected; });
            flagged_.push_back(target);
        };
        flag(*selected);
        for (const auto [helper, owner] : registry.view<represents>().each())
        {
            if (owner.owner == *selected)
            {
                flag(helper);
            }
        }
    }

    void simulation_loop::apply_visibility()
    {
        if (!visibility_dirty_ && visibility_version_ == scene_.scene_version())
        {
            return;
        }
        visibility_dirty_ = false;
        visibility_version_ = scene_.scene_version();

        entt::registry& registry = scene_.registry();
        for (const auto [target, who] : registry.view<identity>().each())
        {
            // Helpers follow their owner
            const identity* decider = &who;
            if (const represents* owner = registry.try_get<represents>(target);
                owner != nullptr && scene_.alive(owner->owner))
            {
                decider = &registry.get<identity>(owner->owner);
            }
            const bool visible = presentation_.visible(decider->id, decider->category);

            const visibility* current = registry.try_get<visibility>(target);
            if (current == nullptr ? visible : current->visible == visible)
            {
                continue; // written only on a change, so the representation sees real changes
            }
            static_cast<void>(registry.get_or_emplace<visibility>(target));
            registry.patch<visibility>(target, [visible](visibility& value) { value.visible = visible; });
        }
    }

    void simulation_loop::mark_dirty(const clock::time_point now) noexcept
    {
        if (!dirty_)
        {
            dirty_ = true;
            dirty_since_ = now;
        }
    }

    bool simulation_loop::check_thread(const char* const operation) const noexcept
    {
        if (std::this_thread::get_id() == owner_thread_)
        {
            return true;
        }
        try
        {
            return AVIS_CHECK(
                logger_name,
                false,
                std::format("simulation_loop::{}: called off the simulation thread, which owns the scene", operation));
        }
        catch (...)
        {
            return false;
        }
    }

    // ---- simulation_thread ----

    result<std::unique_ptr<simulation_thread>> simulation_thread::start(
        threading::thread_pool& pool,
        scene::asset_cache& assets,
        scene::snapshot_buffer& snapshots,
        gui_sink& sink,
        simulation_settings settings)
    {
        std::unique_ptr<simulation_thread> made{ new simulation_thread{} };
        simulation_thread* const self = made.get();

        threading::thread_options options;
        options.on_exit = [](const std::optional<error>& failure)
        {
            if (failure)
            {
                log::get(logger_name)->critical("the simulation thread stopped with an error: {}", *failure);
            }
        };

        result<threading::thread_handle> started = pool.start(
            "simulation",
            [self, &assets, &snapshots, &sink, settings = std::move(settings)](const std::stop_token& stop)
            {
                // Before the loop, which detaches it when it goes. Without one, nothing semantic is drawn.
                std::unique_ptr<representation::representation_system> representation;
                if (result<std::unique_ptr<representation::representation_system>> created =
                        representation::representation_system::create(assets))
                {
                    representation = std::move(*created);
                }
                else
                {
                    log::get(logger_name)->error("the scene representation could not be created: {}", created.error());
                }
                simulation_loop loop{
                    assets, snapshots, sink, settings, simulation_loop::clock::now(), representation.get()
                };
                std::vector<simulation_command> commands;
                for (;;)
                {
                    loop.update(simulation_loop::clock::now());
                    if (!self->commands_.wait_and_drain(commands, loop.next_due(), stop))
                    {
                        break;
                    }
                    const simulation_loop::clock::time_point now = simulation_loop::clock::now();
                    for (const simulation_command& command : commands)
                    {
                        loop.handle(command, now);
                    }
                }
            },
            std::move(options));
        if (!started)
        {
            return std::unexpected(std::move(started.error()));
        }
        made->thread_ = std::move(*started);
        return made;
    }

    simulation_thread::~simulation_thread()
    {
        if (thread_)
        {
            thread_.request_stop();
            thread_.join();
        }
    }

    void simulation_thread::post(simulation_command command)
    {
        commands_.push(std::move(command));
    }
} // namespace avis::visualizer::simulation
