#ifndef AVIS_VISUALIZER_RENDERING_VIEW_H
#define AVIS_VISUALIZER_RENDERING_VIEW_H

// What the renderer draws one frame of: a camera, a size and an output (docs/design/11-renderer.md §2.1, 10 §8).
// Views come from the caller, the Visualizer's presentation layer; the renderer never reads a camera from the snapshot.

#include "avis/graphics/texture.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <Eigen/Geometry>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace avis::visualizer::rendering
{
    // A perspective camera. View space is right-handed: +X right, +Y up, looking down −Z (the scene's convention, 08
    // §4). Projections are reverse-Z: the near plane maps to depth 1, the far plane (or infinity) to 0.
    struct camera
    {
        Eigen::Affine3f world_from_view = Eigen::Affine3f::Identity();
        float vertical_field_of_view = 1.0471976f; // radians; 60°
        float near_plane = 0.1f; // metres
        std::optional<float> far_plane; // metres; none: an infinite far plane

        Eigen::Matrix4f view_from_world() const;

        // Clip from view, for width / height = aspect_ratio
        Eigen::Matrix4f projection(float aspect_ratio) const;
    };

    // The texture a view's image is copied into (present_to_shared, pass 16, 11 §12): an RGBA8_UNORM texture of the
    // view's size that allows copy_destination, for example a slot of 12's shared texture ring
    struct output_target
    {
        rhi::texture_handle texture;
        rhi::access_state initial = rhi::access_states::common; // its state when the frame starts
        rhi::access_state final = rhi::access_states::common; // the state the frame leaves it in (11 §12: common)
        std::optional<rhi::synchronization_point> ready_after; // when its previous reader is done with it
    };

    // How the debug blit shows a resource (11 §11). Each reads the texel under the pixel with Load, remaps it by the
    // selection's value range [range_minimum, range_maximum] to [0, 1] where a mode says so, and writes the result into
    // ldr_color as is: a value of 0.5 is byte 128, not sRGB-encoded.
    enum class debug_blit_mode : std::uint8_t
    {
        raw, // the selected channels, remapped; a single channel as grey
        depth_linear, // linear depth (a depth format is linearized), remapped as grey; nothing drawn is white
        normal_octahedral, // an octahedral RG normal, decoded: RGB = n · 0.5 + 0.5
        velocity, // RG velocity in UV: hue is the direction, value the length in texels over range_maximum
        ao, // R, remapped as grey
        cascade_index, // R as a cascade index: red, green, blue, yellow; anything else (no_cascade) black
        cluster_light_count, // cluster_lights only: the lights of each pixel's cluster, remapped as a heat map
        luminance_heat, // the Rec. 709 luminance of RGB, remapped as a heat map
        alpha_only, // A, remapped as grey
    };

    std::string_view to_string(debug_blit_mode mode) noexcept;

    // debug_selection::channels, for debug_blit_mode::raw
    inline constexpr std::uint8_t debug_channel_red = 1u << 0;
    inline constexpr std::uint8_t debug_channel_green = 1u << 1;
    inline constexpr std::uint8_t debug_channel_blue = 1u << 2;
    inline constexpr std::uint8_t debug_channel_alpha = 1u << 3;
    inline constexpr std::uint8_t debug_channels_all = 0xF;

    enum class view_corner : std::uint8_t
    {
        top_left,
        top_right,
        bottom_left,
        bottom_right,
    };

    // A debug blit in a corner of the view, `scale` of its size (at least 1 pixel), flush with the corner
    struct picture_in_picture
    {
        view_corner corner = view_corner::bottom_right;
        float scale = 0.25f; // in (0, 1]
    };

    // A resource to show in the debug blit (11 §11, docs/tasks/11-renderer.md §11.20): one of the view's textures (the
    // resource_capture names, plus shadow_map) or cluster_lights, with cluster_light_count. A selection the frame can't
    // show (an unknown name, another view's, a resource this frame doesn't render, a mip or slice out of range, a mode
    // that doesn't fit the resource) is skipped with a warning; the frame renders without it.
    struct debug_selection
    {
        // The view's resource, "gbuffer_normal", or qualified with the view, "main/gbuffer_normal"
        std::string resource;
        std::uint32_t mip = 0;
        std::uint32_t slice = 0; // the array slice (shadow_map's cascade)
        std::uint8_t channels = debug_channels_all; // debug_channel_*; raw only
        debug_blit_mode mode = debug_blit_mode::raw;
        float range_minimum = 0.0f;
        float range_maximum = 1.0f;
        std::optional<rendering::picture_in_picture> picture_in_picture; // none: fullscreen
    };

    // The pixel inspector (11 §11): the texel of view_options::debug_blits[selection] under the cursor, read back once
    // the frame is complete (scene_renderer::take_inspector_results). The cursor is in view pixels from the top-left
    // corner; it maps through the blit's rectangle into the resource's mip, as the blit shows it.
    struct inspector_request
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t selection = 0;
    };

    // A click in the view, in pixels from the top-left corner (11 §5.1, docs/tasks/11-renderer.md §11.19). Its frame
    // draws the pickable instances into picking_id and reads back the window around the click; the result comes from
    // scene_renderer::take_pick_results once the frame is complete on the GPU.
    struct pick_request
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };

    // A copy of one of the view's textures into a caller's texture at the end of the view's frame, for tests and tools
    // (docs/tasks/11-renderer.md §11.10). The texture must have the resource's format and the mip's size, allow
    // copy_destination, and be in `common`, where the frame leaves it. Resources: depth, linear_depth, depth_mips,
    // gbuffer_albedo, gbuffer_normal, gbuffer_material, gbuffer_emissive, velocity, hdr_color (11.11), cascade_index
    // (R8_UINT, the sun's shadow cascade of each pixel or gpu::no_cascade, 11.12), ao_raw, ao_edges, ao (11.13),
    // reactive_mask (11.14), taa_output (RGBA16F; hdr_color when TAA is off, 11.15), ldr_color, selection_mask (R8, on
    // a frame with a selected instance, 11.17), picking_id (R32G32_UINT, on a click frame, 11.19).
    struct resource_capture
    {
        std::string resource;
        std::uint32_t mip = 0;
        rhi::texture_handle texture;
    };

    // The same for one of the view's buffers (docs/tasks/11-renderer.md §11.11), copied whole into the start of
    // `buffer`, which must be at least as large and allow copy_destination (a readback buffer can be read once the
    // frame completes). Resources: cluster_lights, light_indices, exposure (11.16: the view's exposure buffer after the
    // frame's adaptation, its layout in shared_types.hlsli).
    struct buffer_capture
    {
        std::string resource;
        rhi::buffer_handle buffer;
    };

    struct view_options
    {
        bool taa = true; // with render.taa.enabled; off for views without history (11 §12.1)
        bool reset_history = false; // a camera cut or a scene reload: the view's history starts again (11 §10.2)
        // The debug blits, after the overlays (and picking) into ldr_color, in this order: later ones on top
        std::vector<debug_selection> debug_blits;
        std::optional<inspector_request> inspect;
        std::optional<pick_request> pick;
        std::optional<std::uint64_t> progressive_vertex_budget; // replaces render.progressive.vertex_budget (11 §9.3)
        std::vector<resource_capture> captures;
        std::vector<buffer_capture> buffer_captures;
    };

    struct view_description
    {
        std::string name; // the graph's view scope and the view_history key: non-empty, no '/', unique per frame
        rendering::camera camera;
        graphics::texture_extent size{ 1, 1 }; // pixels
        float device_pixel_ratio = 1.0f;
        std::optional<output_target> output; // none: nothing writes an import, so the graph culls the view's passes
        view_options options;
    };
} // namespace avis::visualizer::rendering

#endif
