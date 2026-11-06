#ifndef AVIS_VISUALIZER_RENDERING_SCREENSHOT_CAPTURE_H
#define AVIS_VISUALIZER_RENDERING_SCREENSHOT_CAPTURE_H

// Screenshots of a view, taken inside the renderer (docs/design/11-renderer.md §12.1, docs/tasks/11-renderer.md
// §11.21).
//
// A request is either:
// - viewport: the next frame copies the view's ldr_color (after the overlays, or after the debug blits when
//   include_debug_blit is set) into a readback buffer;
// - custom: a temporary view named screenshot_view_name renders the view's camera at the requested size, with the
//   presentation state frozen at the request, screenshot_subframes jittered sub-frames over as many frames, and the
//   last frame copies its ldr_color into a readback buffer.
//
// Once the copying frame is complete on the GPU, a parallel worker encodes the pixels as a PNG with WIC (alpha forced
// to 255), adds tEXt chunks from the request's metadata, and writes the file with the IO layer's atomic replace. The
// renderer reports { id, path } or an error (scene_renderer::take_screenshot_results).
//
// Requests are latest-value and handled one at a time: a request made while one runs is queued, and replaces a queued
// one, which gets a "superseded" result. The next request starts once the previous one's file is written.

#include "rendering/view.h"

#include "avis/error.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/scene/snapshot.h"

#include <exec/async_scope.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    enum class screenshot_mode : std::uint8_t
    {
        viewport, // the view's image as it is rendered
        custom, // a temporary view at the requested size
    };

    // The largest custom screenshot (8K)
    inline constexpr std::uint32_t maximum_screenshot_width = 7680;
    inline constexpr std::uint32_t maximum_screenshot_height = 4320;

    // A custom screenshot is refused when its estimated transient memory exceeds this fraction of the free local video
    // memory budget (09 §8.2)
    inline constexpr double screenshot_budget_fraction = 0.5;

    // The temporary view of a custom screenshot; no other view of the frame may have this name while it renders
    inline constexpr std::string_view screenshot_view_name = "screenshot";

    // What the PNG's text chunks record, so a screenshot can be traced back to its run. Supplied by the caller; an
    // empty camera_pose is filled with the captured view's camera.
    struct screenshot_metadata
    {
        std::string scenario;
        std::uint64_t step = 0;
        double simulation_time = 0.0; // seconds
        std::string driver_id;
        std::string camera_pose;
        std::string render_settings; // as the caller writes them (for example the render section of visualizer.yaml)
    };

    struct screenshot_request
    {
        std::uint64_t id = 0; // the caller's, reported back
        screenshot_mode mode = screenshot_mode::viewport;
        std::uint32_t width = 0; // custom only
        std::uint32_t height = 0;
        bool include_debug_blit = false; // custom: the view's debug blits are drawn into the screenshot view too
        std::string view = "main"; // the view captured, or whose camera and exposure a custom screenshot uses
        std::filesystem::path path; // the file written; naming it and its folder is the caller's (11 §12.1)
        screenshot_metadata metadata;
    };

    struct screenshot_result
    {
        std::uint64_t id = 0;
        std::filesystem::path path;
        std::optional<error> failure; // none: the file was written
        graphics::texture_extent size{ 0, 0 }; // of the image; 0 × 0 when no frame captured it
        std::uint64_t first_frame_index = 0; // the frames that rendered it; 0 when none did
        std::uint64_t last_frame_index = 0;
    };

    // ---- PNG ----

    struct png_text
    {
        std::string keyword; // 1 to 79 Latin-1 characters, no leading, trailing or double spaces
        std::string text;
    };

    // The text chunks of `metadata`: Software, Scenario, Step, Simulation time, Driver, Camera and Render settings
    // (empty strings are left out)
    std::vector<png_text> screenshot_text(const screenshot_metadata& metadata);

    // `rgba` (RGBA8, rows `row_pitch` bytes apart) as a 32-bit BGRA PNG with every alpha 255, through WIC, with a tEXt
    // chunk per entry of `text` after IHDR. Fails for an empty image, short pixels or an invalid keyword.
    result<std::vector<std::byte>> encode_png(
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::byte> rgba,
        std::uint32_t row_pitch,
        std::span<const png_text> text);

    struct png_image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba; // RGBA8, tightly packed
    };

    // A PNG decoded by WIC into RGBA8, for tests and tools
    result<png_image> decode_png(std::span<const std::byte> png);

    // The tEXt chunks of a PNG, in file order; empty for bytes that aren't a PNG
    std::vector<png_text> read_png_text(std::span<const std::byte> png);

    // ---- Requests ----

    // The estimated transient memory of a custom screenshot's view: its graph textures without aliasing, the history,
    // the accumulation texture and the readback buffer
    std::uint64_t screenshot_transient_bytes(std::uint32_t width, std::uint32_t height) noexcept;

    // Why `request` is refused, or none: a viewport request without a path, a custom size of 0 or past 7680 × 4320, or
    // a custom request whose transient estimate exceeds screenshot_budget_fraction of the free budget
    std::optional<std::string>
        screenshot_refusal(const screenshot_request& request, std::optional<rhi::memory_budget> budget);

    // A request being taken (the renderer's view of it while it builds frames)
    struct screenshot_job
    {
        enum class phase : std::uint8_t
        {
            rendering, // frames still render or copy it
            reading, // the copying frame was built; waiting for it to complete on the GPU
            writing, // a worker encodes and writes the file
        };

        screenshot_request request;
        phase state = phase::rendering;
        graphics::texture_extent size{ 0, 0 }; // custom: the requested size; viewport: the view's, once copied
        std::uint64_t first_frame_index = 0;
        std::uint64_t last_frame_index = 0; // the frame that copies into `readback`
        std::uint32_t subframes = 0; // custom: the sub-frames built so far

        // Custom: the presentation state frozen at the first sub-frame (11 §12.1). The snapshot is a copy, kept until
        // the last sub-frame's frame has executed.
        std::optional<scene::snapshot> snapshot;
        std::chrono::steady_clock::time_point wall_time;
        rendering::camera camera;
        float device_pixel_ratio = 1.0f;
        std::vector<debug_selection> debug_blits; // the view's, with include_debug_blit
        rhi::texture_handle accumulation; // RGBA32F, the requested size

        rhi::buffer_handle readback; // the copy's destination; rows row_pitch bytes apart
        std::uint32_t row_pitch = 0;
        std::optional<graphics::submitted_frame> submitted; // of the copying frame

        bool custom() const noexcept
        {
            return request.mode == screenshot_mode::custom;
        }

        // The sub-frame the frame being built renders, and whether it is the last one
        bool last_subframe() const noexcept
        {
            return subframes + 1 >= gpu::screenshot_subframes;
        }
    };

    // The request queue, the job being taken, and the writes on parallel workers
    class screenshot_capture
    {
    public:
        screenshot_capture(rhi::resources resources, rhi::synchronization synchronization);

        // Waits for the writes in flight; releases the job's buffers (deferred by the RHI)
        ~screenshot_capture();

        screenshot_capture(const screenshot_capture&) = delete;
        screenshot_capture& operator=(const screenshot_capture&) = delete;

        // Queues `request`, replacing a queued one. A refused request (screenshot_refusal) gets its result at once.
        void request(screenshot_request request, std::optional<rhi::memory_budget> budget);

        // Called first when a frame is built: hands the finished job's GPU work to a worker, then starts the queued
        // request when no job runs. Returns the job when this frame renders it, else nullptr. Starting a custom job
        // freezes `snapshot`, `wall_now` and the camera of the request's view, and creates its accumulation texture
        // and readback buffer; a missing view or a failed creation fails the request instead.
        screenshot_job* begin_frame(
            std::uint64_t frame_index,
            const scene::snapshot& snapshot,
            std::chrono::steady_clock::time_point wall_now,
            std::span<const view_description> views);

        // The viewport job's readback buffer for a view of `size`, created when the view's frame is built
        result<void> create_viewport_readback(screenshot_job& job, graphics::texture_extent size);

        // The frame that built the job's copy was submitted
        void submitted(std::uint64_t frame_index, const graphics::submitted_frame& frame);

        // The frame failed to build or execute: a job that it rendered fails
        void frame_failed(std::uint64_t frame_index) noexcept;

        // Hands a job whose copy is complete to a worker, and notices a finished write
        void poll();

        // The results so far, in the order they were reported, each once
        std::vector<screenshot_result> take_results();

        // True while a job runs or a request is queued
        bool busy() const noexcept
        {
            return job_.has_value() || pending_.has_value();
        }

        const screenshot_job* job() const noexcept
        {
            return job_ ? &*job_ : nullptr;
        }

    private:
        void report(screenshot_result result);
        void fail(screenshot_job& job, error failure);
        void release(screenshot_job& job) noexcept;
        result<void> start(screenshot_job& job, std::span<const view_description> views);

        rhi::resources resources_;
        rhi::synchronization synchronization_;
        std::optional<screenshot_request> pending_;
        std::optional<screenshot_job> job_;
        std::atomic<bool> written_{ false }; // the job's write finished; its result is reported
        exec::async_scope writes_;

        std::mutex results_mutex_;
        std::vector<screenshot_result> results_;
    };
} // namespace avis::visualizer::rendering

#endif
