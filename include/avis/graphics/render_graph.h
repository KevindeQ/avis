#ifndef AVIS_GRAPHICS_RENDER_GRAPH_H
#define AVIS_GRAPHICS_RENDER_GRAPH_H

// The render graph (docs/design/10-render-graph.md): graphics, compute and copy passes rebuilt every frame. Build
// declares passes and their virtual resources (§3), compile culls, orders and schedules them (§6), execute records and
// submits them (§7). Free of D3D12 and <windows.h>.

#include "avis/error.h"
#include "avis/graphics/buffer.h"
#include "avis/graphics/detail/render_pass_storage.h"
#include "avis/graphics/linear_arena.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource.h"
#include "avis/graphics/texture.h"
#include "avis/graphics/view_scope.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace avis::graphics
{
    class frame_scheduler;
    class frame_token;
    class gpu_profiler;
    class resource_bag;
    class transient_resource_pool;

    namespace detail
    {
        struct compiled_graph; // lib/avis/graphics/render_graph_compiler.h
        struct render_graph_tables; // lib/avis/graphics/render_graph_tables.h
    } // namespace detail

    // The application maps render.async_compute and render.graph_aliasing onto it, together with the device's
    // capabilities
    struct render_graph_configuration
    {
        bool async_compute = true; // compute passes hinted async_compute go to the compute queue
        bool aliasing = true; // transient resources share heap memory when their lifetimes don't overlap
        std::uint32_t passes_per_group_without_timings = 4; // recording group size without GPU timings
        double group_target_milliseconds = 1.0; // recording group size from GPU timings
    };

    // The last submission of each queue the graph submitted to in one frame (at most one per queue), for end_frame. A
    // graphics point alone wouldn't cover async compute work that no later graphics batch waits on.
    struct submitted_frame
    {
        std::array<rhi::synchronization_point, 3> points{};
        std::uint32_t count = 0;

        std::span<const rhi::synchronization_point> last_submissions() const noexcept
        {
            return { points.data(), count };
        }
    };

    class render_graph
    {
    public:
        // The RHI views it uses (09 §4.3, D-96), the pool of transient heaps, and the frame scheduler for the frame's
        // constants and the views of transient resources. With a profiler, every pass is timed (17 §6).
        render_graph(
            rhi::resources resources,
            rhi::command_lists command_lists,
            rhi::queue<rhi::queue_type::graphics> graphics_queue,
            rhi::queue<rhi::queue_type::compute> compute_queue,
            rhi::queue<rhi::queue_type::copy> copy_queue,
            transient_resource_pool& pool,
            frame_scheduler& frames,
            gpu_profiler* profiler = nullptr,
            const render_graph_configuration& configuration = {});
        ~render_graph();

        // Passes keep pointers to the graph
        render_graph(const render_graph&) = delete;
        render_graph& operator=(const render_graph&) = delete;

        // Setup runs immediately, inside the call, on the calling thread. PassData is default-constructed in the
        // graph's arena, as is the decayed execute callable; the returned reference stays valid until the next reset().
        template<typename PassData, typename Setup, typename Execute>
            requires std::default_initializable<PassData> && std::invocable<Setup&, render_pass_builder&, PassData&> &&
                     std::invocable<
                         const std::decay_t<Execute>&,
                         const PassData&,
                         const resource_bag&,
                         graphics_pass_context&>
        const PassData& add_graphics_pass(const std::string_view name, Setup&& setup, Execute&& execute)
        {
            return add_pass<PassData, graphics_pass_context>(
                name, pass_type::graphics, compute_queue_hint::graphics, setup, std::forward<Execute>(execute));
        }

        template<typename PassData, typename Setup, typename Execute>
            requires std::default_initializable<PassData> && std::invocable<Setup&, render_pass_builder&, PassData&> &&
                     std::invocable<
                         const std::decay_t<Execute>&,
                         const PassData&,
                         const resource_bag&,
                         compute_pass_context&>
        const PassData& add_compute_pass(
            const std::string_view name,
            Setup&& setup,
            Execute&& execute,
            const compute_queue_hint queue = compute_queue_hint::graphics)
        {
            return add_pass<PassData, compute_pass_context>(
                name, pass_type::compute, queue, setup, std::forward<Execute>(execute));
        }

        template<typename PassData, typename Setup, typename Execute>
            requires std::default_initializable<PassData> && std::invocable<Setup&, render_pass_builder&, PassData&> &&
                     std::invocable<
                         const std::decay_t<Execute>&,
                         const PassData&,
                         const resource_bag&,
                         copy_pass_context&>
        const PassData& add_copy_pass(const std::string_view name, Setup&& setup, Execute&& execute)
        {
            return add_pass<PassData, copy_pass_context>(
                name, pass_type::copy, compute_queue_hint::graphics, setup, std::forward<Execute>(execute));
        }

        // Version 0 of an external resource, never aliased. It starts the frame in `initial` and is left in `final`.
        // ready_after: the point its data arrives with (an upload queue flush, 17 §7); the first batch that uses it
        // waits on it.
        texture import_texture(
            std::string_view name,
            rhi::texture_handle texture,
            rhi::access_state initial,
            rhi::access_state final,
            std::optional<rhi::synchronization_point> ready_after = {});
        buffer import_buffer(
            std::string_view name,
            rhi::buffer_handle buffer,
            rhi::access_state initial,
            rhi::access_state final,
            std::optional<rhi::synchronization_point> ready_after = {});

        // Qualifies names with "<view_name>/" while the scope lives (§8). Debug checks: no scope open, a name not used
        // this frame, non-empty and without '/'. A refused scope qualifies nothing. View-relative textures created in
        // the scope resolve against `view_size`; without one, against the graph's view size.
        [[nodiscard]] view_scope begin_view(std::string_view view_name);
        [[nodiscard]] view_scope begin_view(std::string_view view_name, texture_extent view_size);

        // The size view-relative textures resolve against outside view scopes, and in scopes begun without a size.
        // Kept across reset(); 1 × 1 until set.
        void set_view_size(texture_extent view_size) noexcept
        {
            default_view_size_ = view_size;
        }

        // The size view-relative textures created now resolve against
        texture_extent view_size() const noexcept
        {
            return open_view_.empty() ? default_view_size_ : open_view_size_;
        }

        // Starts a new frame: destroys the previous frame's passes, pass data and tables, resets the arena, and makes
        // the previous frame's handles stale
        void reset();

        // Culls, orders and schedules the passes, places barriers and transient memory (§6). No view scope may be open
        // (debug check).
        void compile();

        // The first step of execute (§7 step 1): prepares the pool's heap for the frame's context, acquires the placed
        // resources of the live transient resources and their views for this frame, and maps imports to their
        // handles. Fails with the pool's or the scheduler's error (transient_memory_exhausted when the frame's
        // descriptors run out) before anything is recorded. The bag lives in the arena until the next reset().
        result<const resource_bag*> materialise(const frame_token& frame);

        // The frame's build tables and the result of the last compile (nullptr before it); for the executor and tests
        const detail::render_graph_tables& tables() const noexcept
        {
            return *tables_;
        }

        const detail::compiled_graph* compiled() const noexcept
        {
            return compiled_;
        }

        const render_graph_configuration& configuration() const noexcept
        {
            return configuration_;
        }

        // Bytes the frame's tables, passes and pass data use (debug overlay)
        std::size_t arena_bytes_used() const noexcept
        {
            return arena_.bytes_used();
        }

    private:
        friend class render_pass_builder;
        friend class view_scope;

        template<typename PassData, typename Context, typename Setup, typename Execute>
        const PassData& add_pass(
            const std::string_view name,
            const pass_type type,
            const compute_queue_hint queue,
            Setup& setup,
            Execute&& execute)
        {
            using callable_type = std::decay_t<Execute>;

            PassData& data = arena_.create<PassData>();
            const callable_type& callable = arena_.create<callable_type>(std::forward<Execute>(execute));

            const std::uint32_t pass_index = begin_pass(name, type, queue);
            detail::render_pass_node& node = pass(pass_index);
            node.data = &data;
            node.callable = &callable;
            if constexpr (std::is_same_v<Context, graphics_pass_context>)
            {
                node.execute_graphics = &detail::invoke_execute<callable_type, PassData, Context>;
            }
            else if constexpr (std::is_same_v<Context, compute_pass_context>)
            {
                node.execute_compute = &detail::invoke_execute<callable_type, PassData, Context>;
            }
            else
            {
                node.execute_copy = &detail::invoke_execute<callable_type, PassData, Context>;
            }

            render_pass_builder builder{ *this, node, pass_index };
            std::invoke(setup, builder, data);
            return data;
        }

        // Appends a pass node with its qualified name; returns its index
        std::uint32_t begin_pass(std::string_view name, pass_type type, compute_queue_hint queue);
        detail::render_pass_node& pass(std::uint32_t index) noexcept;

        // The name in the arena, qualified with the open view scope
        std::string_view qualify(std::string_view name);
        void end_view() noexcept;
        void create_tables();

        rhi::resources resources_;
        rhi::command_lists command_lists_;
        rhi::queue<rhi::queue_type::graphics> graphics_queue_;
        rhi::queue<rhi::queue_type::compute> compute_queue_;
        rhi::queue<rhi::queue_type::copy> copy_queue_;
        transient_resource_pool& pool_;
        frame_scheduler& frames_;
        gpu_profiler* profiler_;
        render_graph_configuration configuration_;

        linear_arena arena_;
        detail::render_graph_tables* tables_ = nullptr; // in the arena
        detail::compiled_graph* compiled_ = nullptr; // in the arena
        std::uint32_t frame_serial_ = 1; // in the handles of debug builds; bumped by reset()
        std::string_view open_view_; // empty when no scope is open (view names aren't empty)
        texture_extent open_view_size_;
        texture_extent default_view_size_;
    };
} // namespace avis::graphics

#endif
