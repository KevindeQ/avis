#include "rendering/scene_renderer.h"

#include "rendering/renderer_shaders.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/buffer.h"
#include "avis/graphics/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource.h"
#include "avis/graphics/resource_bag.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/texture.h"
#include "avis/graphics/upload_queue.h"
#include "avis/graphics/view_scope.h"
#include "avis/log.h"
#include "avis/rhi/formats.h"

#include <algorithm>
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

        constexpr std::string_view logger_name = "renderer";

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

        // The mip chain of one texture (11.4): mip 0 is copied into an RGBA8_UNORM scratch texture (sRGB formats
        // can't have UAVs), each mip is downsampled from the one before, then mips 1 and up are copied back
        struct mip_seed_data
        {
            graphics::texture source;
            graphics::texture scratch;
        };

        struct mip_step_data
        {
            graphics::texture scratch;
            std::uint32_t destination_mip = 1;
        };

        struct mip_store_data
        {
            graphics::texture scratch;
            graphics::texture target;
        };

        // generate_mips.hlsl's draw constants
        struct mip_constants
        {
            std::uint32_t source;
            std::uint32_t destination;
            std::uint32_t source_mip;
            std::uint32_t srgb;
            std::uint32_t source_width;
            std::uint32_t source_height;
            std::uint32_t destination_width;
            std::uint32_t destination_height;
        };

        constexpr std::uint32_t mip_extent(const std::uint32_t extent, const std::uint32_t mip) noexcept
        {
            return std::max(extent >> mip, 1u);
        }

        constexpr graphics::subresource_range one_mip(const std::uint32_t mip) noexcept
        {
            return { .first_mip = static_cast<std::uint16_t>(mip), .mip_count = 1 };
        }
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

        persistent_state state{ .standard_signature = *standard };

        auto generate_mips = dependencies.pipeline_cache.get_or_create(
            shaders,
            graphics::compute_pipeline_description{ .compute_shader =
                                                        graphics::make_shader_id(shader_ids::generate_mips),
                                                    .root_signature = *standard,
                                                    .debug_name = "generate_mips" });
        if (!generate_mips)
        {
            return std::unexpected{ std::move(generate_mips).error() };
        }
        state.generate_mips = *generate_mips;

        auto pipeline_variants =
            pipeline_variant_registry::create(dependencies.pipeline_cache, dependencies.root_signatures, shaders);
        if (!pipeline_variants)
        {
            return std::unexpected{ std::move(pipeline_variants).error() };
        }
        state.pipeline_variants = std::move(*pipeline_variants);

        auto arena = geometry_arena::create(dependencies.resources, split_geometry_arena(settings.geometry.arena_mib));
        if (!arena)
        {
            return std::unexpected{ std::move(arena).error() };
        }
        state.arena = std::move(*arena);

        // The defaults' uploads are queued now and flushed with the first frame
        auto textures =
            texture_residency::create(dependencies.resources, dependencies.uploads, dependencies.synchronization);
        if (!textures)
        {
            return std::unexpected{ std::move(textures).error() };
        }
        state.textures = std::move(*textures);

        auto materials = material_table::create(
            dependencies.resources, dependencies.uploads, dependencies.synchronization, *state.textures);
        if (!materials)
        {
            return std::unexpected{ std::move(materials).error() };
        }
        state.materials = std::move(*materials);

        return std::unique_ptr<scene_renderer>{ new scene_renderer{
            dependencies, settings, shaders, std::move(state) } };
    }

    scene_renderer::scene_renderer(
        const renderer_dependencies& dependencies,
        const render_settings& settings,
        graphics::shader_library& shaders,
        persistent_state state) :
        dependencies_{ dependencies },
        graph_{ dependencies.graph },
        settings_{ settings },
        shaders_{ shaders },
        standard_signature_{ state.standard_signature },
        histories_{ dependencies.resources },
        generate_mips_pipeline_{ state.generate_mips },
        pipeline_variants_{ std::move(state.pipeline_variants) },
        arena_{ std::move(state.arena) },
        residency_{ std::make_unique<residency_cache>(*arena_, dependencies.uploads, dependencies.synchronization) },
        textures_{ std::move(state.textures) },
        materials_{ std::move(state.materials) },
        frame_data_{ std::make_unique<frame_data_upload>(
            dependencies.resources, dependencies.frames.frames_in_flight()) }
    {}

    scene_renderer::~scene_renderer() = default;

    void scene_renderer::retire(const std::uint64_t completed_frame_index)
    {
        if (completed_frame_index > completed_frame_index_)
        {
            completed_frame_index_ = completed_frame_index;
        }
        residency_->retire(completed_frame_index_);
        materials_->retire(completed_frame_index_);
        textures_->retire(completed_frame_index_);
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
            .residency = residency_->statistics(),
            .textures = textures_->statistics(),
            .materials = materials_->statistics(),
            .extraction = { .alpha = extraction_.interpolation.alpha,
                            .reused = extraction_reused_,
                            .instances = extraction_.instances.size(),
                            .meshes = extraction_.meshes.size(),
                            .lights = extraction_.lights.size(),
                            .skipped_invisible = extraction_.skipped_invisible,
                            .skipped_not_resident = extraction_.skipped_not_resident,
                            .skipped_invalid = extraction_.skipped_invalid,
                            .dropped_lights = extraction_.dropped_lights },
            .frame_data = frame_data_->statistics(),
            .pipelines = pipeline_variants_->statistics(),
            .draw_lists =
                [this]
            {
                std::vector<draw_list_statistics> lists;
                lists.reserve(draw_lists_.size());
                for (const draw_list& list : draw_lists_)
                {
                    lists.push_back(list.statistics());
                }
                return lists;
            }(),
        };
    }

    bool scene_renderer::can_reuse_extraction(
        const scene::snapshot& snapshot, const frame_interpolation& interpolation) const
    {
        // The same snapshot at the same time, and arrays without motion: with motion, their world_previous would be
        // the frame before's, not the world they draw now. Residency must have nothing left to do, since it is
        // skipped too: no upload to retry, no mip chain to generate, and no material to rewrite, either for a pending
        // write or for a texture whose status changed after the material table's last update.
        if (!frame_data_->can_reuse() || extraction_.publish_sequence != snapshot.publish_sequence ||
            extraction_.interpolation.alpha != interpolation.alpha || !extraction_.stationary)
        {
            return false;
        }
        return residency_->statistics().pending_uploads == 0 && textures_->statistics().pending == 0 &&
               textures_->mip_requests().empty() && textures_->status_changes() == texture_changes_resolved_ &&
               materials_->statistics().pending_writes == 0;
    }

    std::optional<error> scene_renderer::build_frame(
        const graphics::frame_token& frame,
        const scene::snapshot& snapshot,
        const std::span<const view_description> views,
        const std::chrono::steady_clock::time_point wall_now)
    {
        graph_.reset();
        draw_lists_.clear(); // they were in the arena

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

        // The reuse shortcut (11 §3.5): the previous frame's arrays, residency and extraction skipped
        const frame_interpolation interpolation = interpolate(snapshot, wall_now);
        const bool reuse = can_reuse_extraction(snapshot, interpolation);

        if (!reuse)
        {
            // Residency (11 §3.6, D-103): queue the uploads of the snapshot's new mesh, texture and material versions
            // (materials after textures, which they resolve against), flush them on the copy queue (the graph records
            // none of them), then make the uploaded versions current. A failed flush keeps the copies queued for the
            // next frame; this one submits nothing.
            residency_->update(snapshot, frame.index());
            textures_->update(snapshot, frame.index());
            materials_->update(snapshot, frame.index());
            texture_changes_resolved_ = textures_->status_changes();
            if (auto flushed = dependencies_.uploads.flush(); !flushed)
            {
                return std::move(flushed).error();
            }
            residency_->after_flush();
            textures_->after_flush();
            materials_->after_flush();

            // Extraction (11 §3.5 steps 1 and 3): the instances of resident meshes, at this frame's time
            const extraction_lookup lookup{
                .mesh_version = [this](const scene::mesh_handle mesh) -> std::optional<std::uint64_t>
                {
                    const resident_mesh* resident = residency_->find(mesh);
                    return resident != nullptr ? std::optional{ resident->version } : std::nullopt;
                },
                .material_slot = [this](const scene::material_handle material) { return materials_->slot(material); },
            };
            extract(snapshot, wall_now, extraction_state_, lookup, extraction_);
        }
        extraction_reused_ = reuse;

        // The meshes the draw lists cull and key by: the versions extraction found resident. A reused frame draws the
        // same ones, since residency was idle.
        draw_meshes_.clear();
        for (const frame_mesh& mesh : extraction_.meshes)
        {
            const resident_mesh* resident = residency_->find(mesh.handle);
            if (resident == nullptr || resident->version != mesh.version)
            {
                draw_meshes_.push_back({}); // no submeshes: its instances get no record
                continue;
            }
            draw_meshes_.push_back(
                { .topology = resident->topology,
                  .attributes = resident->attributes,
                  .progressive = resident->progressive,
                  .primitives = resident->primitives });
        }

        // Shared work, outside any view scope (10 §8). The arena and the material table wait on the newest copies this
        // frame draws.
        const rhi::access_state common = rhi::access_states::common;
        const auto wait_for = [](const rhi::synchronization_point point)
        { return point.value != 0 ? std::optional{ point } : std::nullopt; };
        const std::optional<rhi::synchronization_point> geometry_ready = wait_for(residency_->frame_ready_after());
        geometry_ = {
            .vertices =
                graph_.import_buffer("geometry_vertices", arena_->vertex_buffer(), common, common, geometry_ready),
            .indices = graph_.import_buffer("geometry_indices", arena_->index_buffer(), common, common, geometry_ready),
        };
        frame_materials_ = {
            .table = graph_.import_buffer(
                "materials",
                materials_->buffer(),
                common,
                common,
                wait_for(rhi::latest(materials_->frame_ready_after(), textures_->frame_ready_after()))),
        };
        add_mip_passes();

        // Pass 0 (11 §2.3): the frame's arrays into their ring slots, on the copy queue. The instance indices come with
        // the range walker (11.9).
        auto uploaded = frame_data_->add_upload_pass(
            graph_,
            frame,
            std::max(completed_frame_index_, frame.completed_index()),
            extraction_.instances,
            extraction_.lights,
            {},
            reuse);
        if (!uploaded)
        {
            return std::move(uploaded).error();
        }
        frame_buffers_ = *uploaded;

        for (const view_description& view : views)
        {
            if (auto failed = add_view(frame, snapshot, view))
            {
                return failed;
            }
        }

        histories_.release_unused(frame.index());
        graph_.compile();
        textures_->mips_recorded(); // the chains are in this frame: the textures are current from the next one

        ++frames_rendered_;
        last_frame_index_ = frame.index();
        last_publish_sequence_ = snapshot.publish_sequence;
        last_view_count_ = static_cast<std::uint32_t>(views.size());
        return std::nullopt;
    }

    void scene_renderer::add_mip_passes()
    {
        const rhi::access_state common = rhi::access_states::common;
        const std::span<const mip_request> requests = textures_->mip_requests();
        for (std::size_t index = 0; index < requests.size(); ++index)
        {
            const mip_request& request = requests[index];
            const std::string prefix = std::format("generate_mips_{}", index);
            const graphics::texture target = graph_.import_texture(
                prefix + "_texture",
                request.texture,
                common,
                common,
                request.ready_after.value != 0 ? std::optional{ request.ready_after } : std::nullopt);

            // The scratch's other mips are written by the steps before anything reads them
            const mip_seed_data& seed = graph_.add_copy_pass<mip_seed_data>(
                prefix + "_seed",
                [&](graphics::render_pass_builder& builder, mip_seed_data& data)
                {
                    data.source = builder.read(target, graphics::texture_read::copy_source, one_mip(0));
                    const graphics::texture scratch = builder.create_texture(
                        prefix + "_scratch",
                        { .size = graphics::absolute(request.width, request.height),
                          .mip_levels = request.mip_levels,
                          .format = rhi::format::rgba8_unorm });
                    data.scratch = builder.write(scratch, graphics::texture_write::copy_destination);
                },
                [](const mip_seed_data& data,
                   const graphics::resource_bag& resources,
                   graphics::copy_pass_context& context)
                {
                    context.recorder().copy_texture(
                        resources.texture_handle(data.scratch), {}, resources.texture_handle(data.source), {});
                });

            graphics::texture scratch = seed.scratch;
            for (std::uint32_t mip = 1; mip < request.mip_levels; ++mip)
            {
                const mip_constants constants{ .source_mip = mip - 1,
                                               .srgb = request.srgb ? 1u : 0u,
                                               .source_width = mip_extent(request.width, mip - 1),
                                               .source_height = mip_extent(request.height, mip - 1),
                                               .destination_width = mip_extent(request.width, mip),
                                               .destination_height = mip_extent(request.height, mip) };
                scratch =
                    graph_
                        .add_compute_pass<mip_step_data>(
                            std::format("{}_mip{}", prefix, mip),
                            [&](graphics::render_pass_builder& builder, mip_step_data& data)
                            {
                                builder.read(scratch, graphics::texture_read::srv_compute, one_mip(mip - 1));
                                data.scratch = builder.write(scratch, graphics::texture_write::uav, one_mip(mip));
                                data.destination_mip = mip;
                            },
                            [pipeline = generate_mips_pipeline_, constants](
                                const mip_step_data& data,
                                const graphics::resource_bag& resources,
                                graphics::compute_pass_context& context)
                            {
                                mip_constants bound = constants;
                                bound.source = resources.srv(data.scratch).value;
                                bound.destination = resources.uav(data.scratch, data.destination_mip).value;
                                rhi::compute_recorder& recorder = context.recorder();
                                recorder.set_pipeline(pipeline);
                                graphics::binding_layout::push_draw_constants(recorder, bound);
                                recorder.dispatch(
                                    (constants.destination_width + 7) / 8, (constants.destination_height + 7) / 8, 1);
                            })
                        .scratch;
            }

            graph_.add_copy_pass<mip_store_data>(
                prefix + "_store",
                [&](graphics::render_pass_builder& builder, mip_store_data& data)
                {
                    const graphics::subresource_range chain{ .first_mip = 1,
                                                             .mip_count =
                                                                 static_cast<std::uint16_t>(request.mip_levels - 1) };
                    data.scratch = builder.read(scratch, graphics::texture_read::copy_source, chain);
                    data.target = builder.write(target, graphics::texture_write::copy_destination, chain);
                },
                [mips = request.mip_levels](
                    const mip_store_data& data,
                    const graphics::resource_bag& resources,
                    graphics::copy_pass_context& context)
                {
                    for (std::uint32_t mip = 1; mip < mips; ++mip)
                    {
                        const rhi::subresource subresource{ .mip = static_cast<std::uint16_t>(mip) };
                        context.recorder().copy_texture(
                            resources.texture_handle(data.target),
                            subresource,
                            resources.texture_handle(data.scratch),
                            subresource);
                    }
                });
        }
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

    draw_list scene_renderer::build_view_draw_list(const scene::snapshot& snapshot, const view_description& view)
    {
        // The material state of the slot extraction gave the instance: the material's current slot, or the default
        // material (lit, opaque, single-sided). A set that can't be created is tried again next frame.
        std::optional<std::string> failure; // the first pipeline set error
        const draw_list_sources sources{
            .snapshot = snapshot,
            .extraction = extraction_,
            .meshes = draw_meshes_,
            .cascades = {}, // the shadow cascades come with 11.12
            .commands = {},
            .lookup = { .material =
                            [this](const scene::material_handle material)
                        {
                            const resident_material* resident = materials_->find(material);
                            return resident != nullptr ? draw_material_of(resident->data) : draw_material{};
                        },
                        .pipeline_set =
                            [this, &failure](const pipeline_set_key& key)
                        {
                            auto found = pipeline_variants_->find(key);
                            if (!found && !failure)
                            {
                                failure = found.error().message;
                            }
                            return found;
                        } },
            .opaque_depth_bands = settings_.draw_sorting.opaque_depth_bands,
        };
        draw_list list = build_draw_list(view, sources, graph_.arena());

        if (const std::size_t skipped = list.statistics().skipped_no_pipeline; skipped > 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!last_pipeline_warning_ || now - *last_pipeline_warning_ >= std::chrono::seconds{ 1 })
            {
                last_pipeline_warning_ = now;
                log::get(logger_name)
                    ->warn(
                        "view '{}': {} instances have no pipeline set and aren't drawn: {}",
                        view.name,
                        skipped,
                        failure.value_or("unknown error"));
            }
        }
        return list;
    }

    std::optional<error> scene_renderer::add_view(
        const graphics::frame_token& frame, const scene::snapshot& snapshot, const view_description& view)
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

        // Culled, keyed and sorted (11 §4.5); the passes walk its ranges (11.9 on)
        draw_lists_.push_back(build_view_draw_list(snapshot, view));

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
