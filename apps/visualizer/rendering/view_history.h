#ifndef AVIS_VISUALIZER_RENDERING_VIEW_HISTORY_H
#define AVIS_VISUALIZER_RENDERING_VIEW_HISTORY_H

// Per-view persistent state (docs/design/10-render-graph.md §8, 11 §10.2–§10.3): what a view keeps from one frame to
// the next, keyed by view name. The textures are ping-pong pairs: the frame writes slot `current` and reads the other,
// which holds the view's previous frame. The renderer imports them into the graph every frame.

#include "avis/error.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <Eigen/Core>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace avis::visualizer::rendering
{
    // A history unused for this many frames is released (10 §8)
    inline constexpr std::uint64_t view_history_release_frames = 60;

    struct view_history
    {
        graphics::texture_extent size; // the view size the textures have
        std::array<rhi::texture_handle, 2> taa_history{}; // RGBA16F: pre-exposed HDR colour after TAA (11 §10.2)
        std::array<rhi::texture_handle, 2> depth{}; // R32F: view-space linear depth, for TAA's disocclusion test
        // gpu::exposure_buffer_bytes, raw (layout in shared_types.hlsli); one buffer, read and then written each frame.
        // A resize keeps it, with the two fields below, so the exposure carries on.
        rhi::buffer_handle exposure;
        bool exposure_initialized = false; // the buffer holds an exposure; set by the renderer once it adds the reset
        // The wall time of the view's last exposure adaptation, for the next one's frame time; none before the first
        std::optional<std::chrono::steady_clock::time_point> last_adaptation;
        rhi::buffer_handle cluster_bounds; // cluster_count gpu::cluster_bounds (11 §6.3), structured
        // The unjittered projection the cluster bounds were built for; none before the first build. A projection change
        // rebuilds them (a resize makes a new history).
        std::optional<Eigen::Matrix4f> cluster_projection;
        std::uint32_t current = 0; // the slot this frame writes; 1 − current holds the previous frame
        bool has_previous = false; // the previous slots and view_projection hold this view's previous frame
        Eigen::Matrix4f view_projection = Eigen::Matrix4f::Identity(); // unjittered, of the last frame rendered
        std::uint64_t jitter_sequence = 0; // the TAA jitter's next sample (11 §10.2); 0 when the history starts again
        // The previous slot of taa_history holds a TAA resolve: the frame before ran taa_resolve. Set by the renderer.
        // Its pre-exposure is the exposure buffer's previous exposure.
        bool taa_written = false;
        std::uint64_t last_used_frame = 0;

        std::uint32_t previous() const noexcept
        {
            return 1 - current;
        }
    };

    class view_history_store
    {
    public:
        explicit view_history_store(rhi::resources resources) noexcept;
        ~view_history_store(); // destroys every history's resources (deferred by the RHI until the GPU is done)
        view_history_store(const view_history_store&) = delete;
        view_history_store& operator=(const view_history_store&) = delete;

        // The history of view `name` for frame `frame_index`. A new name, or a new size, creates the resources and
        // starts without a previous frame (a new size keeps the exposure); otherwise the ping-pong slots swap and
        // has_previous is set. Fails with the
        // RHI's error (nothing is kept then), or with invalid_argument when the name was already acquired this frame.
        // The pointer stays valid until release_unused or the store's destruction.
        result<view_history*> acquire(std::string_view name, graphics::texture_extent size, std::uint64_t frame_index);

        // Releases the histories not acquired in the last view_history_release_frames frames, counting `frame_index`
        void release_unused(std::uint64_t frame_index) noexcept;

        const view_history* find(std::string_view name) const noexcept;
        view_history* find(std::string_view name) noexcept;
        std::size_t size() const noexcept
        {
            return histories_.size();
        }

    private:
        // Creates the resources `created` doesn't have yet and adds it; on failure, destroys them and adds nothing
        result<view_history*> insert(
            std::string_view name, graphics::texture_extent size, std::uint64_t frame_index, view_history created);
        result<void> create_resources(std::string_view name, graphics::texture_extent size, view_history& history);
        void destroy_resources(view_history& history) noexcept;

        rhi::resources resources_;
        std::map<std::string, view_history, std::less<>> histories_;
    };
} // namespace avis::visualizer::rendering

#endif
