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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace avis::visualizer::rendering
{
    // A history unused for this many frames is released (10 §8)
    inline constexpr std::uint64_t view_history_release_frames = 60;

    // The exposure buffer: the exposure, three spare floats, then the 128-bin luminance histogram (11 §10.3)
    inline constexpr std::uint32_t exposure_histogram_bins = 128;
    inline constexpr std::uint64_t exposure_buffer_size = 16 + exposure_histogram_bins * 4;

    struct view_history
    {
        graphics::texture_extent size; // the view size the textures have
        std::array<rhi::texture_handle, 2> taa_history{}; // RGBA16F: pre-exposed HDR colour after TAA (11 §10.2)
        std::array<rhi::texture_handle, 2> depth{}; // R32F: view-space linear depth, for TAA's disocclusion test
        rhi::buffer_handle exposure; // exposure_buffer_size bytes, raw; one buffer, read and then written each frame
        std::uint32_t current = 0; // the slot this frame writes; 1 − current holds the previous frame
        bool has_previous = false; // the previous slots and view_projection hold this view's previous frame
        Eigen::Matrix4f view_projection = Eigen::Matrix4f::Identity(); // unjittered, of the last frame rendered
        std::uint64_t jitter_sequence = 0; // the TAA jitter's next sample (11 §10.2); 0 when the history starts again
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
        // starts without a previous frame; otherwise the ping-pong slots swap and has_previous is set. Fails with the
        // RHI's error (nothing is kept then), or with invalid_argument when the name was already acquired this frame.
        // The pointer stays valid until release_unused or the store's destruction.
        result<view_history*> acquire(std::string_view name, graphics::texture_extent size, std::uint64_t frame_index);

        // Releases the histories not acquired in the last view_history_release_frames frames, counting `frame_index`
        void release_unused(std::uint64_t frame_index) noexcept;

        const view_history* find(std::string_view name) const noexcept;
        std::size_t size() const noexcept
        {
            return histories_.size();
        }

    private:
        result<void> create_resources(std::string_view name, graphics::texture_extent size, view_history& history);
        void destroy_resources(view_history& history) noexcept;

        rhi::resources resources_;
        std::map<std::string, view_history, std::less<>> histories_;
    };
} // namespace avis::visualizer::rendering

#endif
