// Mesh residency (docs/tasks/11-renderer.md §11.3, 11 §3.6): new versions become current only after their flush,
// ranges are freed only once no frame can read them, a full staging ring or arena defers the upload to the next frame,
// and the CPU data is let go once it's in the ring. On a device, driving update / flush / after_flush / retire by hand
// the way the renderer's prepare stage does; the last test goes through the renderer itself.

#include "helpers/graphics_fixture.h"
#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"
#include "helpers/renderer_fixture.h"
#include "helpers/rhi_fixture.h"
#include "rendering/geometry_arena.h"
#include "rendering/residency_cache.h"
#include "rendering/scene_renderer.h"

#include "avis/graphics/error.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/graphics/upload_queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/snapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;

namespace
{
    // `vertices` vertices (a multiple of 3) with normals and uvs, one triangle per three, shifted by `offset` so that
    // versions differ
    scene::mesh_data make_mesh(const std::uint32_t vertices, const float offset = 0.0f)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        for (std::uint32_t vertex = 0; vertex < vertices; ++vertex)
        {
            builder.add_vertex({ static_cast<float>(vertex), offset, 0.0f })
                .normal(Eigen::Vector3f::UnitZ())
                .uv({ 0.0f, 0.0f });
            builder.add_index(vertex);
        }
        result<scene::mesh_data> built = std::move(builder).build();
        REQUIRE(built.has_value());
        return std::move(*built);
    }

    // The renderer's residency without the renderer: the RHI, an upload queue, a 1 MiB + 256 KiB arena and the
    // cache, with an asset cache for the CPU versions. Destroyed in reverse: the cache, the arena, the queue, the RHI.
    struct residency_setup
    {
        explicit residency_setup(
            const std::size_t ring_size = tests::test_staging_ring_size,
            const geometry_arena_capacity capacity = { .vertex_bytes = 1 << 20, .index_bytes = 256 << 10 }) :
            uploads{ tests::make_test_upload_queue(hardware_interface, ring_size) },
            arena{ make_arena(hardware_interface, capacity) },
            residency{ std::make_unique<residency_cache>(
                *arena, *uploads, rhi::synchronization{ *hardware_interface }) }
        {}

        ~residency_setup()
        {
            hardware_interface->wait_idle();
            residency.reset();
            for (const scene::mesh_handle mesh : meshes)
            {
                assets.release(mesh);
            }
        }

        static std::unique_ptr<geometry_arena>
            make_arena(tests::test_rhi& hardware_interface, const geometry_arena_capacity& capacity)
        {
            auto created = geometry_arena::create(rhi::resources{ *hardware_interface }, capacity);
            REQUIRE(created.has_value());
            return std::move(*created);
        }

        scene::mesh_handle create_mesh(scene::mesh_data data)
        {
            const result<scene::mesh_handle> mesh = assets.create_mesh(std::move(data));
            REQUIRE(mesh.has_value());
            meshes.push_back(*mesh);
            return *mesh;
        }

        // A snapshot holding the current version of each mesh, as make_snapshot lists them
        scene::snapshot snapshot_of(const std::vector<scene::mesh_handle>& listed) const
        {
            scene::snapshot made;
            for (const scene::mesh_handle mesh : listed)
            {
                made.meshes.push_back({ .handle = mesh, .mesh = assets.current(mesh) });
            }
            std::ranges::sort(made.meshes, {}, &scene::versioned_mesh_entry::handle);
            return made;
        }

        // One prepare stage: update, flush, after_flush, for the next frame index
        void prepare(const scene::snapshot& snapshot)
        {
            residency->update(snapshot, ++frame);
            REQUIRE(uploads->flush().has_value());
            residency->after_flush();
        }

        void wait_for_gpu()
        {
            hardware_interface->wait_idle();
        }

        std::uint64_t current_version(const scene::mesh_handle mesh) const
        {
            const resident_mesh* resident = residency->find(mesh);
            return resident == nullptr ? 0 : resident->version;
        }

        tests::test_rhi hardware_interface;
        std::unique_ptr<graphics::upload_queue> uploads;
        std::unique_ptr<geometry_arena> arena;
        std::unique_ptr<residency_cache> residency;
        scene::asset_cache assets;
        std::vector<scene::mesh_handle> meshes;
        std::uint64_t frame = 0;
    };

    std::vector<std::uint64_t> iota_versions(const std::uint64_t first, const std::uint64_t last)
    {
        std::vector<std::uint64_t> versions(last - first + 1);
        std::iota(versions.begin(), versions.end(), first);
        return versions;
    }
} // namespace

TEST_CASE("a resident mesh has its range, counts, attributes and bounds", "[renderer]")
{
    residency_setup setup;
    const scene::mesh_handle mesh = setup.create_mesh(make_mesh(30));
    setup.prepare(setup.snapshot_of({ mesh }));

    const resident_mesh* resident = setup.residency->find(mesh);
    REQUIRE(resident != nullptr);
    CHECK(resident->version == 1);
    CHECK(resident->vertex_count == 30);
    CHECK(resident->index_count == 30);
    CHECK(resident->attributes == (scene::vertex_attribute::normal | scene::vertex_attribute::uv0));
    CHECK(resident->topology == scene::mesh_topology::triangles);
    CHECK_FALSE(resident->progressive);
    CHECK(resident->primitives.size() == 1);
    CHECK(resident->bounds.minimum.isApprox(Eigen::Vector3f(0.0f, 0.0f, 0.0f)));
    CHECK(resident->bounds.maximum.isApprox(Eigen::Vector3f(29.0f, 0.0f, 0.0f)));

    const vertex_stream_layout layout = layout_vertex_streams(30, resident->attributes);
    CHECK(resident->range.vertex_size == layout.size);
    CHECK(resident->range.index_size == 120);
    CHECK(resident->range.streams.normal == resident->range.vertex_offset + layout.offsets.normal);
    CHECK(resident->range.streams.uv0 == resident->range.vertex_offset + layout.offsets.uv0);

    // Drawn in this frame, after the copy-queue point of its flush
    CHECK(resident->last_used_frame == 1);
    CHECK(resident->ready_after.queue == rhi::queue_type::copy);
    CHECK(resident->ready_after.value != 0);
    CHECK(setup.residency->frame_ready_after() == resident->ready_after);

    const residency_statistics statistics = setup.residency->statistics();
    CHECK(statistics.meshes == 1);
    CHECK(statistics.resident_versions == 1);
    CHECK(statistics.pending_uploads == 0);
    CHECK(statistics.uploaded_bytes == 30 * 12 + 30 * 4 + 30 * 4 + 30 * 4); // positions, normals, uvs, indices
}

TEST_CASE("a new version draws only after its flush, and the previous one is freed after retire", "[renderer]")
{
    residency_setup setup;
    const scene::mesh_handle mesh = setup.create_mesh(make_mesh(30));
    setup.prepare(setup.snapshot_of({ mesh })); // frame 1: version 1
    REQUIRE(setup.current_version(mesh) == 1);

    REQUIRE(setup.assets.update_mesh(mesh, make_mesh(60, 1.0f)).has_value());
    const scene::snapshot second = setup.snapshot_of({ mesh });

    // Queued but not flushed: version 1 keeps drawing
    setup.residency->update(second, ++setup.frame);
    CHECK(setup.current_version(mesh) == 1);
    CHECK(setup.residency->statistics().pending_uploads == 1);

    REQUIRE(setup.uploads->flush().has_value());
    CHECK(setup.current_version(mesh) == 1); // after_flush makes it current
    setup.residency->after_flush();
    CHECK(setup.current_version(mesh) == 2);
    CHECK(setup.residency->find(mesh)->vertex_count == 60);
    CHECK(setup.residency->versions(mesh) == std::vector<std::uint64_t>{ 1, 2 });
    CHECK(setup.arena->statistics().ranges == 2);

    // Version 1 was last drawn by frame 1
    setup.wait_for_gpu();
    setup.residency->retire(0);
    CHECK(setup.residency->versions(mesh) == std::vector<std::uint64_t>{ 1, 2 });
    setup.residency->retire(1);
    CHECK(setup.residency->versions(mesh) == std::vector<std::uint64_t>{ 2 });
    CHECK(setup.arena->statistics().ranges == 1);

    // The current version stays however far frames retire
    setup.residency->retire(100);
    CHECK(setup.current_version(mesh) == 2);
}

TEST_CASE("ten versions published while no frame completes keep their ranges until frames retire", "[renderer]")
{
    residency_setup setup;
    const scene::mesh_handle mesh = setup.create_mesh(make_mesh(30));
    for (int version = 1; version <= 10; ++version)
    {
        if (version > 1)
        {
            REQUIRE(setup.assets.update_mesh(mesh, make_mesh(30, static_cast<float>(version))).has_value());
        }
        setup.prepare(setup.snapshot_of({ mesh })); // frame `version`
        setup.residency->retire(0); // nothing completed yet
        CHECK(setup.current_version(mesh) == static_cast<std::uint64_t>(version));
    }
    CHECK(setup.residency->versions(mesh) == iota_versions(1, 10));
    CHECK(setup.arena->statistics().ranges == 10);

    // Version k was last drawn by frame k, so retiring frame k frees it, in order
    setup.wait_for_gpu();
    for (std::uint64_t completed = 1; completed <= 9; ++completed)
    {
        setup.residency->retire(completed);
        CHECK(setup.residency->versions(mesh) == iota_versions(completed + 1, 10));
    }
    setup.residency->retire(10);
    CHECK(setup.residency->versions(mesh) == std::vector<std::uint64_t>{ 10 });
    CHECK(setup.arena->statistics().ranges == 1);
}

TEST_CASE("a full staging ring defers the upload, and the previous version keeps drawing", "[renderer]")
{
    tests::log_capture capture{ "renderer" };
    residency_setup setup{ std::size_t{ 64 } << 10 }; // a 64 KiB ring
    const scene::mesh_handle mesh = setup.create_mesh(make_mesh(30));
    setup.prepare(setup.snapshot_of({ mesh }));
    REQUIRE(setup.current_version(mesh) == 1);
    setup.wait_for_gpu();

    // Fill the ring with small copies into a scratch buffer, so the next stream doesn't fit
    rhi::resources resources{ *setup.hardware_interface };
    auto scratch = resources.create_buffer(
        { .size = 4096,
          .usage = rhi::buffer_usage::copy_destination,
          .memory = rhi::memory_type::gpu_only,
          .debug_name = "ring filler" });
    REQUIRE(scratch.has_value());
    const std::vector<std::byte> chunk(256);
    bool full = false;
    for (int copy = 0; copy < 1000 && !full; ++copy)
    {
        const result<graphics::upload_ticket> ticket = setup.uploads->upload_buffer(*scratch, 0, chunk);
        if (!ticket)
        {
            REQUIRE(graphics::is_error(ticket.error(), graphics::error_kind::upload_queue_full));
            full = true;
        }
    }
    REQUIRE(full);

    // Version 2 and a new mesh can't be uploaded this frame
    REQUIRE(setup.assets.update_mesh(mesh, make_mesh(300, 1.0f)).has_value());
    const scene::mesh_handle fresh = setup.create_mesh(make_mesh(300, 2.0f));
    const scene::snapshot listed = setup.snapshot_of({ mesh, fresh });
    setup.prepare(listed);
    CHECK(setup.current_version(mesh) == 1);
    CHECK(setup.residency->find(fresh) == nullptr); // nothing resident: not drawn
    CHECK(setup.residency->statistics().pending_uploads == 2);
    CHECK(setup.residency->statistics().failed_uploads >= 2);
    CHECK(capture.contains("not made resident this frame: upload failed"));

    // Next frame, once the flush that filled the ring is done, the uploads go through
    setup.wait_for_gpu();
    setup.prepare(listed);
    CHECK(setup.current_version(mesh) == 2);
    CHECK(setup.current_version(fresh) == 1);
    CHECK(setup.residency->statistics().pending_uploads == 0);

    resources.destroy(*scratch);
}

TEST_CASE("a full arena defers the upload, with a warning at most once a second", "[renderer]")
{
    tests::log_capture capture{ "renderer" };
    // Room for one 30-vertex mesh (with normals and uvs: 624 bytes), not for a 300-vertex one
    residency_setup setup{ tests::test_staging_ring_size, { .vertex_bytes = 1024, .index_bytes = 4096 } };
    const scene::mesh_handle small = setup.create_mesh(make_mesh(30));
    const scene::mesh_handle large = setup.create_mesh(make_mesh(300));
    const scene::snapshot listed = setup.snapshot_of({ small, large });

    setup.prepare(listed);
    CHECK(setup.current_version(small) == 1);
    CHECK(setup.residency->find(large) == nullptr);
    CHECK(capture.contains("the geometry arena's vertex buffer has no free block"));

    const auto warnings = [&]
    {
        return std::ranges::count_if(
            capture.lines(), [](const std::string& line) { return line.contains("not made resident"); });
    };
    CHECK(warnings() == 1);
    setup.prepare(listed); // retried, still no room, and no second warning within a second
    CHECK(setup.residency->find(large) == nullptr);
    CHECK(warnings() == 1);
    CHECK(setup.residency->statistics().failed_uploads == 2);
}

TEST_CASE("residency lets go of the CPU data once it's in the staging ring", "[renderer]")
{
    residency_setup setup;
    const scene::mesh_handle mesh = setup.create_mesh(make_mesh(300));
    scene::snapshot listed = setup.snapshot_of({ mesh });

    // Version 1 is superseded; only the snapshot holds it now
    REQUIRE(setup.assets.update_mesh(mesh, make_mesh(300, 1.0f)).has_value());
    const std::uint64_t retained = setup.assets.statistics().retained_geometry_bytes;
    CHECK(retained == listed.meshes[0].mesh.data->byte_size());

    setup.prepare(listed);
    REQUIRE(setup.current_version(mesh) == 1);
    CHECK(setup.assets.statistics().retained_geometry_bytes == retained);

    // Releasing the snapshot frees version 1's CPU data: residency kept no reference to it
    listed.clear();
    CHECK(setup.assets.statistics().retained_geometry_bytes == 0);
    CHECK(setup.current_version(mesh) == 1); // the GPU copy stays
}

TEST_CASE("a mesh that leaves the snapshot is freed once its last frame retires", "[renderer]")
{
    residency_setup setup;
    const scene::mesh_handle kept = setup.create_mesh(make_mesh(30));
    const scene::mesh_handle dropped = setup.create_mesh(make_mesh(30, 1.0f));
    setup.prepare(setup.snapshot_of({ kept, dropped })); // frame 1
    setup.prepare(setup.snapshot_of({ kept })); // frame 2: `dropped` isn't drawn
    setup.wait_for_gpu();

    setup.residency->retire(0);
    CHECK(setup.residency->versions(dropped) == std::vector<std::uint64_t>{ 1 });
    setup.residency->retire(1);
    CHECK(setup.residency->versions(dropped).empty());
    CHECK(setup.residency->find(dropped) == nullptr);
    CHECK(setup.current_version(kept) == 1);
    CHECK(setup.arena->statistics().ranges == 1);
}

TEST_CASE("an upload ticket resolved before its flush is a debug check", "[renderer]")
{
    tests::recorded_assertions assertions;
    residency_setup setup;
    const scene::mesh_handle mesh = setup.create_mesh(make_mesh(30));
    const scene::snapshot listed = setup.snapshot_of({ mesh });

    setup.residency->update(listed, ++setup.frame);
    setup.residency->after_flush(); // no flush in between
    CHECK(setup.residency->find(mesh) == nullptr);
#ifndef NDEBUG
    CHECK(assertions.contains("resolved to nullopt"));
#endif

    // The tickets were kept: the next after_flush, after a flush, resolves them
    REQUIRE(setup.uploads->flush().has_value());
    setup.residency->after_flush();
    CHECK(setup.current_version(mesh) == 1);
}

TEST_CASE_METHOD(
    tests::renderer_fixture, "the renderer makes the snapshot's meshes resident and imports the arena", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    publish();
    scene::mesh_handle cube;
    {
        const scene::snapshot_reference snapshot = snapshots.acquire();
        REQUIRE(snapshot->meshes.size() == 1);
        cube = snapshot->meshes[0].handle;
    }

    const std::vector views{ view() };
    render_and_read(views);
    const resident_mesh* resident = renderer->residency().find(cube);
    REQUIRE(resident != nullptr);
    CHECK(resident->vertex_count == 24);
    CHECK(resident->index_count == 36);

    const renderer_debug_information information = renderer->debug_information();
    CHECK(information.residency.meshes == 1);
    CHECK(information.residency.pending_uploads == 0);
    CHECK(information.residency.arena.ranges == 1);
    REQUIRE(information.graph != nullptr);
    for (const std::string_view name : { "geometry_vertices", "geometry_indices" })
    {
        const auto found =
            std::ranges::find(information.graph->resources, name, &graphics::resource_debug_information::name);
        REQUIRE(found != information.graph->resources.end());
        CHECK(found->imported);
        CHECK_FALSE(found->is_texture);
    }

    // Destroyed: once the frames that drew it retire, its range is freed
    for (const auto& [entity, mesh] : world.registry().view<scene::components::mesh>().each())
    {
        world.destroy_entity(entity);
        break;
    }
    publish();
    render_and_read(views, 4);
    CHECK(renderer->residency().find(cube) == nullptr);
    CHECK(renderer->debug_information().residency.arena.ranges == 0);
}
