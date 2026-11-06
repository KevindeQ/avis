#include "rendering/scene_renderer.h"

#include "rendering/blue_noise.h"
#include "rendering/passes/depth_passes.h"
#include "rendering/passes/exposure_passes.h"
#include "rendering/passes/forward_pass.h"
#include "rendering/passes/gbuffer_pass.h"
#include "rendering/passes/gtao_pass.h"
#include "rendering/passes/lighting_passes.h"
#include "rendering/passes/scene_range_pass.h"
#include "rendering/passes/shadow_pass.h"
#include "rendering/passes/taa_pass.h"
#include "rendering/renderer_shaders.h"
#include "rendering/view_constants.h"

#include "avis/detail/check.h"
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

#include <Eigen/LU>

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

        struct capture_data
        {
            graphics::texture source;
            graphics::texture destination;
            std::uint32_t mip = 0;
        };

        // A view texture resource_capture may name
        struct capturable
        {
            std::string_view name;
            graphics::texture texture;
            rhi::format format;
            std::uint32_t mips = 1;
            graphics::texture_extent size{ 0, 0 }; // of mip 0; 0 × 0: the view's
        };

        struct buffer_capture_data
        {
            graphics::buffer source;
            graphics::buffer destination;
        };

        result<rhi::compute_pipeline_handle> create_compute(
            graphics::pipeline_cache& cache,
            graphics::shader_library& shaders,
            const rhi::root_signature_handle signature,
            const std::string_view id)
        {
            return cache.get_or_create(
                shaders,
                graphics::compute_pipeline_description{ .compute_shader = graphics::make_shader_id(id),
                                                        .root_signature = signature,
                                                        .debug_name = std::string{ id.substr(0, id.find('.')) } });
        }

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
        environment_pipelines environment;

        const std::array computes{
            std::pair{ &state.generate_mips, shader_ids::generate_mips },
            std::pair{ &state.linearize_depth, shader_ids::linearize_depth },
            std::pair{ &state.depth_mips, shader_ids::depth_mips },
            std::pair{ &state.cluster_build, shader_ids::cluster_build },
            std::pair{ &state.light_cull, shader_ids::light_cull },
            std::pair{ &state.gtao, shader_ids::gtao },
            std::pair{ &state.gtao_denoise, shader_ids::gtao_denoise },
            std::pair{ &state.deferred_lighting, shader_ids::deferred_lighting },
            std::pair{ &state.taa, shader_ids::taa },
            std::pair{ &state.exposure_histogram, shader_ids::exposure_histogram },
            std::pair{ &state.exposure_adapt, shader_ids::exposure_adapt },
            std::pair{ &state.tonemap, shader_ids::tonemap },
            std::pair{ &state.selection_outline, shader_ids::selection_outline },
            std::pair{ &environment.sky, shader_ids::environment_sky },
            std::pair{ &environment.prefilter, shader_ids::environment_prefilter },
            std::pair{ &environment.irradiance, shader_ids::environment_irradiance },
            std::pair{ &environment.brdf_lut, shader_ids::brdf_lut },
        };
        for (const auto& [pipeline, id] : computes)
        {
            auto created = create_compute(dependencies.pipeline_cache, shaders, *standard, id);
            if (!created)
            {
                return std::unexpected{ std::move(created).error() };
            }
            *pipeline = *created;
        }

        auto sky = dependencies.pipeline_cache.get_or_create(
            shaders,
            graphics::graphics_pipeline_description{ .vertex_shader = graphics::make_shader_id(shader_ids::sky_vertex),
                                                     .pixel_shader = graphics::make_shader_id(shader_ids::sky_pixel),
                                                     .state = sky_pipeline_state(),
                                                     .root_signature = *standard,
                                                     .debug_name = "sky" });
        if (!sky)
        {
            return std::unexpected{ std::move(sky).error() };
        }
        state.sky = *sky;

        auto baker = environment_baker::create(dependencies.resources, environment);
        if (!baker)
        {
            return std::unexpected{ std::move(baker).error() };
        }
        state.environment = std::move(*baker);

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

        // The tonemap's dither (11 §10.4), uploaded with the first frame's flush like the default textures
        rhi::resources resources = dependencies.resources;
        auto blue_noise = resources.create_texture(
            { .width = gpu::blue_noise_size,
              .height = gpu::blue_noise_size,
              .format = rhi::format::r8_unorm,
              .usage = rhi::texture_usage::shader_resource | rhi::texture_usage::copy_destination,
              .initial_layout = rhi::texture_layout::common,
              .debug_name = "blue_noise" });
        if (!blue_noise)
        {
            return std::unexpected{ std::move(blue_noise).error() };
        }
        state.blue_noise = *blue_noise;
        const graphics::subresource_data texels{ .bytes = std::as_bytes(blue_noise_texels()),
                                                 .row_pitch = gpu::blue_noise_size };
        auto upload = dependencies.uploads.upload_texture(state.blue_noise, std::span{ &texels, 1 });
        if (!upload)
        {
            resources.destroy(state.blue_noise);
            return std::unexpected{ std::move(upload).error() };
        }
        state.blue_noise_upload = *upload;

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
        ao_parameters_{ settings.ao },
        exposure_parameters_{ settings.exposure },
        tonemapper_{ settings.tonemapper },
        shaders_{ shaders },
        standard_signature_{ state.standard_signature },
        histories_{ dependencies.resources },
        generate_mips_pipeline_{ state.generate_mips },
        linearize_depth_pipeline_{ state.linearize_depth },
        depth_mips_pipeline_{ state.depth_mips },
        cluster_build_pipeline_{ state.cluster_build },
        light_cull_pipeline_{ state.light_cull },
        gtao_pipeline_{ state.gtao },
        gtao_denoise_pipeline_{ state.gtao_denoise },
        deferred_lighting_pipeline_{ state.deferred_lighting },
        sky_pipeline_{ state.sky },
        taa_pipeline_{ state.taa },
        exposure_histogram_pipeline_{ state.exposure_histogram },
        exposure_adapt_pipeline_{ state.exposure_adapt },
        tonemap_pipeline_{ state.tonemap },
        selection_outline_pipeline_{ state.selection_outline },
        blue_noise_texture_{ state.blue_noise },
        blue_noise_upload_{ state.blue_noise_upload },
        environment_{ std::move(state.environment) },
        pipeline_variants_{ std::move(state.pipeline_variants) },
        arena_{ std::move(state.arena) },
        residency_{ std::make_unique<residency_cache>(*arena_, dependencies.uploads, dependencies.synchronization) },
        textures_{ std::move(state.textures) },
        materials_{ std::move(state.materials) },
        frame_data_{ std::make_unique<frame_data_upload>(
            dependencies.resources, dependencies.frames.frames_in_flight()) }
    {}

    scene_renderer::~scene_renderer()
    {
        dependencies_.resources.destroy(blue_noise_texture_); // deferred by the RHI until the GPU is done with it
    }

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
            .draw_runs =
                [this]
            {
                std::vector<std::size_t> runs;
                runs.reserve(prepared_draw_lists_.size());
                for (const prepared_draw_list& list : prepared_draw_lists_)
                {
                    runs.push_back(list.runs.size());
                }
                return runs;
            }(),
            .instance_indices = instance_indices_.size(),
            .progressive = progressive_statistics_,
            .progressive_vertices =
                [this]
            {
                std::vector<std::uint64_t> vertices;
                vertices.reserve(prepared_draw_lists_.size());
                for (const prepared_draw_list& list : prepared_draw_lists_)
                {
                    vertices.push_back(progressive_vertices(list));
                }
                return vertices;
            }(),
            .environment = environment_->statistics(),
        };
    }

    void scene_renderer::forget_frame() noexcept
    {
        frame_data_->forget();
        environment_->forget();
        for (const std::string& name : rebuilt_clusters_)
        {
            if (view_history* history = histories_.find(name))
            {
                history->cluster_projection.reset();
            }
        }
        rebuilt_clusters_.clear();
        for (const std::string& name : resolved_views_)
        {
            if (view_history* history = histories_.find(name))
            {
                history->taa_written = false;
            }
        }
        resolved_views_.clear();
        for (const std::string& name : reset_exposures_)
        {
            if (view_history* history = histories_.find(name))
            {
                history->exposure_initialized = false;
            }
        }
        reset_exposures_.clear();
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
        rebuilt_clusters_.clear();
        resolved_views_.clear();
        reset_exposures_.clear();
        view_cascades_.clear();
        draw_lists_.clear(); // they were in the arena
        prepared_draw_lists_.clear();
        progressive_statistics_.clear();
        instance_indices_.clear();

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
            if (blue_noise_upload_)
            {
                if (const std::optional<rhi::synchronization_point> point =
                        dependencies_.uploads.point(*blue_noise_upload_))
                {
                    blue_noise_ready_ = *point;
                    blue_noise_upload_.reset();
                }
            }

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
                  .primitives = resident->primitives,
                  .range = resident->range });
        }

        // Each view's draw list, culled, keyed and sorted (11 §4.5), then cut into runs whose instance indices pass 0
        // uploads (11.9): a serial prefix pass, so the passes' recording groups don't race
        view_cascades_.reserve(views.size());
        draw_lists_.reserve(views.size());
        for (const view_description& view : views)
        {
            view_cascades_.push_back(compute_view_cascades(view));
            draw_lists_.push_back(build_view_draw_list(snapshot, view, view_cascades_.back()));
        }
        // The progressive LOD (11 §9.3) of each view, rate-limited against the view's previous frame, gives the runs of
        // progressive meshes their vertex counts
        std::erase_if(
            progressive_lods_,
            [&names](const auto& entry) { return !names.contains(std::string_view{ entry.first }); });
        prepared_draw_lists_.reserve(draw_lists_.size());
        progressive_statistics_.reserve(draw_lists_.size());
        for (std::size_t index = 0; index < draw_lists_.size(); ++index)
        {
            const view_description& view = views[index];
            auto state = progressive_lods_.find(view.name);
            if (state == progressive_lods_.end())
            {
                state = progressive_lods_.emplace(view.name, progressive_lod{}).first;
            }
            if (view.options.reset_history)
            {
                state->second.reset();
            }
            const progressive_lod_parameters parameters{
                .vertex_budget = view.options.progressive_vertex_budget.value_or(settings_.progressive.vertex_budget),
                .shadow_lod_scale = settings_.progressive.shadow_lod_scale,
            };
            const progressive_draw_counts lod = compute_progressive_lod(
                view,
                draw_lists_[index],
                draw_meshes_,
                extraction_.instances,
                parameters,
                state->second,
                graph_.arena());
            progressive_statistics_.push_back(lod.statistics);
            prepared_draw_lists_.push_back(
                prepare_draw_runs(draw_lists_[index], draw_meshes_, graph_.arena(), instance_indices_, lod.by_record));
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
        AVIS_CHECK(
            logger_name,
            !blue_noise_upload_,
            "the blue-noise texture's upload ticket is unresolved: the first frame flushes the upload queue (11 §3.6)");
        blue_noise_ =
            graph_.import_texture("blue_noise", blue_noise_texture_, common, common, wait_for(blue_noise_ready_));
        add_mip_passes();

        // The baked environment (11 §6.4). The bake passes write imported textures, which the graph never culls: they
        // are only added when a view writes something.
        const bool any_view_writes = std::ranges::any_of(
            views,
            [](const view_description& view)
            { return view.output || !view.options.captures.empty() || !view.options.buffer_captures.empty(); });
        environment_resources_ = environment_->add_passes(
            graph_,
            environment_description{ .settings = snapshot.environment, .uniform_radiance = environment_override_ },
            any_view_writes);
        ambient_intensity_ = snapshot.environment.ambient_intensity;

        // Pass 0 (11 §2.3): the frame's arrays into their ring slots, on the copy queue. A reused frame copies only the
        // instance indices, which follow the camera.
        auto uploaded = frame_data_->add_upload_pass(
            graph_,
            frame,
            std::max(completed_frame_index_, frame.completed_index()),
            extraction_.instances,
            extraction_.lights,
            instance_indices_,
            reuse);
        if (!uploaded)
        {
            return std::move(uploaded).error();
        }
        frame_buffers_ = *uploaded;

        for (std::size_t index = 0; index < views.size(); ++index)
        {
            if (auto failed = add_view(frame, views[index], index, wall_now))
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
        for (const resource_capture& capture : view.options.captures)
        {
            if (!capture.texture.is_valid())
            {
                return invalid_view(view.name, std::format("the capture of '{}' has no texture", capture.resource));
            }
        }
        for (const buffer_capture& capture : view.options.buffer_captures)
        {
            if (!capture.buffer.is_valid())
            {
                return invalid_view(view.name, std::format("the capture of '{}' has no buffer", capture.resource));
            }
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

    shadow_cascades scene_renderer::compute_view_cascades(const view_description& view) const
    {
        if (!extraction_.sun || *extraction_.sun >= extraction_.lights.size())
        {
            return {};
        }
        const gpu::light_data& sun = extraction_.lights[*extraction_.sun];
        if (sun.casts_shadows == 0)
        {
            return {};
        }
        const float aspect_ratio = static_cast<float>(view.size.width) / static_cast<float>(view.size.height);
        return compute_cascades(
            view.camera,
            aspect_ratio,
            { sun.direction.x, sun.direction.y, sun.direction.z },
            cascade_parameters_of(settings_.shadows));
    }

    draw_list scene_renderer::build_view_draw_list(
        const scene::snapshot& snapshot, const view_description& view, const shadow_cascades& cascades)
    {
        // The material state of the slot extraction gave the instance: the material's current slot, or the default
        // material (lit, opaque, single-sided). A set that can't be created is tried again next frame.
        std::optional<std::string> failure; // the first pipeline set error
        const draw_list_sources sources{
            .snapshot = snapshot,
            .extraction = extraction_,
            .meshes = draw_meshes_,
            .cascades = cascades.used_viewpoints(),
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
        const graphics::frame_token& frame,
        const view_description& view,
        const std::size_t index,
        const std::chrono::steady_clock::time_point wall_now)
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
        if (!(*history)->has_previous)
        {
            (*history)->jitter_sequence = 0;
        }
        // TAA accumulates onto the previous frame's resolve, if there was one (11 §10.2)
        const bool taa_history = (*history)->has_previous && (*history)->taa_written;

        // The camera of this frame, with the TAA jitter when TAA is on for the view, and the previous frame's
        // unjittered view-projection from the history, for motion vectors; a view without a previous frame has none
        const Eigen::Matrix4f view_projection = unjittered_view_projection(view);
        const bool taa = settings_.taa.enabled && view.options.taa;
        const view_frame camera_frame{
            .view = view,
            .view_projection_previous = (*history)->has_previous ? (*history)->view_projection : view_projection,
            .jitter = taa ? jitter_pixels((*history)->jitter_sequence) : Eigen::Vector2f::Zero(),
            .frame_index = static_cast<std::uint32_t>(frame.index()),
        };
        (*history)->view_projection = view_projection;
        ++(*history)->jitter_sequence;

        const graphics::view_scope scope = graph_.begin_view(view.name, view.size);
        const history_imports imported = import_history(graph_, **history);

        // A view that writes nothing has its passes culled, unless one writes its history
        const bool writes = view.output || !view.options.captures.empty() || !view.options.buffer_captures.empty();

        // The exposure buffer holds an exposure from the view's first frame on (11.16): the reset comes before any
        // pass reads it. A view that writes nothing adds no exposure pass, since they write its history.
        graphics::buffer exposure = imported.exposure;
        if (writes && !(*history)->exposure_initialized)
        {
            exposure = add_exposure_reset(
                graph_, exposure_adapt_pipeline_, exposure, manual_exposure(exposure_parameters_.ev100));
            (*history)->exposure_initialized = true;
            reset_exposures_.push_back(view.name);
        }

        const scene_pass_inputs inputs{
            .list = prepared_draw_lists_[index],
            .meshes = draw_meshes_,
            .pipelines = *pipeline_variants_,
            .frame = frame_buffers_,
            .geometry = geometry_,
            .materials = frame_materials_,
            .view_constants = make_view_constants(camera_frame),
            .split = { .passes_per_group = graph_.configuration().passes_per_group_without_timings },
        };

        // Passes 1 and 2 (11.10)
        const graphics::texture depth = add_depth_prepass(graph_, inputs);
        const std::uint32_t depth_mips = depth_mip_levels(view.size.width, view.size.height);
        const linearized_depth linearized = add_linearize_depth(
            graph_,
            depth,
            { .linearize = linearize_depth_pipeline_, .reduce = depth_mips_pipeline_ },
            inputs.view_constants,
            view.size);

        // Pass 3 on async compute, declared before the G-buffer so it overlaps it (11 §2.3). The bounds are rebuilt
        // when the projection changed; a view that writes nothing doesn't, since the build writes its history.
        const Eigen::Matrix4f projection =
            view.camera.projection(static_cast<float>(view.size.width) / static_cast<float>(view.size.height));
        const bool rebuild_clusters =
            writes && (!(*history)->cluster_projection || *(*history)->cluster_projection != projection);
        if (rebuild_clusters)
        {
            (*history)->cluster_projection = projection;
            rebuilt_clusters_.push_back(view.name);
        }
        const light_clusters clusters = add_light_clusters(
            graph_,
            { .build = cluster_build_pipeline_,
              .cull = light_cull_pipeline_,
              .bounds = (*history)->cluster_bounds,
              .rebuild = rebuild_clusters,
              .projection_inverse = projection.inverse(),
              .size = view.size,
              .frame = frame_buffers_,
              .depth_mips = linearized.depth_mips,
              .depth_mip_count = depth_mips,
              .view_constants = inputs.view_constants,
              .keep_front = !inputs.list.list->range(forward_range()).empty() });

        // Pass 4 (11.10); passes 5 and 6 on async compute, declared after the G-buffer and before the shadow cascades
        // (11.12), so they overlap them (11 §2.3); then lighting and the sky (11.11), the transparents (11.14), TAA
        // (11.15), exposure and the tonemap (11.16)
        const gbuffer_targets gbuffer = add_gbuffer_pass(graph_, inputs, depth);
        gtao_outputs ambient_occlusion;
        if (ao_parameters_.intensity > 0.0f)
        {
            ambient_occlusion = add_gtao(
                graph_,
                { .pipelines = { .gtao = gtao_pipeline_, .denoise = gtao_denoise_pipeline_ },
                  .depth_mips = linearized.depth_mips,
                  .depth_mip_count = depth_mips,
                  .normal = gbuffer.normal,
                  .view_constants = inputs.view_constants,
                  .parameters = ao_parameters_ });
        }
        const shadow_cascades& cascades = view_cascades_[index];
        graphics::texture shadow_map;
        if (cascades.count > 0)
        {
            shadow_map = add_shadow_cascades(graph_, inputs, cascades, static_cast<std::uint32_t>(frame.index()));
        }
        const bool wants_cascade_index = std::ranges::any_of(
            view.options.captures, [](const resource_capture& capture) { return capture.resource == "cascade_index"; });
        const deferred_lighting_outputs lit = add_deferred_lighting(
            graph_,
            { .pipeline = deferred_lighting_pipeline_,
              .gbuffer = gbuffer,
              .depth = gbuffer.depth,
              .linear_depth = linearized.linear_depth,
              .clusters = clusters,
              .frame = frame_buffers_,
              .sun = extraction_.sun.value_or(gpu::no_light),
              .environment = environment_resources_,
              .exposure = exposure,
              .ambient_intensity = ambient_intensity_,
              .view_constants = inputs.view_constants,
              .shadow_map = shadow_map,
              .ambient_occlusion = ambient_occlusion.ao,
              .ambient_occlusion_scale = ambient_occlusion.scale,
              .shadow = make_shadow_constants(cascades, 0),
              .cascade_index = wants_cascade_index });
        const sky_outputs sky = add_sky_pass(
            graph_,
            { .pipeline = sky_pipeline_,
              .hdr_color = lit.hdr_color,
              .depth = gbuffer.depth,
              .sky = environment_->sky(),
              .exposure = exposure,
              .view_constants = inputs.view_constants });
        const forward_outputs transparent = add_forward_transparent(
            graph_,
            inputs,
            { .hdr_color = sky.hdr_color,
              .depth = sky.depth,
              .clusters = clusters,
              .sun = extraction_.sun.value_or(gpu::no_light),
              .environment = environment_resources_,
              .exposure = exposure,
              .ambient_intensity = ambient_intensity_,
              .shadow_map = shadow_map,
              .shadow = make_shadow_constants(cascades, 0) });
        // Without TAA, taa_output is hdr_color, and the history isn't written
        graphics::texture taa_output = transparent.hdr_color;
        const bool resolve = taa && writes;
        if (resolve)
        {
            taa_output = add_taa_resolve(
                             graph_,
                             { .pipeline = taa_pipeline_,
                               .hdr_color = transparent.hdr_color,
                               .velocity = gbuffer.velocity,
                               .linear_depth = linearized.linear_depth,
                               .reactive_mask = transparent.reactive_mask,
                               .history = imported.taa_history_current,
                               .depth_history = imported.depth_current,
                               .previous_history = imported.taa_history_previous,
                               .previous_depth_history = imported.depth_previous,
                               .has_history = taa_history,
                               .exposure = exposure,
                               .view_constants = inputs.view_constants })
                             .output;
            resolved_views_.push_back(view.name);
        }
        (*history)->taa_written = resolve;

        // Passes 12 and 13: the next frame's exposure from taa_output, timed by the wall time since the view's last
        // adaptation, then the tonemap, rescaled to that exposure
        graphics::buffer adapted = exposure;
        if (writes)
        {
            const std::optional<std::chrono::steady_clock::time_point> last = (*history)->last_adaptation;
            const float frame_time =
                last ? std::clamp(
                           std::chrono::duration<float>(wall_now - *last).count(), 0.0f, exposure_maximum_frame_time) :
                       0.0f;
            (*history)->last_adaptation = wall_now;
            adapted = add_exposure_adaptation(
                graph_,
                { .histogram = exposure_histogram_pipeline_,
                  .adapt = exposure_adapt_pipeline_,
                  .color = taa_output,
                  .exposure = exposure,
                  .settings = exposure_parameters_,
                  .frame_time = frame_time });
        }
        const graphics::texture tonemapped = add_tonemap(
            graph_,
            { .pipeline = tonemap_pipeline_,
              .color = taa_output,
              .exposure = adapted,
              .blue_noise = blue_noise_,
              .tonemapper = tonemapper_ });

        // Pass 14 (11.17): the selection outline, then the overlays, unlit, into ldr_color
        const overlay_outputs overlays = add_overlay(
            graph_,
            inputs,
            { .outline = selection_outline_pipeline_,
              .ldr_color = tonemapped,
              .depth = transparent.depth,
              .selection_color = selection_color_ });
        const graphics::texture ldr_color = overlays.ldr_color;

        // Captures, for tests and tools: copies of the final versions
        const graphics::texture_extent ao_size = gtao_extent(ao_parameters_, view.size);
        const std::array resources{
            capturable{ "depth", overlays.depth, rhi::format::d32_float },
            capturable{ "linear_depth", linearized.linear_depth, rhi::format::r32_float },
            capturable{ "depth_mips", linearized.depth_mips, rhi::format::rg32_float, depth_mips },
            capturable{ "gbuffer_albedo", gbuffer.albedo, rhi::format::rgba8_unorm_srgb },
            capturable{ "gbuffer_normal", gbuffer.normal, rhi::format::rg16_unorm },
            capturable{ "gbuffer_material", gbuffer.material, rhi::format::rgba8_unorm },
            capturable{ "gbuffer_emissive", gbuffer.emissive, rhi::format::r11g11b10_float },
            capturable{ "velocity", gbuffer.velocity, rhi::format::rg16_float },
            capturable{ "ao_raw", ambient_occlusion.raw, rhi::format::r8_unorm, 1, ao_size },
            capturable{ "ao_edges", ambient_occlusion.edges, rhi::format::r8_unorm, 1, ao_size },
            capturable{ "ao", ambient_occlusion.ao, rhi::format::r8_unorm, 1, ao_size },
            capturable{ "hdr_color", transparent.hdr_color, rhi::format::rgba16_float },
            capturable{ "reactive_mask", transparent.reactive_mask, rhi::format::r8_unorm },
            capturable{ "taa_output", taa_output, rhi::format::rgba16_float },
            capturable{ "cascade_index", lit.cascade_index, rhi::format::r8_uint },
            capturable{ "selection_mask", overlays.selection_mask, rhi::format::r8_unorm },
            capturable{ "ldr_color", ldr_color, rhi::format::rgba8_unorm },
        };
        for (std::size_t capture_index = 0; capture_index < view.options.captures.size(); ++capture_index)
        {
            const resource_capture& capture = view.options.captures[capture_index];
            const auto found = std::ranges::find(resources, std::string_view{ capture.resource }, &capturable::name);
            if (found == resources.end())
            {
                return invalid_view(view.name, std::format("no resource '{}' to capture", capture.resource));
            }
            const rhi::texture_description& target = dependencies_.resources.description(capture.texture);
            if (!found->texture.valid())
            {
                return invalid_view(view.name, std::format("'{}' isn't rendered this frame", capture.resource));
            }
            const graphics::texture_extent base = found->size.width != 0 ? found->size : view.size;
            const std::uint32_t width = std::max(base.width >> capture.mip, 1u);
            const std::uint32_t height = std::max(base.height >> capture.mip, 1u);
            if (capture.mip >= found->mips || target.format != found->format || target.width != width ||
                target.height != height || !(target.usage & rhi::texture_usage::copy_destination))
            {
                return invalid_view(
                    view.name,
                    std::format(
                        "the capture of '{}' mip {} needs a {} texture of {} x {} that allows copy_destination",
                        capture.resource,
                        capture.mip,
                        rhi::to_string(found->format),
                        width,
                        height));
            }
            const rhi::access_state common = rhi::access_states::common;
            const graphics::texture destination =
                graph_.import_texture(std::format("capture_{}", capture_index), capture.texture, common, common);
            const auto setup = [&](graphics::render_pass_builder& builder, capture_data& data)
            {
                const graphics::subresource_range mip{ .first_mip = static_cast<std::uint16_t>(capture.mip),
                                                       .mip_count = 1 };
                data.source = builder.read(found->texture, graphics::texture_read::copy_source, mip);
                data.destination = builder.write(destination, graphics::texture_write::copy_destination);
                data.mip = capture.mip;
            };
            const auto execute = [](const capture_data& data, const graphics::resource_bag& bag, auto& context)
            {
                context.recorder().copy_texture(
                    bag.texture_handle(data.destination),
                    {},
                    bag.texture_handle(data.source),
                    rhi::subresource{ .mip = static_cast<std::uint16_t>(data.mip) });
            };
            const std::string pass_name = std::format("copy_capture_{}", capture_index);
            if (rhi::traits(found->format).depth)
            {
                // On the graphics queue, which can decompress a compressed depth buffer; a copy queue may not
                graph_.add_graphics_pass<capture_data>(
                    pass_name,
                    setup,
                    [execute](
                        const capture_data& data,
                        const graphics::resource_bag& bag,
                        graphics::graphics_pass_context& context) { execute(data, bag, context); });
            }
            else
            {
                graph_.add_copy_pass<capture_data>(
                    pass_name,
                    setup,
                    [execute](
                        const capture_data& data,
                        const graphics::resource_bag& bag,
                        graphics::copy_pass_context& context) { execute(data, bag, context); });
            }
        }

        if (auto failed = add_buffer_captures(view, clusters.lights, clusters.indices, adapted))
        {
            return failed;
        }

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
                    data.source = builder.read(ldr_color, graphics::texture_read::copy_source);
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

    std::optional<error> scene_renderer::add_buffer_captures(
        const view_description& view,
        const graphics::buffer cluster_lights,
        const graphics::buffer light_indices,
        const graphics::buffer exposure)
    {
        struct capturable_buffer
        {
            std::string_view name;
            graphics::buffer buffer;
            std::uint64_t size;
        };
        const std::array resources{
            capturable_buffer{
                "cluster_lights", cluster_lights, std::uint64_t{ gpu::cluster_count } * sizeof(gpu::cluster_lights) },
            capturable_buffer{ "light_indices", light_indices, cluster_light_index_count * 4 },
            capturable_buffer{ "exposure", exposure, gpu::exposure_buffer_bytes },
        };
        for (std::size_t capture_index = 0; capture_index < view.options.buffer_captures.size(); ++capture_index)
        {
            const buffer_capture& capture = view.options.buffer_captures[capture_index];
            const auto found =
                std::ranges::find(resources, std::string_view{ capture.resource }, &capturable_buffer::name);
            if (found == resources.end())
            {
                return invalid_view(view.name, std::format("no buffer '{}' to capture", capture.resource));
            }
            const rhi::buffer_description& target = dependencies_.resources.description(capture.buffer);
            if (target.size < found->size || !(target.usage & rhi::buffer_usage::copy_destination))
            {
                return invalid_view(
                    view.name,
                    std::format(
                        "the capture of '{}' needs a buffer of at least {} bytes that allows copy_destination",
                        capture.resource,
                        found->size));
            }
            const rhi::access_state common = rhi::access_states::common;
            const graphics::buffer destination =
                graph_.import_buffer(std::format("buffer_capture_{}", capture_index), capture.buffer, common, common);
            graph_.add_copy_pass<buffer_capture_data>(
                std::format("copy_buffer_capture_{}", capture_index),
                [&](graphics::render_pass_builder& builder, buffer_capture_data& data)
                {
                    data.source = builder.read(found->buffer, graphics::buffer_read::copy_source);
                    data.destination = builder.write(destination, graphics::buffer_write::copy_destination);
                },
                [size = found->size](
                    const buffer_capture_data& data,
                    const graphics::resource_bag& bag,
                    graphics::copy_pass_context& context)
                {
                    context.recorder().copy_buffer(
                        bag.buffer_handle(data.destination), 0, bag.buffer_handle(data.source), 0, size);
                });
        }
        return std::nullopt;
    }
} // namespace avis::visualizer::rendering
