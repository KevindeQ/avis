#ifndef AVIS_VISUALIZER_SIMULATION_SIMULATION_THREAD_H
#define AVIS_VISUALIZER_SIMULATION_SIMULATION_THREAD_H

// The simulation thread (docs/design/12-visualizer.md §4, 03 §4.2): it owns the scene and runs the presentation path
// of the tick loop. Part A has no steps: a scene source (the demo scene) may tick, and the GUI's commands change what's
// shown. Part B adds the step handling (task 12.19).
//
// Two parts:
// - simulation_loop does the work, given the time. It's a plain object on the thread that constructed it, so tests
//   drive it on the test thread with a made-up clock.
// - simulation_thread runs a simulation_loop on a thread from a thread pool, fed by a command_queue.

#include "representation/presentation_state.h"
#include "simulation/command_queue.h"
#include "simulation/demo_scene.h"
#include "simulation/gui_sink.h"
#include "simulation/simulation_command.h"

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"
#include "avis/simulation/time.h"
#include "avis/threading/thread_handle.h"

#include <Eigen/Dense>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace avis::threading
{
    class thread_pool;
} // namespace avis::threading

namespace avis::visualizer::representation
{
    class scene_representation;
} // namespace avis::visualizer::representation

namespace avis::visualizer::simulation
{
    struct simulation_settings
    {
        representation::representation_settings representation; // visualizer.yaml's `representation`
        scene::environment_settings environment; // until a scenario sets its `visualization`
        bool demo_scene = false; // --demo-scene: the demo scene ticks at 10 Hz
    };

    class simulation_loop
    {
    public:
        using clock = std::chrono::steady_clock;

        // At most one presentation republish per this long (03 §4.4). The window opens at the first change, so a
        // burst of commands shorter than this gives one republish.
        static constexpr std::chrono::milliseconds republish_interval{ 16 };
        static constexpr std::chrono::milliseconds selected_interval{ 50 }; // the selected entity at up to 20 Hz
        static constexpr std::chrono::milliseconds statistics_interval{ 500 }; // 2 Hz

        // The name of the group the Visualizer's helpers live under (08 §4.2)
        static constexpr const char* helpers_name = "helpers";

        // Creates the scene with its helpers and publishes the first snapshot. Everything given must outlive the
        // loop; `representation` may be null (no meshes for the semantic components, until task 12.8).
        simulation_loop(
            scene::asset_cache& assets,
            scene::snapshot_buffer& snapshots,
            gui_sink& sink,
            const simulation_settings& settings,
            clock::time_point now,
            representation::scene_representation* representation = nullptr);
        ~simulation_loop();

        simulation_loop(const simulation_loop&) = delete;
        simulation_loop& operator=(const simulation_loop&) = delete;

        // Applies a command (12 §4.1). A presentation command marks the snapshot dirty; simulation commands are
        // logged and ignored until task 12.19.
        void handle(const simulation_command& command, clock::time_point now);

        // Does what's due at `now`: a demo tick, a coalesced republish, then the GUI's updates
        void update(clock::time_point now);

        // When update() has something to do next, at the latest the next statistics
        clock::time_point next_due() const noexcept;

        // For tests, and for the thread: the scene and the state the commands set. Owned by this thread.
        scene::scene& scene() noexcept
        {
            return scene_;
        }

        const representation::presentation_state& presentation() const noexcept
        {
            return presentation_;
        }

        // Every snapshot this loop published: ticks and republishes
        std::uint64_t publishes() const noexcept
        {
            return publishes_;
        }

        avis::simulation::step_index step() const noexcept
        {
            return step_;
        }

    private:
        // The tick sequence of 08 §9.6 with a made-up step: the demo scene writes (1), then 2–6. Step 7 (end_tick)
        // runs at the start of the next tick, so republishes in between keep this tick's world_previous.
        void run_tick(clock::time_point now);

        // Steps 4–6 without a tick: the step fields and the anchor stay (03 §4.4)
        void republish();

        // Step 4: the representation, then the selection and visibility flags
        void present();

        // Step 6
        void publish();

        // After a tick or a republish: tree changes, the follow target, and a check that the selection still exists
        void after_publish();

        void post_gui_updates(clock::time_point now);

        void create_helpers();

        // Destroys every Driver entity, then the helpers whose owner went with them
        void reset();

        void select(scene::object_id id);

        // The selected flag on the selected entity and the helpers that represent it, cleared on the ones before
        void apply_selection_flags();

        // visibility on every entity from its toggles and its ancestors; helpers follow their owner
        void apply_visibility();

        void mark_dirty(clock::time_point now) noexcept;

        // Debug check: the calling thread is the one that constructed the loop
        bool check_thread(const char* operation) const noexcept;

        scene::snapshot_buffer& snapshots_;
        gui_sink& sink_;
        representation::scene_representation* representation_;
        std::thread::id owner_thread_;

        scene::scene scene_;
        representation::presentation_state presentation_;
        std::optional<demo_scene> demo_;

        // The step fields every publish carries; only a tick changes them
        avis::simulation::step_index step_ = 0;
        avis::simulation::simulation_duration time_previous_{ 0 };
        avis::simulation::simulation_duration time_current_{ 0 };
        avis::simulation::time_anchor anchor_;
        std::uint64_t publishes_ = 0;
        bool tick_open_ = false; // a tick published and its end_tick hasn't run yet

        // Republishes (03 §4.4)
        bool dirty_ = false;
        clock::time_point dirty_since_{};
        clock::time_point last_republish_{};

        // The demo scene's next tick
        clock::time_point next_tick_{};

        // Selection: the entities whose flag is set, and the 20 Hz updates
        std::vector<scene::entity> flagged_;
        bool selected_pending_ = false; // the selection or its entity changed since the last post
        clock::time_point last_selected_post_{};
        bool selected_posted_ = false; // a selected_entity_changed has been sent since startup

        // Visibility is recomputed after a structural change or a visibility command
        bool visibility_dirty_ = true;
        std::uint64_t visibility_version_ = 0;

        clock::time_point next_statistics_{};

        // The follow target as last posted, with the extent it had
        bool follow_posted_ = false;
        scene::object_id follow_id_ = 0;
        std::optional<Eigen::Vector3f> follow_extent_;
    };

    // Runs a simulation_loop on a thread from a thread pool (an application thread, 01 §4.1). Commands from any thread
    // go through post(); the loop sleeps until a command or the next due time.
    class simulation_thread
    {
    public:
        // Starts the thread. Fails when the pool has no room. The loop and its scene are created on the new thread;
        // the asset cache, the snapshot buffer and the sink must outlive this object.
        static result<std::unique_ptr<simulation_thread>> start(
            threading::thread_pool& pool,
            scene::asset_cache& assets,
            scene::snapshot_buffer& snapshots,
            gui_sink& sink,
            simulation_settings settings);

        ~simulation_thread(); // stops the loop and joins the thread

        simulation_thread(const simulation_thread&) = delete;
        simulation_thread& operator=(const simulation_thread&) = delete;

        // Any thread
        void post(simulation_command command);

    private:
        simulation_thread() = default;

        command_queue<simulation_command> commands_;
        threading::thread_handle thread_;
    };
} // namespace avis::visualizer::simulation

#endif
