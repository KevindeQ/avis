#ifndef AVIS_GRAPHICS_RENDER_GRAPH_EXECUTOR_H
#define AVIS_GRAPHICS_RENDER_GRAPH_EXECUTOR_H

// Private: the render graph's execution (docs/design/10-render-graph.md §7): materialise, which fills the resource bag;
// the per-thread current pass the bag checks lookups against; and the executor, which records the passes in groups on
// the parallel scheduler and submits them in order.

#include "render_graph_compiler.h"
#include "render_graph_tables.h"

#include "avis/error.h"
#include "avis/graphics/detail/render_graph_execute_sender.h"
#include "avis/graphics/linear_arena.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"

#include <stdexec/execution.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace avis::graphics
{
    class frame_scheduler;
    class frame_token;
    class transient_resource_pool;
} // namespace avis::graphics

namespace avis::graphics::detail
{
    // The pass whose execute callback runs on this thread, or nullptr. Debug builds check bag lookups against its
    // declared accesses (§3.4).
    const render_pass_node* current_pass() noexcept;

    // Sets the current pass for the thread while alive; restores the previous one on destruction
    class current_pass_scope
    {
    public:
        explicit current_pass_scope(const render_pass_node* pass) noexcept;
        ~current_pass_scope();

        current_pass_scope(const current_pass_scope&) = delete;
        current_pass_scope& operator=(const current_pass_scope&) = delete;

    private:
        const render_pass_node* previous_;
    };

    struct resource_bag_access
    {
        static_assert(std::is_trivially_destructible_v<resource_bag>);

        // A bag in the arena; nothing to destroy
        static const resource_bag&
            create(linear_arena& arena, rhi::resources resources, std::span<const bag_entry> entries);

        static std::span<const bag_entry> entries(const resource_bag& bag) noexcept
        {
            return bag.entries_;
        }
    };

    // §7 step 1: prepares the pool's heap for the frame's context, acquires the placed resources of the live
    // transient resources at their offsets, creates the views their accesses need in the frame's descriptor range,
    // and maps imports to their handles. Fails with the pool's or the scheduler's first error.
    result<const resource_bag*> materialise(
        const render_graph_tables& tables,
        const compiled_graph& compiled,
        transient_resource_pool& pool,
        frame_scheduler& frames,
        const frame_token& frame,
        rhi::resources resources,
        linear_arena& arena);

    class render_graph_executor;

    // The bulk's item: records one group
    struct record_group_function
    {
        render_graph_executor* executor;

        void operator()(std::size_t group) const noexcept;
    };

    // Receives the end of the bulk, on a parallel worker. No stop token: the bulk always runs to its end, and the
    // executor decides how execute completes.
    struct record_groups_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value() noexcept;
        template<class Error>
        void set_error(Error&& error) noexcept;
        void set_stopped() noexcept;

        stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        render_graph_executor* executor;
    };

    using record_groups_sender = decltype(stdexec::bulk(
        stdexec::schedule(std::declval<stdexec::parallel_scheduler>()),
        stdexec::par,
        std::size_t{},
        std::declval<record_group_function>()));
    using record_groups_operation = stdexec::connect_result_t<record_groups_sender, record_groups_receiver>;

    // Records and submits a compiled frame (§7 steps 2–4). One per graph, reused across frames so a steady-state frame
    // allocates nothing; one execute at a time.
    class render_graph_executor
    {
    public:
        explicit render_graph_executor(render_graph& graph) noexcept : graph_{ graph } {}
        ~render_graph_executor();

        render_graph_executor(const render_graph_executor&) = delete;
        render_graph_executor& operator=(const render_graph_executor&) = delete;

        // On the thread that starts execute: materialises, prepares the groups and the trailing lists, and starts the
        // bulk (or finishes at once when nothing is recorded). Completes the operation inline on a failure.
        void run(render_graph_execute_operation& operation, const frame_token& frame) noexcept;

        // On parallel workers
        void record(std::size_t group) noexcept;
        void finish() noexcept;
        void fail(avis::error error) noexcept;
        void scheduler_stopped() noexcept;

    private:
        struct barrier_lists
        {
            std::span<const rhi::texture_barrier> textures;
            std::span<const rhi::buffer_barrier> buffers;

            bool empty() const noexcept
            {
                return textures.empty() && buffers.empty();
            }
        };

        // Consecutive passes of one batch, recorded into one command list by one worker
        struct recording_group
        {
            std::uint32_t batch = 0;
            rhi::queue_type queue = rhi::queue_type::graphics;
            std::span<const std::uint32_t> passes;
            bool ends_batch = false; // records the batch's end barriers after its passes
            std::uint32_t slot = 0; // its list among the lists of its queue type
            std::optional<avis::error> error;
        };

        template<rhi::queue_type Queue>
        std::vector<std::optional<rhi::command_list<Queue>>>& slots() noexcept;
        template<rhi::queue_type Queue>
        std::vector<rhi::command_list<Queue>>& submit_buffer() noexcept;
        template<rhi::queue_type Queue>
        std::optional<rhi::command_list<Queue>>& trailing() noexcept;
        template<rhi::queue_type Queue>
        rhi::queue<Queue> queue() const noexcept;

        barrier_lists convert(const barrier_batch& batch);
        void build_groups();
        template<rhi::queue_type Queue>
        result<void> prepare_trailing_list();

        template<rhi::queue_type Queue>
        void record_group(recording_group& group);
        template<rhi::queue_type Queue>
        void record_pass(rhi::command_list<Queue>& list, std::uint32_t pass);

        template<rhi::queue_type Queue>
        result<rhi::synchronization_point> submit_batch(std::uint32_t batch, std::size_t first_group);
        template<rhi::queue_type Queue>
        result<std::optional<rhi::synchronization_point>> submit_trailing();

        void complete(result<submitted_frame> outcome) noexcept;
        void discard() noexcept;

        render_graph& graph_;
        render_graph_execute_operation* operation_ = nullptr;
        const frame_token* frame_ = nullptr;
        const resource_bag* bag_ = nullptr;
        std::span<const bag_entry> entries_;

        // The compiled barriers with physical handles, in the arena
        std::span<barrier_lists> pass_barriers_; // per pass
        std::span<barrier_lists> batch_end_barriers_; // per batch
        std::array<barrier_lists, 3> trailing_barriers_{}; // per queue

        std::vector<recording_group> groups_;
        std::unordered_map<std::string_view, double> timings_; // completed GPU timings of frame N − F, by pass name
        std::vector<rhi::synchronization_point> batch_points_; // per batch, once submitted
        std::vector<rhi::synchronization_point> waits_;
        std::vector<std::optional<rhi::graphics_command_list>> graphics_slots_;
        std::vector<std::optional<rhi::compute_command_list>> compute_slots_;
        std::vector<std::optional<rhi::copy_command_list>> copy_slots_;
        std::vector<rhi::graphics_command_list> graphics_submit_;
        std::vector<rhi::compute_command_list> compute_submit_;
        std::vector<rhi::copy_command_list> copy_submit_;
        std::optional<rhi::graphics_command_list> graphics_trailing_;
        std::optional<rhi::compute_command_list> compute_trailing_;
        std::optional<rhi::copy_command_list> copy_trailing_;
        std::optional<avis::error> bulk_error_; // the parallel scheduler failed
        bool bulk_stopped_ = false; // the parallel scheduler is stopping
        std::optional<record_groups_operation> bulk_; // connected again every frame
    };
} // namespace avis::graphics::detail

#endif
