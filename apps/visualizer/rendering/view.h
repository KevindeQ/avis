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

    // A resource to show in the debug blit (11 §11). The debug blit task (11.20) adds the rest of the selection.
    struct debug_selection
    {
        std::string resource; // qualified graph name, "main/gbuffer_normal"
        std::uint32_t mip = 0;
        std::uint32_t slice = 0;
    };

    // A click in the view, in pixels from the top-left corner (11 §5.1; handled by the picking task, 11.19)
    struct pick_request
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };

    struct view_options
    {
        bool taa = true; // with render.taa.enabled; off for views without history (11 §12.1)
        bool reset_history = false; // a camera cut or a scene reload: the view's history starts again (11 §10.2)
        std::optional<rendering::debug_selection> debug_selection;
        std::optional<pick_request> pick;
        std::optional<std::uint64_t> progressive_vertex_budget; // replaces render.progressive.vertex_budget (11 §9.3)
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
