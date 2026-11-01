#include "rendering/scene_renderer.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/buffer.h"
#include "avis/graphics/error.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource.h"
#include "avis/graphics/resource_bag.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/texture.h"
#include "avis/graphics/view_scope.h"
#include "avis/rhi/formats.h"

#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        // The name the standard root signature is registered under; the pipeline cache matches pipelines to it
        constexpr std::string_view standard_signature_name = "standard";

        error invalid_view(const std::string_view name, const std::string_view message)
        {
            return graphics::make_error(
                graphics::error_kind::invalid_argument, std::format("view '{}': {}", name, message));
        }

        // The view's persistent resources in this frame's graph (10 §8): the slots this frame writes, the slots that
        // hold the previous frame, and the exposure buffer. Imported in and left in `common`.
        struct history_imports
        {
            graphics::texture taa_history_current;
            graphics::texture taa_history_previous;
            graphics::texture depth_current;
            graphics::texture depth_previous;
            graphics::buffer exposure;
        };

        history_imports import_history(graphics::render_graph& graph, const view_history& history)
        {
            const rhi::access_state common = rhi::access_states::common;
            return {
                .taa_history_current =
                    graph.import_texture("taa_history_current", history.taa_history[history.current], common, common),
                .taa_history_previous = graph.import_texture(
                    "taa_history_previous", history.taa_history[history.previous()], common, common),
                .depth_current =
                    graph.import_texture("depth_history_current", history.depth[history.current], common, common),
                .depth_previous =
                    graph.import_texture("depth_history_previous", history.depth[history.previous()], common, common),
                .exposure = graph.import_buffer("exposure", history.exposure, common, common),
            };
        }

        struct tonemap_data
        {
            graphics::texture ldr_color;
        };

        struct present_data
        {
            graphics::texture source;
            graphics::texture output;
        };
    } // namespace

    result<std::unique_ptr<scene_renderer>> scene_renderer::create(
        const renderer_dependencies& dependencies, const render_settings& settings, graphics::shader_library& shaders)
    {
        // The standard signature first: the pipeline cache tries candidates in the order they were added (11 §13)
        auto standard = dependencies.root_signatures.add(
            std::string{ standard_signature_name }, graphics::binding_layout::standard_description());
        if (!standard)
        {
            return std::unexpected{ std::move(standard).error() };
        }
        return std::unique_ptr<scene_renderer>{ new scene_renderer{ dependencies, settings, shaders, *standard } };
    }

    scene_renderer::scene_renderer(
        const renderer_dependencies& dependencies,
        const render_settings& settings,
        graphics::shader_library& shaders,
        const rhi::root_signature_handle standard_signature) :
        dependencies_{ dependencies },
        graph_{ dependencies.graph },
        settings_{ settings },
        shaders_{ shaders },
        standard_signature_{ standard_signature },
        histories_{ dependencies.resources }
    {}

    scene_renderer::~scene_renderer() = default;

    void scene_renderer::retire(const std::uint64_t completed_frame_index) noexcept
    {
        if (completed_frame_index > completed_frame_index_)
        {
            completed_frame_index_ = completed_frame_index;
        }
    }

    renderer_debug_information scene_renderer::debug_information() const
    {
        return {
            .graph = frames_rendered_ > 0 ? &graph_.debug_information() : nullptr,
            .frames_rendered = frames_rendered_,
            .last_frame_index = last_frame_index_,
            .completed_frame_index = completed_frame_index_,
            .last_publish_sequence = last_publish_sequence_,
            .views = last_view_count_,
            .view_histories = histories_.size(),
        };
    }

    std::optional<error> scene_renderer::build_frame(
        const graphics::frame_token& frame,
        const scene::snapshot& snapshot,
        const std::span<const view_description> views,
        [[maybe_unused]] const std::chrono::steady_clock::time_point wall_now)
    {
        graph_.reset();

        // Checked before anything is added: the graph's own checks on view names only assert
        std::set<std::string_view> names;
        for (const view_description& view : views)
        {
            if (auto invalid = check_view(view))
            {
                return invalid;
            }
            if (!names.insert(view.name).second)
            {
                return invalid_view(view.name, "a second view with this name in the frame");
            }
        }

        // Shared work, outside any view scope (10 §8): the frame data upload comes with extraction (11.5)

        for (const view_description& view : views)
        {
            if (auto failed = add_view(frame, view))
            {
                return failed;
            }
        }

        histories_.release_unused(frame.index());
        graph_.compile();

        ++frames_rendered_;
        last_frame_index_ = frame.index();
        last_publish_sequence_ = snapshot.publish_sequence;
        last_view_count_ = static_cast<std::uint32_t>(views.size());
        return std::nullopt;
    }

    std::optional<error> scene_renderer::check_view(const view_description& view) const
    {
        if (view.name.empty() || view.name.find('/') != std::string::npos)
        {
            return invalid_view(view.name, "a view name must be non-empty and contain no '/'");
        }
        if (view.size.width == 0 || view.size.height == 0)
        {
            return invalid_view(view.name, std::format("empty size {} x {}", view.size.width, view.size.height));
        }
        if (view.output)
        {
            if (!view.output->texture.is_valid())
            {
                return invalid_view(view.name, "the output texture is invalid");
            }
            const rhi::texture_description& output = dependencies_.resources.description(view.output->texture);
            if (output.format != rhi::format::rgba8_unorm || output.width != view.size.width ||
                output.height != view.size.height || !(output.usage & rhi::texture_usage::copy_destination))
            {
                return invalid_view(
                    view.name,
                    std::format(
                        "the output must be an rgba8_unorm texture of {} x {} that allows copy_destination, got {} "
                        "{} x {}",
                        view.size.width,
                        view.size.height,
                        rhi::to_string(output.format),
                        output.width,
                        output.height));
            }
        }
        return std::nullopt;
    }

    std::optional<error> scene_renderer::add_view(const graphics::frame_token& frame, const view_description& view)
    {
        auto history = histories_.acquire(view.name, view.size, frame.index());
        if (!history)
        {
            return std::move(history).error();
        }
        if (view.options.reset_history)
        {
            (*history)->has_previous = false;
        }

        // The camera of this frame, and the previous frame's view-projection from the history, for motion vectors
        // (11.10); a view without a previous frame has no motion
        const float aspect_ratio = static_cast<float>(view.size.width) / static_cast<float>(view.size.height);
        const Eigen::Matrix4f view_projection = view.camera.projection(aspect_ratio) * view.camera.view_from_world();
        [[maybe_unused]] const Eigen::Matrix4f view_projection_previous =
            (*history)->has_previous ? (*history)->view_projection : view_projection;
        (*history)->view_projection = view_projection;

        const graphics::view_scope scope = graph_.begin_view(view.name, view.size);
        [[maybe_unused]] const history_imports imported = import_history(graph_, **history);

        // Placeholder for the pass chain of 11 §2.3 (11.10 to 11.16): ldr_color is the background colour
        const tonemap_data& tonemap = graph_.add_graphics_pass<tonemap_data>(
            "tonemap",
            [](graphics::render_pass_builder& builder, tonemap_data& data)
            {
                graphics::texture_description description{ .size = graphics::view_relative(1.0f),
                                                           .format = rhi::format::rgba8_unorm };
                description.clear_value = rhi::clear_value{ .color = background_color };
                data.ldr_color = builder.write(
                    builder.create_texture("ldr_color", description), graphics::texture_write::render_target);
            },
            [](const tonemap_data& data,
               const graphics::resource_bag& resources,
               graphics::graphics_pass_context& context)
            {
                context.recorder().clear_render_target(resources.render_target_view(data.ldr_color), background_color);
            });

        // Pass 16 (11 §12): copies ldr_color into the view's output. Without one, nothing writes an import, and the
        // graph culls the view's passes.
        if (view.output)
        {
            const graphics::texture output = graph_.import_texture(
                "output", view.output->texture, view.output->initial, view.output->final, view.output->ready_after);
            graph_.add_copy_pass<present_data>(
                "present_to_shared",
                [&](graphics::render_pass_builder& builder, present_data& data)
                {
                    data.source = builder.read(tonemap.ldr_color, graphics::texture_read::copy_source);
                    data.output = builder.write(output, graphics::texture_write::copy_destination);
                },
                [](const present_data& data,
                   const graphics::resource_bag& resources,
                   graphics::copy_pass_context& context)
                {
                    context.recorder().copy_texture(
                        resources.texture_handle(data.output), {}, resources.texture_handle(data.source), {});
                });
        }
        return std::nullopt;
    }
} // namespace avis::visualizer::rendering
