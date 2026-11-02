// The range walker (docs/tasks/11-renderer.md §11.9, 11 §4.6–§4.8): runs and their instance indices, commands, the
// redundant-state filter, viewport changes and recording groups. The unit tests cut hand-built draw lists into runs
// without a device. The [renderer] tests walk draw lists built from a scene of coloured quads with the renderer's
// pipeline sets, in a graph of their own on the renderer fixture, and read the image back.

#include "helpers/recorded_assertions.h"
#include "helpers/renderer_fixture.h"
#include "rendering/draw_lists.h"
#include "rendering/geometry_arena.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/range_walker.h"
#include "rendering/scene_renderer.h"
#include "rendering/sort_key.h"

#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/graphics/linear_arena.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/resources.h"
#include "avis/scene/flags.h"
#include "avis/scene/snapshot.h"

#include <Eigen/Geometry>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;

namespace
{
    // Materials by handle index
    constexpr std::uint32_t opaque_material = 1;
    constexpr std::uint32_t blended_material = 2;
    constexpr std::uint32_t double_sided_material = 3;

    constexpr scene::material_handle material(const std::uint32_t index) noexcept
    {
        return { .index = index, .generation = 1 };
    }

    // Unlit, so the overlay passes draw them with the vertex colour
    draw_material material_state(const scene::material_handle handle)
    {
        draw_material state{ .shading_model = scene::shading_model::unlit };
        if (handle.index == blended_material)
        {
            state.alpha_mode = scene::alpha_mode::blend;
        }
        if (handle.index == double_sided_material)
        {
            state.double_sided = true;
        }
        return state;
    }

    Eigen::Matrix4f placed(const Eigen::Vector3f& position, const float scale = 1.0f)
    {
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        matrix.topLeftCorner<3, 3>() *= scale;
        matrix.col(3).head<3>() = position;
        return matrix;
    }

    // A reverse-Z orthographic cascade over [-x, x] × [-y, y] of world space, looking down −Z from z = 100 to −100
    cascade_viewpoint orthographic_cascade(const float x, const float y)
    {
        cascade_viewpoint viewpoint;
        viewpoint.view_from_world = placed(Eigen::Vector3f{ 0.0f, 0.0f, -100.0f });
        Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
        projection(0, 0) = 1.0f / x;
        projection(1, 1) = 1.0f / y;
        projection(2, 2) = 1.0f / 200.0f;
        projection(2, 3) = 1.0f;
        projection(3, 3) = 1.0f;
        viewpoint.projection = projection;
        return viewpoint;
    }

    constexpr std::uint32_t quad_vertices = 4;
    constexpr std::uint32_t quad_indices = 6;

    // Unit quads: two triangles in the XY plane over [-0.5, 0.5]², facing +Z, with one sRGB vertex colour per mesh. The
    // snapshot and the extraction result are built together, so instance_sources line up.
    struct quad_scene
    {
        scene::snapshot snapshot;
        extraction_result extraction;
        std::vector<std::vector<scene::mesh_primitive>> primitives; // per mesh
        std::vector<draw_mesh> meshes;
        std::vector<std::array<std::uint8_t, 4>> colors; // per mesh

        std::uint32_t add_mesh(const std::array<std::uint8_t, 4> color, const bool progressive = false)
        {
            scene::mesh_primitive primitive;
            primitive.index_count = quad_indices;
            primitive.vertex_count = quad_vertices;
            primitive.bounds = { .minimum = Eigen::Vector3f{ -0.5f, -0.5f, 0.0f },
                                 .maximum = Eigen::Vector3f{ 0.5f, 0.5f, 0.0f } };
            primitives.push_back({ primitive });
            meshes.push_back(
                { .topology = scene::mesh_topology::triangles,
                  .attributes = scene::vertex_attribute::color0,
                  .progressive = progressive });
            colors.push_back(color);
            extraction.meshes.push_back(
                { .handle = { .index = static_cast<std::uint32_t>(meshes.size()), .generation = 1 }, .version = 1 });
            finish();
            return static_cast<std::uint32_t>(meshes.size() - 1);
        }

        scene::render_instance&
            add(const scene::object_id id,
                const std::uint32_t mesh,
                const std::uint32_t material_index,
                const Eigen::Matrix4f& world,
                const scene::draw_layer layer = scene::draw_layer::overlay_on_top,
                const std::uint8_t layer_priority = 0)
        {
            scene::render_instance instance;
            instance.object_id = id;
            instance.mesh = extraction.meshes[mesh].handle;
            instance.material = material(material_index);
            instance.world_current = world;
            instance.world_previous = world;
            instance.layer = layer;
            instance.layer_priority = layer_priority;
            instance.flags = scene::instance_flags::visible | scene::instance_flags::casts_shadows;
            snapshot.instances.push_back(instance);
            extraction.instance_sources.push_back(static_cast<std::uint32_t>(snapshot.instances.size() - 1));

            gpu::instance_data data{};
            std::memcpy(&data.world, world.data(), sizeof(data.world));
            std::memcpy(&data.world_previous, world.data(), sizeof(data.world_previous));
            data.mesh = mesh;
            data.material = material_index; // the slot
            data.object_id_low = static_cast<std::uint32_t>(id);
            extraction.instances.push_back(data);
            return snapshot.instances.back();
        }

        void finish()
        {
            for (std::size_t index = 0; index < meshes.size(); ++index)
            {
                meshes[index].primitives = primitives[index];
            }
        }

        draw_list build(
            graphics::linear_arena& arena,
            const view_description& view,
            draw_list_lookup lookup,
            const std::span<const cascade_viewpoint> cascades = {},
            const draw_commands* commands = nullptr) const
        {
            const draw_list_sources sources{
                .snapshot = snapshot,
                .extraction = extraction,
                .meshes = meshes,
                .cascades = cascades,
                .commands = commands != nullptr ? commands->items() : std::span<const draw_item>{},
                .command_arguments = commands != nullptr ? commands->arguments() : std::span<const std::byte>{},
                .lookup = std::move(lookup),
                .opaque_depth_bands = 16,
            };
            return build_draw_list(view, sources, arena);
        }
    };

    // A set index from the key alone, for the unit tests
    draw_list_lookup unit_lookup()
    {
        return { .material = &material_state,
                 .pipeline_set = [](const pipeline_set_key& key) -> result<std::uint32_t>
                 {
                     const pipeline_set_key canonical = key.canonical();
                     return static_cast<std::uint32_t>(canonical.alpha_mode) * 2 + (canonical.double_sided ? 1u : 0u);
                 } };
    }

    // A camera at the origin looking down −Z, 60° vertical field of view
    view_description test_view(const std::uint32_t width = 128, const std::uint32_t height = 72)
    {
        view_description view;
        view.name = "main";
        view.size = { width, height };
        view.camera.far_plane = 1000.0f;
        return view;
    }

    constexpr key_group camera_group(const viewport_layer layer, const translucency value = translucency::opaque)
    {
        return { .fullscreen_layer = fullscreen_layer::scene,
                 .viewport = viewport::camera(),
                 .viewport_layer = layer,
                 .translucency = value };
    }

    // ---- the GPU harness ----

    // An upload-heap buffer the CPU writes and the shaders read in place, grown by replacing it
    class upload_buffer
    {
    public:
        upload_buffer(rhi::resources resources, const std::uint32_t stride, const bool index) :
            resources_{ resources },
            stride_{ stride },
            index_{ index }
        {}

        ~upload_buffer()
        {
            if (buffer_.is_valid())
            {
                resources_.destroy(buffer_);
            }
        }

        upload_buffer(const upload_buffer&) = delete;
        upload_buffer& operator=(const upload_buffer&) = delete;

        // Writes `bytes` from the start; the GPU must be done with the previous content
        void write(const std::span<const std::byte> bytes)
        {
            // At least 256 bytes, in whole elements of a structured buffer
            const std::size_t unit = std::max<std::size_t>(stride_, 4);
            const std::size_t size = (std::max<std::size_t>(bytes.size(), 256) + unit - 1) / unit * unit;
            if (!buffer_.is_valid() || resources_.description(buffer_).size < size)
            {
                if (buffer_.is_valid())
                {
                    resources_.destroy(buffer_);
                }
                avis::flags<rhi::buffer_usage> usage = rhi::buffer_usage::shader_resource;
                if (index_)
                {
                    usage = usage | rhi::buffer_usage::index;
                }
                auto created = resources_.create_buffer(
                    { .size = size * 2,
                      .structure_stride = stride_,
                      .usage = usage,
                      .memory = rhi::memory_type::upload,
                      .debug_name = "range walker test buffer" });
                if (!created)
                {
                    FAIL(std::format("creating a {}-byte upload buffer: {}", size * 2, created.error().message));
                }
                buffer_ = *created;
            }
            std::memcpy(resources_.mapped_bytes(buffer_).data(), bytes.data(), bytes.size());
        }

        rhi::buffer_handle handle() const noexcept
        {
            return buffer_;
        }

        rhi::bindless_index srv() const
        {
            return resources_.srv(buffer_);
        }

    private:
        rhi::resources resources_;
        rhi::buffer_handle buffer_;
        std::uint32_t stride_;
        bool index_;
    };

    struct walk_result
    {
        tests::rgba8_image image; // the first render target, when it is RGBA8
        range_walk_statistics statistics; // of every part
        std::size_t parts = 0;
        std::uint32_t recording_groups = 0;
        std::size_t items = 0; // in the range walked
    };

    struct walk_request
    {
        pass_kind kind = pass_kind::overlay_on_top;
        key_range range = prefix_range(key_prefix(fullscreen_layer::scene));
        std::uint32_t parts = 1;
        bool depth_written = true; // pass_bindings::depth_written
        std::span<const cascade_viewpoint> cascades; // bound as slices of a depth array (shadow kinds)
        const draw_commands* commands = nullptr;
    };

    tests::renderer_fixture_options harness_options(const std::uint32_t passes_per_group)
    {
        tests::renderer_fixture_options options;
        options.graph.graph.passes_per_group_without_timings = passes_per_group;
        return options;
    }

    // The renderer fixture, for its pipeline sets and graph, with the scene's geometry and frame arrays in upload
    // buffers. The renderer itself never renders here.
    class walker_harness
    {
    public:
        explicit walker_harness(const std::uint32_t passes_per_group = 1) :
            fixture{ harness_options(passes_per_group) },
            resources{ *fixture.graphics.hardware_interface },
            vertices{ resources, 0, false },
            indices{ resources, 0, true },
            instances{ resources, sizeof(gpu::instance_data), false },
            instance_indices{ resources, sizeof(std::uint32_t), false }
        {}

        ~walker_harness()
        {
            fixture.graphics.hardware_interface->wait_idle();
        }

        walker_harness(const walker_harness&) = delete;
        walker_harness& operator=(const walker_harness&) = delete;

        // The meshes' streams, one range each, and their ranges in the scene's mesh table
        void upload_meshes(quad_scene& scene)
        {
            std::vector<std::byte> vertex_bytes;
            std::vector<std::uint32_t> index_values;
            for (std::size_t mesh = 0; mesh < scene.meshes.size(); ++mesh)
            {
                const vertex_stream_layout layout = layout_vertex_streams(quad_vertices, scene.meshes[mesh].attributes);
                const auto base = static_cast<std::uint32_t>(vertex_bytes.size());
                vertex_bytes.resize(vertex_bytes.size() + layout.size);

                const std::array<float, 12> positions{ -0.5f, -0.5f, 0.0f, 0.5f,  -0.5f, 0.0f,
                                                       0.5f,  0.5f,  0.0f, -0.5f, 0.5f,  0.0f };
                std::memcpy(vertex_bytes.data() + base + layout.offsets.position, positions.data(), sizeof(positions));
                for (std::uint32_t vertex = 0; vertex < quad_vertices; ++vertex)
                {
                    std::memcpy(
                        vertex_bytes.data() + base + layout.offsets.color0 + vertex * attribute_stride,
                        scene.colors[mesh].data(),
                        attribute_stride);
                }

                geometry_range range;
                range.vertex_offset = base;
                range.vertex_size = static_cast<std::uint32_t>(layout.size);
                range.index_offset = static_cast<std::uint32_t>(index_values.size() * index_stride);
                range.index_size = quad_indices * index_stride;
                range.streams = layout.offsets;
                range.streams.position += base;
                range.streams.color0 += base;
                scene.meshes[mesh].range = range;
                // Counter-clockwise seen from +Z
                for (const std::uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
                {
                    index_values.push_back(index);
                }
            }
            vertices.write(vertex_bytes);
            indices.write(std::as_bytes(std::span{ index_values }));
        }

        // The pipeline sets the renderer created, by the draw list's lookup
        draw_list_lookup lookup()
        {
            return { .material = &material_state,
                     .pipeline_set = [this](const pipeline_set_key& key)
                     { return fixture.renderer->pipeline_variants().find(key); } };
        }

        // One frame: builds the scene's draw list in the graph's arena, cuts it into runs, writes the instances and
        // indices, then walks the request's range in `parts` passes into the kind's targets (cleared to 0) and copies
        // an RGBA8 first target into the fixture's output
        walk_result walk_frame(const quad_scene& scene, const walk_request& request)
        {
            graphics::render_graph& graph = fixture.graphics.graph;
            const view_description view = test_view(fixture.output_size.width, fixture.output_size.height);
            graphics::frame_token frame = fixture.graphics.begin_frame();
            graph.reset();
            graph.set_view_size(view.size);

            const draw_list list = scene.build(graph.arena(), view, lookup(), request.cascades, request.commands);
            std::vector<std::uint32_t> frame_indices;
            const prepared_draw_list prepared = prepare_draw_runs(list, scene.meshes, graph.arena(), frame_indices);
            instances.write(std::as_bytes(std::span{ scene.extraction.instances }));
            instance_indices.write(std::as_bytes(std::span{ frame_indices }));

            const std::span<const draw_run> runs = prepared.runs_of(list.range(request.range));
            const std::vector<std::span<const draw_run>> parts = split_runs(runs, request.parts);

            walk_result walked{ .parts = parts.size(), .items = item_count(runs) };
            std::mutex statistics_mutex;
            const range_walk_sources sources{ .list = prepared,
                                              .meshes = scene.meshes,
                                              .pipelines = fixture.renderer->pipeline_variants(),
                                              .index_buffer = indices.handle() };

            // The camera's and the cascades' view constants
            std::vector<gpu::view_constants> viewpoints;
            const float aspect = static_cast<float>(view.size.width) / static_cast<float>(view.size.height);
            viewpoints.push_back(view_constants_of(view.camera.projection(aspect) * view.camera.view_from_world()));
            for (const cascade_viewpoint& cascade : request.cascades)
            {
                viewpoints.push_back(view_constants_of(cascade.projection * cascade.view_from_world));
            }
            const gpu::scene_pass_constants pass_constants{ .instances = instances.srv().value,
                                                            .instance_indices = instance_indices.srv().value,
                                                            .vertices = vertices.srv().value,
                                                            .indices = indices.srv().value,
                                                            .materials = 0,
                                                            .padding0 = 0,
                                                            .padding1 = 0,
                                                            .padding2 = 0 };

            const pass_attachment_formats formats = attachment_formats(request.kind);
            const bool shadow = request.kind == pass_kind::shadow;
            const auto layers = static_cast<std::uint32_t>(std::max<std::size_t>(request.cascades.size(), 1));

            struct targets
            {
                std::array<graphics::texture, 8> colors{};
                std::size_t color_count = 0;
                graphics::texture depth;
            };
            targets current;
            for (std::size_t part = 0; part < std::max<std::size_t>(parts.size(), 1); ++part)
            {
                const std::span<const draw_run> part_runs = parts.empty() ? std::span<const draw_run>{} : parts[part];
                const targets& written = graph.add_graphics_pass<targets>(
                    std::format("walk_{}", part),
                    [&](graphics::render_pass_builder& builder, targets& data)
                    {
                        builder.set_side_effects();
                        data.color_count = formats.render_targets.size();
                        for (std::size_t index = 0; index < data.color_count; ++index)
                        {
                            graphics::texture input = current.colors[index];
                            if (part == 0)
                            {
                                graphics::texture_description description{ .size = graphics::view_relative(1.0f),
                                                                           .format = formats.render_targets[index] };
                                description.clear_value = rhi::clear_value{};
                                input = builder.create_texture(std::format("color_{}", index), description);
                            }
                            data.colors[index] = builder.write(
                                input,
                                graphics::texture_write::render_target,
                                graphics::all_subresources,
                                part == 0 ? graphics::write_flags::needs_clear : graphics::write_flags::none);
                        }
                        graphics::texture depth = current.depth;
                        if (part == 0)
                        {
                            graphics::texture_description description{ .size = graphics::view_relative(1.0f),
                                                                       .depth_or_layers = shadow ? layers : 1,
                                                                       .format = formats.depth };
                            description.clear_value = rhi::clear_value{};
                            depth = builder.create_texture("depth", description);
                        }
                        data.depth = builder.write(
                            depth,
                            graphics::texture_write::depth_write,
                            graphics::all_subresources,
                            part == 0 ? graphics::write_flags::needs_clear : graphics::write_flags::none);
                    },
                    [&, part_runs](
                        const targets& data,
                        const graphics::resource_bag& bag,
                        graphics::graphics_pass_context& context)
                    {
                        std::array<rhi::texture_view, 8> colors{};
                        for (std::size_t index = 0; index < data.color_count; ++index)
                        {
                            colors[index] = bag.render_target_view(data.colors[index]);
                        }
                        const std::uint32_t width = bag.width(data.depth);
                        const std::uint32_t height = bag.height(data.depth);
                        const rhi::viewport full_viewport{
                            0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f
                        };
                        const rhi::rectangle full_scissor{
                            0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)
                        };

                        pass_bindings bindings;
                        bindings.depth_written = request.depth_written;
                        bindings.pass_constants = context.allocate_constants(pass_constants);
                        if (shadow)
                        {
                            // A cascade's slice of the depth array per viewport (11 §4.6)
                            for (std::size_t cascade = 0; cascade < request.cascades.size(); ++cascade)
                            {
                                rhi::texture_view slice = bag.depth_stencil_view(data.depth);
                                slice.first_layer = static_cast<std::uint16_t>(cascade);
                                slice.layer_count = 1;
                                bindings
                                    .viewports[viewport::shadow_cascade(static_cast<std::uint32_t>(cascade)).index] =
                                    viewport_binding{ .render_targets = {},
                                                      .depth = slice,
                                                      .viewport = full_viewport,
                                                      .scissor = full_scissor,
                                                      .view_constants =
                                                          context.allocate_constants(viewpoints[cascade + 1]) };
                            }
                        }
                        else
                        {
                            bindings.viewports[0] =
                                viewport_binding{ .render_targets = std::span{ colors }.first(data.color_count),
                                                  .depth = bag.depth_stencil_view(data.depth),
                                                  .viewport = full_viewport,
                                                  .scissor = full_scissor,
                                                  .view_constants = context.allocate_constants(viewpoints[0]) };
                        }

                        const range_walk_statistics statistics =
                            walk(part_runs, request.kind, context, sources, bindings);
                        const std::lock_guard lock{ statistics_mutex };
                        walked.statistics += statistics;
                    });
                current.colors = written.colors;
                current.color_count = written.color_count;
                current.depth = written.depth;
            }

            const bool readable =
                !formats.render_targets.empty() && formats.render_targets[0] == rhi::format::rgba8_unorm;
            if (readable)
            {
                const graphics::texture output = graph.import_texture(
                    "output", fixture.output, rhi::access_states::common, rhi::access_states::common);
                struct copy_data
                {
                    graphics::texture source;
                    graphics::texture output;
                };
                graph.add_copy_pass<copy_data>(
                    "copy_to_output",
                    [&](graphics::render_pass_builder& builder, copy_data& data)
                    {
                        data.source = builder.read(current.colors[0], graphics::texture_read::copy_source);
                        data.output = builder.write(output, graphics::texture_write::copy_destination);
                    },
                    [](const copy_data& data, const graphics::resource_bag& bag, graphics::copy_pass_context& context)
                    {
                        context.recorder().copy_texture(
                            bag.texture_handle(data.output), {}, bag.texture_handle(data.source), {});
                    });
            }

            graph.compile();
            const result<graphics::submitted_frame> submitted = fixture.graphics.execute(frame);
            REQUIRE(submitted.has_value());
            fixture.graphics.wait(*submitted);
            fixture.graphics.frames->end_frame(std::move(frame), submitted->last_submissions());
            walked.recording_groups = graph.debug_information().recording_groups;
            if (readable)
            {
                walked.image = fixture.read_output(fixture.output);
            }
            return walked;
        }

        tests::renderer_fixture fixture;
        rhi::resources resources;
        upload_buffer vertices;
        upload_buffer indices;
        upload_buffer instances;
        upload_buffer instance_indices;

    private:
        static gpu::view_constants view_constants_of(const Eigen::Matrix4f& clip_from_world)
        {
            gpu::view_constants constants{};
            std::memcpy(&constants.view_projection, clip_from_world.data(), sizeof(constants.view_projection));
            std::memcpy(
                &constants.view_projection_unjittered, clip_from_world.data(), sizeof(constants.view_projection));
            std::memcpy(&constants.view_projection_previous, clip_from_world.data(), sizeof(constants.view_projection));
            constants.device_pixel_ratio = 1.0f;
            return constants;
        }
    };

    constexpr std::array<std::uint8_t, 4> red{ 255, 0, 0, 255 };
    constexpr std::array<std::uint8_t, 4> green{ 0, 255, 0, 255 };
    constexpr std::array<std::uint8_t, 4> blue{ 0, 0, 255, 255 };
    constexpr std::array<std::uint8_t, 4> half_white{ 255, 255, 255, 128 };

    std::size_t count_not_black(const tests::rgba8_image& image)
    {
        return image.count_different({ 0, 0, 0, 0 });
    }

    // Draw items of one group, in key order, whose (key, mesh) changes at each run: what the runs must be
    std::size_t expected_runs(const draw_list& list, const std::span<const draw_mesh> meshes)
    {
        std::size_t runs = 0;
        const std::span<const draw_item> items = list.items();
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            if (index == 0 || is_command_key(items[index].key) || items[index].key != items[index - 1].key)
            {
                ++runs;
                continue;
            }
            const draw_record& record = list.records()[items[index].value];
            const draw_record& previous = list.records()[items[index - 1].value];
            if (record.mesh != previous.mesh || record.submesh != previous.submesh || meshes[record.mesh].progressive)
            {
                ++runs;
            }
        }
        return runs;
    }
} // namespace

// ---- runs (unit) ----

TEST_CASE("runs merge equal keys of one mesh and write their instance indices contiguously", "[renderer][unit]")
{
    graphics::linear_arena arena;
    quad_scene scene;
    const std::uint32_t first = scene.add_mesh(red);
    const std::uint32_t second = scene.add_mesh(green);

    SECTION("100 instances of one mesh and material are one run of 100")
    {
        for (scene::object_id id = 1; id <= 100; ++id)
        {
            scene.add(id, first, opaque_material, placed({ 0.0f, 0.0f, -5.0f }));
        }
        const draw_list list = scene.build(arena, test_view(), unit_lookup());
        std::vector<std::uint32_t> indices{ 7, 7, 7 }; // earlier views' indices are kept
        const prepared_draw_list prepared = prepare_draw_runs(list, scene.meshes, arena, indices);

        REQUIRE(prepared.runs.size() == 1);
        CHECK(prepared.runs[0] == draw_run{ .first = 0, .count = 100, .instance_offset = 3 });
        REQUIRE(indices.size() == 103);
        std::set<std::uint32_t> instances(indices.begin() + 3, indices.end());
        CHECK(instances.size() == 100);
        CHECK(*instances.rbegin() == 99);
    }

    SECTION("two meshes are two runs")
    {
        for (scene::object_id id = 1; id <= 100; ++id)
        {
            scene.add(id, id % 2 == 0 ? first : second, opaque_material, placed({ 0.0f, 0.0f, -5.0f }));
        }
        const draw_list list = scene.build(arena, test_view(), unit_lookup());
        std::vector<std::uint32_t> indices;
        const prepared_draw_list prepared = prepare_draw_runs(list, scene.meshes, arena, indices);

        REQUIRE(prepared.runs.size() == 2);
        CHECK(prepared.runs[0].count == 50);
        CHECK(prepared.runs[1].count == 50);
        CHECK(prepared.runs[1].instance_offset == 50);
        for (const draw_run& run : prepared.runs)
        {
            const std::uint32_t mesh = list.records()[list.items()[run.first].value].mesh;
            for (std::uint32_t index = 0; index < run.count; ++index)
            {
                const draw_item& item = list.items()[run.first + index];
                CHECK(list.records()[item.value].mesh == mesh);
                CHECK(indices[run.instance_offset + index] == list.records()[item.value].instance);
            }
        }
    }

    SECTION("interleaved translucent items keep back-to-front order and aren't merged")
    {
        for (scene::object_id id = 1; id <= 20; ++id)
        {
            const float distance = 2.0f + static_cast<float>(id);
            scene.add(id, id % 2 == 0 ? first : second, blended_material, placed({ 0.0f, 0.0f, -distance }));
        }
        const draw_list list = scene.build(arena, test_view(), unit_lookup());
        std::vector<std::uint32_t> indices;
        const prepared_draw_list prepared = prepare_draw_runs(list, scene.meshes, arena, indices);

        REQUIRE(prepared.runs.size() == 20);
        float previous = std::numeric_limits<float>::infinity();
        for (const draw_run& run : prepared.runs)
        {
            CHECK(run.count == 1);
            const std::uint32_t instance = indices[run.instance_offset];
            const float distance =
                -scene.snapshot.instances[scene.extraction.instance_sources[instance]].world_current(2, 3);
            CHECK(distance < previous); // far to near
            previous = distance;
        }
    }

    SECTION("progressive meshes draw one instance per run")
    {
        const std::uint32_t progressive = scene.add_mesh(blue, true);
        for (scene::object_id id = 1; id <= 10; ++id)
        {
            scene.add(id, progressive, opaque_material, placed({ 0.0f, 0.0f, -5.0f }));
        }
        const draw_list list = scene.build(arena, test_view(), unit_lookup());
        std::vector<std::uint32_t> indices;
        const prepared_draw_list prepared = prepare_draw_runs(list, scene.meshes, arena, indices);
        CHECK(prepared.runs.size() == 10);
        CHECK(indices.size() == 10);
    }

    SECTION("commands are runs of their own, after their group's draws")
    {
        for (scene::object_id id = 1; id <= 10; ++id)
        {
            scene.add(id, first, opaque_material, placed({ 0.0f, 0.0f, -5.0f }));
        }
        draw_commands commands;
        commands.reset_state(camera_group(viewport_layer::overlay_on_top), 0);
        commands.set_write_mask(camera_group(viewport_layer::overlay_on_top), 1, write_mask::color);
        const draw_list list = scene.build(arena, test_view(), unit_lookup(), {}, &commands);
        std::vector<std::uint32_t> indices;
        const prepared_draw_list prepared = prepare_draw_runs(list, scene.meshes, arena, indices);

        REQUIRE(prepared.runs.size() == 3);
        CHECK(prepared.runs[0].count == 10);
        CHECK(prepared.runs[1] == draw_run{ .first = 10, .count = 1, .instance_offset = 0 });
        CHECK(prepared.runs[2] == draw_run{ .first = 11, .count = 1, .instance_offset = 0 });
        CHECK(indices.size() == 10); // commands have no instances
        CHECK(list.command_arguments().size() >= 16 + 4);
        CHECK(list.items()[10].value != list.items()[11].value);
    }

    SECTION("random scenes: runs cover the list, each is one key and one mesh")
    {
        std::mt19937 random{ 99 };
        std::uniform_int_distribution<int> choice{ 0, 5 };
        std::uniform_real_distribution<float> distance{ 1.0f, 100.0f };
        const std::uint32_t progressive = scene.add_mesh(blue, true);
        for (scene::object_id id = 1; id <= 1000; ++id)
        {
            const int pick = choice(random);
            const std::uint32_t mesh = pick < 2 ? first : (pick < 4 ? second : progressive);
            const std::uint32_t material_index =
                pick % 3 == 0 ? blended_material : (pick % 3 == 1 ? opaque_material : double_sided_material);
            const scene::draw_layer layer = pick == 5 ? scene::draw_layer::overlay : scene::draw_layer::world;
            scene.add(id, mesh, material_index, placed({ 0.0f, 0.0f, -distance(random) }), layer);
        }
        const draw_list list = scene.build(arena, test_view(), unit_lookup());
        std::vector<std::uint32_t> indices;
        const prepared_draw_list prepared = prepare_draw_runs(list, scene.meshes, arena, indices);

        CHECK(prepared.runs.size() == expected_runs(list, scene.meshes));
        std::uint32_t next = 0;
        for (const draw_run& run : prepared.runs)
        {
            CHECK(run.first == next);
            next += run.count;
        }
        CHECK(next == list.items().size());
        CHECK(indices.size() == list.items().size());

        // runs_of a group's range is exactly the runs inside it
        for (const draw_group& group : list.groups())
        {
            const std::span<const draw_run> runs = prepared.runs_of(list.range(key_prefix(group.group)));
            REQUIRE(!runs.empty());
            CHECK(runs.front().first == group.first);
            CHECK(runs.back().first + runs.back().count == group.first + group.count);
        }
        CHECK(prepared.runs_of(list.range(key_prefix(fullscreen_layer::hud))).empty());
    }
}

TEST_CASE("long ranges split into parts at run boundaries", "[renderer][unit]")
{
    SECTION("the part count")
    {
        const recording_split split{ .items_per_part = 1000, .passes_per_group = 4, .maximum_parts = 16 };
        CHECK(recording_part_count(0, split) == 1);
        CHECK(recording_part_count(4000, split) == 1); // four parts would land in one recording group
        CHECK(recording_part_count(4001, split) == 5);
        CHECK(recording_part_count(10'000, split) == 10);
        CHECK(recording_part_count(1'000'000, split) == 16);
        CHECK(recording_part_count(5000, { .items_per_part = 1000, .passes_per_group = 1, .maximum_parts = 16 }) == 5);
    }

    SECTION("parts")
    {
        std::vector<draw_run> runs;
        std::uint32_t first = 0;
        for (std::uint32_t index = 0; index < 37; ++index)
        {
            const std::uint32_t count = 1 + index % 7;
            runs.push_back({ .first = first, .count = count, .instance_offset = first });
            first += count;
        }
        for (const std::uint32_t parts : { 1u, 2u, 4u, 7u, 37u, 100u })
        {
            const std::vector<std::span<const draw_run>> split = split_runs(runs, parts);
            CHECK(split.size() == std::min<std::size_t>(parts, runs.size()));
            std::size_t covered = 0;
            for (const std::span<const draw_run> part : split)
            {
                CHECK(!part.empty());
                CHECK(part.data() == runs.data() + covered); // contiguous, in order
                covered += part.size();
            }
            CHECK(covered == runs.size());
            if (parts == 4)
            {
                // About a quarter of the items each, within one run
                for (const std::span<const draw_run> part : split)
                {
                    CHECK(item_count(part) >= first / 4 - 7);
                    CHECK(item_count(part) <= first / 4 + 7);
                }
            }
        }
        CHECK(split_runs({}, 4).empty());
    }
}

TEST_CASE("group names come from the key prefix", "[renderer][unit]")
{
    CHECK(group_name(camera_group(viewport_layer::world)) == "scene/camera/world/opaque");
    CHECK(
        group_name(camera_group(viewport_layer::overlay_on_top, translucency::subtractive)) ==
        "scene/camera/overlay_on_top/subtractive");
    CHECK(
        group_name(
            { .fullscreen_layer = fullscreen_layer::scene,
              .viewport = viewport::shadow_cascade(2),
              .viewport_layer = viewport_layer::world,
              .translucency = translucency::opaque }) == "scene/shadow_cascade[2]/world/opaque");
    CHECK(
        group_name(
            { .fullscreen_layer = fullscreen_layer::fullscreen_effects,
              .viewport = viewport::camera(),
              .viewport_layer = viewport_layer::background,
              .translucency = translucency::blended }) == "fullscreen_effects/camera/background/blended");
}

// ---- walking (device) ----

TEST_CASE("the walker batches instances into one draw per key and mesh", "[renderer]")
{
    walker_harness harness;
    quad_scene scene;
    const std::uint32_t first = scene.add_mesh(red);
    const std::uint32_t second = scene.add_mesh(green);

    SECTION("100 instances of one mesh and material: one draw of 100")
    {
        for (scene::object_id id = 1; id <= 100; ++id)
        {
            const float x = -4.0f + 0.08f * static_cast<float>(id);
            scene.add(id, first, opaque_material, placed({ x, 0.0f, -5.0f }));
        }
        harness.upload_meshes(scene);
        const walk_result walked = harness.walk_frame(scene, {});

        CHECK(walked.statistics.draws == 1);
        CHECK(walked.statistics.instances == 100);
        CHECK(walked.statistics.pipeline_changes == 1);
        CHECK(walked.statistics.signature_changes == 1);
        CHECK(walked.statistics.constant_pushes == 1);
        CHECK(walked.statistics.events == 1);
        CHECK(walked.statistics.viewport_changes == 1);
        CHECK(walked.statistics.skipped_invalid == 0);
        // A red band across the middle
        const tests::rgba8 centre = walked.image.at(walked.image.width / 2, walked.image.height / 2);
        CHECK(centre[0] == 255);
        CHECK(centre[1] == 0);
        CHECK(walked.image.at(walked.image.width / 2, 2)[0] == 0);
    }

    SECTION("two meshes: two draws")
    {
        for (scene::object_id id = 1; id <= 100; ++id)
        {
            const float x = -4.0f + 0.08f * static_cast<float>(id);
            scene.add(id, id <= 50 ? first : second, opaque_material, placed({ x, 0.0f, -5.0f }));
        }
        harness.upload_meshes(scene);
        const walk_result walked = harness.walk_frame(scene, {});

        CHECK(walked.statistics.draws == 2);
        CHECK(walked.statistics.instances == 100);
        CHECK(walked.statistics.pipeline_changes == 1); // same key
        CHECK(walked.statistics.constant_pushes == 2);
        const std::uint32_t y = walked.image.height / 2;
        CHECK(walked.image.at(walked.image.width / 4, y)[0] == 255);
        CHECK(walked.image.at(walked.image.width * 3 / 4, y)[1] == 255);
    }

    SECTION("interleaved translucent items aren't merged, and blend back to front")
    {
        // Half-transparent quads, alternating meshes, all covering the centre: the nearest is drawn last
        const std::uint32_t half_red = scene.add_mesh({ 255, 0, 0, 128 });
        const std::uint32_t half_green = scene.add_mesh({ 0, 255, 0, 128 });
        for (scene::object_id id = 1; id <= 6; ++id)
        {
            const float distance = 3.0f + static_cast<float>(id);
            scene.add(
                id, id % 2 == 0 ? half_red : half_green, blended_material, placed({ 0.0f, 0.0f, -distance }, 2.0f));
        }
        harness.upload_meshes(scene);
        const walk_result walked = harness.walk_frame(scene, {});

        CHECK(walked.statistics.draws == 6);
        CHECK(walked.statistics.instances == 6);
        CHECK(walked.statistics.pipeline_changes == 1);
        // Back to front, each `over` halves what's behind it, so the nearest quad has the largest share: id 1, green
        const tests::rgba8 centre = walked.image.at(walked.image.width / 2, walked.image.height / 2);
        CHECK(centre[1] > centre[0]);
        CHECK(centre[0] > 0);
    }
}

TEST_CASE("the filter sets each pipeline once per set and band", "[renderer]")
{
    walker_harness harness;
    quad_scene scene;
    const std::uint32_t first = scene.add_mesh(red);
    const std::uint32_t second = scene.add_mesh(green);

    // World-layer opaque quads in two sets (single- and double-sided) at distances over several depth bands, drawn by
    // the depth prepass's PSOs
    std::mt19937 random{ 5 };
    std::uniform_real_distribution<float> distance{ 2.0f, 400.0f };
    std::uniform_real_distribution<float> lateral{ -0.3f, 0.3f };
    for (scene::object_id id = 1; id <= 400; ++id)
    {
        const float d = distance(random);
        scene.add(
            id,
            id % 3 == 0 ? second : first,
            id % 2 == 0 ? opaque_material : double_sided_material,
            placed({ lateral(random) * d, lateral(random) * d * 0.5f, -d }),
            scene::draw_layer::world);
    }
    harness.upload_meshes(scene);
    const walk_result walked = harness.walk_frame(
        scene,
        { .kind = pass_kind::depth,
          .range = prefix_range(key_prefix(camera_group(viewport_layer::world, translucency::opaque))) });

    // The (band, set) pairs and the set changes along the range, from the list itself
    const draw_list list = scene.build(harness.fixture.graphics.graph.arena(), test_view(), harness.lookup());
    const std::span<const draw_item> items = list.range(key_prefix(camera_group(viewport_layer::world)));
    std::set<std::pair<std::uint64_t, std::uint64_t>> pairs;
    std::size_t set_changes = 0;
    for (std::size_t index = 0; index < items.size(); ++index)
    {
        const std::uint64_t set = key_fields::pipeline_set.extract(items[index].key);
        pairs.insert({ key_fields::depth.extract(items[index].key), set });
        if (index == 0 || set != key_fields::pipeline_set.extract(items[index - 1].key))
        {
            ++set_changes;
        }
    }
    INFO("items " << items.size() << ", (band, set) pairs " << pairs.size());
    REQUIRE(walked.items == items.size());
    CHECK(pairs.size() > 2); // the scene spans several bands
    CHECK(walked.statistics.pipeline_changes == set_changes);
    CHECK(walked.statistics.pipeline_changes <= pairs.size());
    CHECK(walked.statistics.signature_changes == 1);
    CHECK(walked.statistics.constant_pushes == walked.statistics.draws);
    CHECK(walked.statistics.viewport_sets == 1);
    CHECK(walked.statistics.scissor_sets == 1);
    CHECK(walked.statistics.instances == items.size());
}

TEST_CASE("a new viewport field binds that cascade's slice", "[renderer]")
{
    walker_harness harness;
    quad_scene scene;
    const std::uint32_t mesh = scene.add_mesh(red);
    for (scene::object_id id = 1; id <= 20; ++id)
    {
        // Left half in cascade 0, right half in cascade 1, all in the camera
        const float x = id <= 10 ? -2.0f : 2.0f;
        scene.add(id, mesh, opaque_material, placed({ x, 0.0f, -5.0f }), scene::draw_layer::world);
    }
    harness.upload_meshes(scene);
    const std::array<cascade_viewpoint, 2> cascades{ orthographic_cascade(10.0f, 10.0f),
                                                     orthographic_cascade(10.0f, 10.0f) };
    const walk_result walked = harness.walk_frame(
        scene,
        { .kind = pass_kind::shadow,
          .range = prefix_range(
              key_prefix(fullscreen_layer::scene, viewport::shadow_cascade(0)),
              key_prefix(fullscreen_layer::scene, viewport::shadow_cascade(3))),
          .cascades = cascades });

    CHECK(walked.items == 40);
    CHECK(walked.statistics.viewport_changes == 2);
    CHECK(walked.statistics.events == 2);
    CHECK(walked.statistics.draws == 2);
    CHECK(walked.statistics.instances == 40);
    CHECK(walked.statistics.pipeline_changes == 1);
    CHECK(walked.statistics.skipped_invalid == 0);
}

TEST_CASE("commands run through the walk", "[renderer]")
{
    walker_harness harness;
    quad_scene scene;
    const std::uint32_t mesh = scene.add_mesh(red);
    for (scene::object_id id = 1; id <= 4; ++id)
    {
        scene.add(id, mesh, opaque_material, placed({ -3.0f + 2.0f * static_cast<float>(id - 1), 0.0f, -5.0f }));
    }
    harness.upload_meshes(scene);

    SECTION("a clear after the overlay group draws over it")
    {
        // overlay sorts before overlay_on_top: the clear runs after the (empty) overlay group's draws, before ours
        draw_commands commands;
        commands.clear_render_target(
            camera_group(viewport_layer::overlay), 0, { .attachment = 0, .color = { 0.0f, 0.0f, 1.0f, 1.0f } });
        const walk_result walked = harness.walk_frame(scene, { .commands = &commands });
        CHECK(walked.statistics.commands == 1);
        CHECK(walked.statistics.draws == 1);
        CHECK(walked.image.at(1, 1)[2] == 255); // cleared to blue
        CHECK(walked.image.at(walked.image.width / 2 + 10, walked.image.height / 2)[0] == 255); // a quad on top
    }

    SECTION("a clear after the draws of the same group erases them")
    {
        draw_commands commands;
        commands.clear_render_target(
            camera_group(viewport_layer::overlay_on_top), 0, { .attachment = 0, .color = { 0.0f, 1.0f, 0.0f, 1.0f } });
        const walk_result walked = harness.walk_frame(scene, { .commands = &commands });
        CHECK(walked.image.count_different({ 0, 255, 0, 255 }) == 0);
    }

    SECTION("set_viewport_scissor limits the following draws; reset_state ends it")
    {
        draw_commands commands;
        const float width = static_cast<float>(harness.fixture.output_size.width);
        const float height = static_cast<float>(harness.fixture.output_size.height);
        commands.set_viewport_scissor(
            camera_group(viewport_layer::overlay),
            0,
            { .viewport = { 0.0f, 0.0f, width, height, 0.0f, 1.0f },
              .scissor = { 0, 0, static_cast<std::int32_t>(width / 2), static_cast<std::int32_t>(height) } });
        const walk_result walked = harness.walk_frame(scene, { .commands = &commands });
        const std::uint32_t y = walked.image.height / 2;
        bool right_half_empty = true;
        for (std::uint32_t x = walked.image.width / 2; x < walked.image.width; ++x)
        {
            right_half_empty = right_half_empty && walked.image.at(x, y)[0] == 0;
        }
        CHECK(right_half_empty);
        CHECK(count_not_black(walked.image) > 0);
        CHECK(walked.statistics.scissor_sets == 1);
    }

    SECTION("set_write_mask draws with the mask's variant")
    {
        draw_commands commands;
        commands.set_write_mask(camera_group(viewport_layer::overlay), 0, write_mask::green | write_mask::alpha);
        const walk_result walked = harness.walk_frame(scene, { .commands = &commands });
        CHECK(walked.statistics.draws == 1);
        CHECK(count_not_black(walked.image) > 0);
        bool no_red = true;
        for (const tests::rgba8 pixel : walked.image.pixels)
        {
            no_red = no_red && pixel[0] == 0;
        }
        CHECK(no_red); // the quads are red: masked away, alpha written
        CHECK(harness.fixture.renderer->pipeline_variants().statistics().write_mask_variants >= 1);
    }

    SECTION("a callback runs with its arguments, and the filter forgets its state")
    {
        struct callback_arguments
        {
            std::uint32_t value = 0;
            std::uint32_t* seen = nullptr;
            std::size_t* targets = nullptr;
        };
        std::uint32_t seen = 0;
        std::size_t targets = 0;
        draw_commands commands;
        commands.callback(
            camera_group(viewport_layer::overlay_on_top),
            0,
            [](command_context& context, const void* arguments)
            {
                callback_arguments copied;
                std::memcpy(&copied, arguments, sizeof(copied));
                *copied.seen = copied.value;
                *copied.targets = context.targets().render_targets.size();
            },
            callback_arguments{ .value = 42, .seen = &seen, .targets = &targets });
        // A second group after the callback: its draw sets its pipeline again
        scene.add(5, mesh, blended_material, placed({ 0.0f, 2.0f, -5.0f }));
        const walk_result walked = harness.walk_frame(scene, { .commands = &commands });
        CHECK(seen == 42);
        CHECK(targets == 1);
        CHECK(walked.statistics.commands == 1);
        CHECK(walked.statistics.pipeline_changes == 2);
        CHECK(walked.statistics.signature_changes == 2); // forgotten by the callback
    }
}

TEST_CASE("a clear command on an undeclared attachment fails a debug check", "[renderer]")
{
    walker_harness harness;
    quad_scene scene;
    const std::uint32_t mesh = scene.add_mesh(red);
    scene.add(1, mesh, opaque_material, placed({ 0.0f, 0.0f, -5.0f }));
    harness.upload_meshes(scene);

    tests::recorded_assertions assertions;
    draw_commands commands;
    // The overlay passes write one colour attachment, and this pass declares no depth write
    commands.clear_render_target(camera_group(viewport_layer::overlay_on_top), 0, { .attachment = 1 });
    commands.clear_depth_stencil(camera_group(viewport_layer::overlay_on_top), 1, {});
    const walk_result walked = harness.walk_frame(scene, { .depth_written = false, .commands = &commands });

    CHECK(walked.statistics.commands == 2);
    CHECK(walked.statistics.skipped_invalid == 2);
    CHECK(walked.statistics.draws == 1);
#ifndef NDEBUG
    CHECK(assertions.count() == 2);
    CHECK(assertions.contains("clear_render_target on attachment 1"));
    CHECK(assertions.contains("clear_depth_stencil"));
#endif
}

TEST_CASE("splitting a range into four groups gives the same image as one", "[renderer]")
{
    walker_harness harness{ 1 }; // one pass per recording group
    quad_scene scene;
    const std::array<std::uint32_t, 3> meshes{ scene.add_mesh(red), scene.add_mesh(green), scene.add_mesh(half_white) };

    // Overlapping quads in random places and priorities, opaque and translucent: the image depends on the draw order
    std::mt19937 random{ 17 };
    std::uniform_real_distribution<float> x{ -4.5f, 4.5f };
    std::uniform_real_distribution<float> y{ -2.5f, 2.5f };
    std::uniform_real_distribution<float> scale{ 0.2f, 0.8f };
    std::uniform_int_distribution<int> pick{ 0, 2 };
    std::uniform_int_distribution<int> priority{ 0, 7 };
    for (scene::object_id id = 1; id <= 600; ++id)
    {
        const std::uint32_t mesh = meshes[static_cast<std::size_t>(pick(random))];
        const std::uint32_t material_index = mesh == meshes[2] ? blended_material : opaque_material;
        scene.add(
            id,
            mesh,
            material_index,
            placed({ x(random), y(random), -5.0f - 0.01f * static_cast<float>(id) }, scale(random)),
            id % 2 == 0 ? scene::draw_layer::overlay : scene::draw_layer::overlay_on_top,
            static_cast<std::uint8_t>(priority(random)));
    }
    harness.upload_meshes(scene);

    // The overlay pass kind tests depth against the cleared (far) depth: everything passes
    const key_range overlays = prefix_range(
        key_prefix(fullscreen_layer::scene, viewport::camera(), viewport_layer::overlay),
        key_prefix(fullscreen_layer::scene, viewport::camera(), viewport_layer::overlay_on_top));
    const walk_result one = harness.walk_frame(scene, { .kind = pass_kind::overlay, .range = overlays, .parts = 1 });
    const walk_result four = harness.walk_frame(scene, { .kind = pass_kind::overlay, .range = overlays, .parts = 4 });

    CHECK(one.parts == 1);
    CHECK(four.parts == 4);
    CHECK(four.recording_groups >= 4);
    CHECK(one.statistics.draws == four.statistics.draws);
    CHECK(one.statistics.instances == 600);
    CHECK(four.statistics.instances == 600);
    // Each part starts with a clean filter
    CHECK(four.statistics.signature_changes == 4);
    CHECK(count_not_black(one.image) > one.image.pixels.size() / 2);
    REQUIRE(one.image.pixels.size() == four.image.pixels.size());
    std::size_t different = 0;
    for (std::size_t index = 0; index < one.image.pixels.size(); ++index)
    {
        const tests::rgba8 a = one.image.pixels[index];
        const tests::rgba8 b = four.image.pixels[index];
        if (a != b)
        {
            ++different;
        }
    }
    CHECK(different == 0);
}

TEST_CASE("the renderer cuts its draw lists into runs and uploads their instance indices", "[renderer]")
{
    tests::renderer_fixture fixture;
    for (int index = 0; index < 3; ++index)
    {
        // Each cube has its own mesh and material: three runs of one
        fixture.add_unit_cube({ -2.0f + 2.0f * static_cast<float>(index), 0.0f, -8.0f });
    }
    fixture.publish();
    const std::array views{ fixture.view("main") };
    REQUIRE(fixture.render_frame(views).has_value());

    const visualizer::rendering::renderer_debug_information information = fixture.renderer->debug_information();
    REQUIRE(information.draw_runs.size() == 1);
    REQUIRE(fixture.renderer->prepared_draw_lists().size() == 1);
    const prepared_draw_list& prepared = fixture.renderer->prepared_draw_lists()[0];
    const draw_list& list = fixture.renderer->draw_lists()[0];
    CHECK(prepared.list == &list);
    CHECK(information.draw_runs[0] == prepared.runs.size());
    CHECK(information.instance_indices == list.items().size() - list.statistics().command_items);
    CHECK(fixture.renderer->instance_indices().size() == information.instance_indices);

    const std::span<const draw_run> runs =
        prepared.runs_of(list.range(key_prefix(camera_group(viewport_layer::world, translucency::opaque))));
    REQUIRE(runs.size() == 3);
    for (const draw_run& run : runs)
    {
        CHECK(run.count == 1);
        CHECK(
            fixture.renderer->instance_indices()[run.instance_offset] ==
            list.records()[list.items()[run.first].value].instance);
    }
    CHECK(fixture.renderer->frame_data().instance_indices.valid());
}
