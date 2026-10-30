// Upload queue: staging ring, tickets, flush (docs/tasks/07-graphics-frames-and-transfers.md §7.5) and asynchronous
// uploads (§7.6). Needs a D3D12 device (WARP is enough). Reads back through hand-recorded graphics-queue copies.

#include "helpers/graphics_fixture.h"
#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/graphics/error.h"
#include "avis/graphics/upload_queue.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/error.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace avis;
using rhi::buffer_usage;
using rhi::queue_type;
using rhi::texture_usage;

namespace
{
    constexpr std::size_t kibibyte = 1024;

    std::vector<std::byte> pattern(const std::size_t size, const std::uint32_t seed)
    {
        std::vector<std::byte> bytes(size);
        for (std::size_t index = 0; index < size; ++index)
        {
            bytes[index] = static_cast<std::byte>((index * 31 + seed * 17 + (index >> 8)) & 0xFF);
        }
        return bytes;
    }

    rhi::buffer_handle gpu_buffer(rhi::resources& resources, const std::uint64_t size)
    {
        auto buffer = resources.create_buffer(
            { .size = size, .usage = buffer_usage::copy_destination | buffer_usage::copy_source, .debug_name = "gpu" });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    rhi::buffer_handle readback_buffer(rhi::resources& resources, const std::uint64_t size)
    {
        auto buffer = resources.create_buffer(
            { .size = size, .usage = buffer_usage::copy_destination, .memory = rhi::memory_type::readback });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    // Copies `source` into a readback buffer on the graphics queue after `waits`, waits, and returns its bytes
    std::vector<std::byte> read_buffer(
        tests::test_rhi& hardware_interface,
        const rhi::buffer_handle source,
        const std::uint64_t size,
        const std::span<const rhi::synchronization_point> waits)
    {
        rhi::resources resources{ *hardware_interface };
        const rhi::buffer_handle readback = readback_buffer(resources, size);
        auto list = rhi::command_lists{ *hardware_interface }.acquire<queue_type::graphics>("read back");
        REQUIRE(list.has_value());
        list->copy_buffer(readback, 0, source, 0, size);
        const auto point =
            rhi::queue<queue_type::graphics>{ *hardware_interface }.submit(std::span{ &*list, 1 }, waits);
        REQUIRE(point.has_value());
        hardware_interface.wait_for(*point);
        const std::span<const std::byte> bytes = resources.mapped_bytes(readback).first(size);
        std::vector<std::byte> copy{ bytes.begin(), bytes.end() };
        resources.destroy(readback);
        return copy;
    }

    // The ticket's point, which must exist (its flush happened)
    rhi::synchronization_point point_of(const graphics::upload_queue& queue, const graphics::upload_ticket ticket)
    {
        const auto point = queue.point(ticket);
        REQUIRE(point.has_value());
        return *point;
    }

    rhi::synchronization_point flushed(graphics::upload_queue& queue)
    {
        const auto point = queue.flush();
        REQUIRE(point.has_value());
        return *point;
    }

    // Tightly packed texels of one subresource: a pattern of `row_size` bytes × rows × depth
    struct texel_data
    {
        std::vector<std::byte> bytes;
        std::uint32_t row_pitch = 0;
        std::uint32_t rows = 0;
        std::uint32_t depth = 0;
        std::uint64_t row_size = 0;
    };

    texel_data texels_for(
        rhi::resources& resources,
        const rhi::texture_handle texture,
        const rhi::subresource target,
        const std::uint32_t seed,
        const std::uint32_t padding = 0)
    {
        const rhi::copy_footprint footprint = resources.copyable_footprint(texture, target);
        texel_data data;
        data.row_size = footprint.row_size;
        data.row_pitch = static_cast<std::uint32_t>(footprint.row_size) + padding;
        data.rows = footprint.row_count;
        data.depth = footprint.depth;
        data.bytes = pattern(static_cast<std::size_t>(data.row_pitch) * data.rows * data.depth, seed);
        return data;
    }

    // Reads one subresource back on the graphics queue and compares its rows with the caller's
    bool texture_matches(
        tests::test_rhi& hardware_interface,
        const rhi::texture_handle texture,
        const rhi::subresource target,
        const texel_data& expected,
        const rhi::synchronization_point uploaded)
    {
        rhi::resources resources{ *hardware_interface };
        const rhi::copy_footprint footprint = resources.copyable_footprint(texture, target);
        const rhi::buffer_handle readback = readback_buffer(resources, footprint.total_size);
        auto list = rhi::command_lists{ *hardware_interface }.acquire<queue_type::graphics>("read texture");
        REQUIRE(list.has_value());
        list->copy_texture_to_buffer(readback, 0, texture, target); // from common: no barrier after the copy queue
        const std::array waits{ uploaded };
        const auto point =
            rhi::queue<queue_type::graphics>{ *hardware_interface }.submit(std::span{ &*list, 1 }, waits);
        REQUIRE(point.has_value());
        hardware_interface.wait_for(*point);

        const std::span<const std::byte> bytes = resources.mapped_bytes(readback);
        bool same = true;
        for (std::uint32_t row = 0; row < expected.rows * expected.depth; ++row)
        {
            same = same && std::memcmp(
                               bytes.data() + row * footprint.row_pitch,
                               expected.bytes.data() + row * expected.row_pitch,
                               expected.row_size) == 0;
        }
        resources.destroy(readback);
        return same;
    }

    rhi::texture_handle texture(rhi::resources& resources, rhi::texture_description description)
    {
        description.usage =
            texture_usage::copy_destination | texture_usage::copy_source | texture_usage::shader_resource;
        auto created = resources.create_texture(description);
        REQUIRE(created.has_value());
        return *created;
    }
} // namespace

TEST_CASE("upload_queue: create checks the ring size", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto created = graphics::upload_queue::create(
        rhi::resources{ *hardware_interface },
        rhi::command_lists{ *hardware_interface },
        rhi::queue<queue_type::copy>{ *hardware_interface },
        rhi::synchronization{ *hardware_interface },
        { .staging_ring_size = 0 });
    REQUIRE_FALSE(created.has_value());
    CHECK(graphics::is_error(created.error(), graphics::error_kind::invalid_argument));
}

TEST_CASE("upload_queue: a buffer upload reads back after its flush", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface);

    const std::vector<std::byte> data = pattern(4 * kibibyte, 1);
    const rhi::buffer_handle destination = gpu_buffer(resources, 8 * kibibyte);
    const auto ticket = queue->upload_buffer(destination, 1024, data);
    REQUIRE(ticket.has_value());
    CHECK(ticket->flush_index == 1);

    flushed(*queue);
    const std::array waits{ point_of(*queue, *ticket) };
    const std::vector<std::byte> bytes = read_buffer(hardware_interface, destination, 8 * kibibyte, waits);
    CHECK(std::memcmp(bytes.data() + 1024, data.data(), data.size()) == 0);
    resources.destroy(destination);
}

TEST_CASE("upload_queue: tickets resolve only through their own flush", "[graphics]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface);
    const rhi::buffer_handle destination = gpu_buffer(resources, 4 * kibibyte);

    const std::vector<std::byte> data = pattern(256, 2);
    const auto ticket = queue->upload_buffer(destination, 0, data);
    REQUIRE(ticket.has_value());
    CHECK_FALSE(queue->point(*ticket).has_value());

    // Other copy-queue work between the upload and its flush doesn't stand in for the flush
    const rhi::synchronization_point marker = tests::submit_marker<queue_type::copy>(hardware_interface);
    hardware_interface.wait_for(marker);
    CHECK_FALSE(queue->point(*ticket).has_value());

    const rhi::synchronization_point flush_point = flushed(*queue);
    CHECK(flush_point.queue == queue_type::copy);
    CHECK(flush_point.value > marker.value);
    CHECK(point_of(*queue, *ticket) == flush_point);

    hardware_interface.wait_for(flush_point);
    flushed(*queue); // nothing queued; retires the completed flush
    CHECK(point_of(*queue, *ticket) == rhi::synchronization_point{});

    // An empty upload has nothing to wait for
    const auto empty = queue->upload_buffer(destination, 0, {});
    REQUIRE(empty.has_value());
    CHECK(empty->flush_index == 0);
    CHECK(point_of(*queue, *empty) == rhi::synchronization_point{});
    CHECK(assertions.count() == 0);

    SECTION("a ticket of another queue")
    {
        const auto other = tests::make_test_upload_queue(hardware_interface);
        const auto foreign = other->upload_buffer(destination, 0, data);
        REQUIRE(foreign.has_value());
        CHECK(foreign->queue_id != ticket->queue_id);
        CHECK_FALSE(queue->point(*foreign).has_value());
#ifndef NDEBUG
        CHECK(assertions.contains("issued by another upload queue"));
#endif
    }
    resources.destroy(destination);
}

TEST_CASE("upload_queue: flush with nothing queued returns the last flush's point", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface);
    const rhi::queue<queue_type::copy> copy_queue{ *hardware_interface };

    CHECK(flushed(*queue) == rhi::synchronization_point{});
    CHECK(copy_queue.last_submitted().value == 0);

    const rhi::buffer_handle destination = gpu_buffer(resources, 1024);
    const std::vector<std::byte> data = pattern(1024, 3);
    REQUIRE(queue->upload_buffer(destination, 0, data).has_value());
    const rhi::synchronization_point first = flushed(*queue);
    CHECK(copy_queue.last_submitted() == first);

    CHECK(flushed(*queue) == first);
    CHECK(copy_queue.last_submitted() == first); // no submission
    hardware_interface.wait_for(first);
    resources.destroy(destination);
}

TEST_CASE("upload_queue: the ring wraps", "[graphics]")
{
    constexpr std::size_t upload_size = 300 * kibibyte;
    constexpr std::size_t uploads = 100;

    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface); // 1 MiB: three uploads fit
    const rhi::buffer_handle destination = gpu_buffer(resources, upload_size * uploads);

    SECTION("with a flush and a wait every third upload, every upload succeeds")
    {
        std::vector<rhi::synchronization_point> points;
        for (std::size_t index = 0; index < uploads; ++index)
        {
            const std::vector<std::byte> data = pattern(upload_size, static_cast<std::uint32_t>(index));
            const auto ticket = queue->upload_buffer(destination, index * upload_size, data);
            REQUIRE(ticket.has_value());
            if (index % 3 == 2 || index + 1 == uploads)
            {
                const rhi::synchronization_point point = flushed(*queue);
                hardware_interface.wait_for(point);
                points.push_back(point);
            }
        }

        const std::vector<std::byte> bytes =
            read_buffer(hardware_interface, destination, upload_size * uploads, std::span{ &points.back(), 1 });
        bool same = true;
        for (std::size_t index = 0; index < uploads; ++index)
        {
            const std::vector<std::byte> expected = pattern(upload_size, static_cast<std::uint32_t>(index));
            same = same && std::memcmp(bytes.data() + index * upload_size, expected.data(), upload_size) == 0;
        }
        CHECK(same);
    }

    SECTION("without flushing, the fourth fails")
    {
        const std::vector<std::byte> data = pattern(upload_size, 4);
        for (std::size_t index = 0; index < 3; ++index)
        {
            REQUIRE(queue->upload_buffer(destination, index * upload_size, data).has_value());
        }
        const auto full = queue->upload_buffer(destination, 3 * upload_size, data);
        REQUIRE_FALSE(full.has_value());
        CHECK(graphics::is_error(full.error(), graphics::error_kind::upload_queue_full));
        hardware_interface.wait_for(flushed(*queue));
    }
    resources.destroy(destination);
}

TEST_CASE("upload_queue: textures read back identical texels", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface);

    SECTION("a full mip chain with tight, unaligned rows")
    {
        // 33 texels × 4 bytes = 132-byte rows: not 256-aligned
        const rhi::texture_handle target =
            texture(resources, { .width = 33, .height = 17, .mip_levels = 0, .format = rhi::format::rgba8_unorm });
        std::vector<texel_data> texels;
        std::vector<graphics::subresource_data> subresources;
        for (std::uint16_t mip = 0; mip < 6; ++mip) // 33 → 16 → 8 → 4 → 2 → 1
        {
            texels.push_back(texels_for(resources, target, { .mip = mip }, mip));
        }
        for (std::uint16_t mip = 0; mip < 6; ++mip)
        {
            subresources.push_back({ .target = { .mip = mip }, .bytes = texels[mip].bytes });
        }
        CHECK(texels[0].row_pitch == 132);
        const auto ticket = queue->upload_texture(target, subresources);
        REQUIRE(ticket.has_value());
        flushed(*queue);
        const rhi::synchronization_point point = point_of(*queue, *ticket);
        for (std::uint16_t mip = 0; mip < 6; ++mip)
        {
            CHECK(texture_matches(hardware_interface, target, { .mip = mip }, texels[mip], point));
        }
        resources.destroy(target);
    }

    SECTION("padded caller rows")
    {
        const rhi::texture_handle target =
            texture(resources, { .width = 20, .height = 9, .format = rhi::format::rgba8_unorm });
        const texel_data texels = texels_for(resources, target, {}, 9, 44); // 80-byte rows in 124-byte pitches
        const std::array subresources{ graphics::subresource_data{
            .target = {}, .bytes = texels.bytes, .row_pitch = texels.row_pitch } };
        const auto ticket = queue->upload_texture(target, subresources);
        REQUIRE(ticket.has_value());
        flushed(*queue);
        CHECK(texture_matches(hardware_interface, target, {}, texels, point_of(*queue, *ticket)));
        resources.destroy(target);
    }

    SECTION("a BC1 texture in block rows")
    {
        const rhi::texture_handle target =
            texture(resources, { .width = 64, .height = 32, .mip_levels = 2, .format = rhi::format::bc1_unorm });
        const texel_data top = texels_for(resources, target, { .mip = 0 }, 5);
        const texel_data second = texels_for(resources, target, { .mip = 1 }, 6);
        CHECK(top.rows == 8); // 32 texels in 4-texel block rows
        CHECK(top.row_size == 16 * 8); // 16 blocks of 8 bytes
        const std::array subresources{ graphics::subresource_data{ .target = { .mip = 0 }, .bytes = top.bytes },
                                       graphics::subresource_data{ .target = { .mip = 1 }, .bytes = second.bytes } };
        const auto ticket = queue->upload_texture(target, subresources);
        REQUIRE(ticket.has_value());
        flushed(*queue);
        const rhi::synchronization_point point = point_of(*queue, *ticket);
        CHECK(texture_matches(hardware_interface, target, { .mip = 0 }, top, point));
        CHECK(texture_matches(hardware_interface, target, { .mip = 1 }, second, point));
        resources.destroy(target);
    }

    SECTION("one face of a cube")
    {
        const rhi::texture_handle target = texture(
            resources,
            { .dimension = rhi::texture_dimension::cube,
              .width = 16,
              .height = 16,
              .format = rhi::format::rgba8_unorm });
        const texel_data face = texels_for(resources, target, { .layer = 3 }, 7);
        const std::array subresources{ graphics::subresource_data{ .target = { .layer = 3 }, .bytes = face.bytes } };
        const auto ticket = queue->upload_texture(target, subresources);
        REQUIRE(ticket.has_value());
        flushed(*queue);
        CHECK(texture_matches(hardware_interface, target, { .layer = 3 }, face, point_of(*queue, *ticket)));
        resources.destroy(target);
    }
}

TEST_CASE("upload_queue: a request larger than half the ring uses a dedicated buffer", "[graphics]")
{
    constexpr std::size_t size = 3 * 1024 * kibibyte;

    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface); // 1 MiB
    const rhi::buffer_handle destination = gpu_buffer(resources, size + 800 * kibibyte);
    auto& lifetime = hardware_interface->state().lifetime();

    // Fill the ring first, past the dedicated upload's range: it doesn't need ring space
    const std::vector<std::byte> ring_data = pattern(400 * kibibyte, 8);
    REQUIRE(queue->upload_buffer(destination, size, ring_data).has_value());
    REQUIRE(queue->upload_buffer(destination, size + 400 * kibibyte, ring_data).has_value());

    const std::vector<std::byte> data = pattern(size, 9);
    const auto ticket = queue->upload_buffer(destination, 0, data);
    REQUIRE(ticket.has_value());
    CHECK(lifetime.pending() == 0);

    const rhi::synchronization_point point = flushed(*queue);
    CHECK(lifetime.pending() >= 1); // the dedicated buffer, destroyed right after the submit

    hardware_interface.wait_for(point);
    CHECK(lifetime.pending() == 0); // freed once the flush completed

    const std::array waits{ point };
    const std::vector<std::byte> bytes = read_buffer(hardware_interface, destination, size + 800 * kibibyte, waits);
    CHECK(std::memcmp(bytes.data(), data.data(), size) == 0);
    CHECK(std::memcmp(bytes.data() + size, ring_data.data(), ring_data.size()) == 0);
    resources.destroy(destination);
}

TEST_CASE("upload_queue: invalid uploads fail with invalid_argument", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface);
    const std::vector<std::byte> data = pattern(256, 10);

    const auto is_invalid = [](const result<graphics::upload_ticket>& outcome)
    { return !outcome.has_value() && graphics::is_error(outcome.error(), graphics::error_kind::invalid_argument); };

    auto upload = resources.create_buffer(
        { .size = 1024, .usage = buffer_usage::copy_source, .memory = rhi::memory_type::upload });
    REQUIRE(upload.has_value());
    CHECK(is_invalid(queue->upload_buffer(*upload, 0, data)));

    const rhi::buffer_handle destination = gpu_buffer(resources, 1024);
    CHECK(is_invalid(queue->upload_buffer(destination, 900, data)));
    CHECK(is_invalid(queue->upload_buffer(destination, 2048, data)));
    CHECK(is_invalid(queue->upload_buffer(rhi::buffer_handle{}, 0, data)));
    CHECK(queue->upload_buffer(destination, 768, data).has_value());

    const rhi::texture_handle target =
        texture(resources, { .width = 8, .height = 8, .format = rhi::format::rgba8_unorm });
    const std::vector<std::byte> texels = pattern(8 * 8 * 4, 11);
    const std::array missing_mip{ graphics::subresource_data{ .target = { .mip = 1 }, .bytes = texels } };
    const std::array missing_layer{ graphics::subresource_data{ .target = { .layer = 1 }, .bytes = texels } };
    const std::array short_data{ graphics::subresource_data{ .target = {}, .bytes = std::span{ texels }.first(100) } };
    const std::array short_pitch{ graphics::subresource_data{ .target = {}, .bytes = texels, .row_pitch = 16 } };
    CHECK(is_invalid(queue->upload_texture(target, missing_mip)));
    CHECK(is_invalid(queue->upload_texture(target, missing_layer)));
    CHECK(is_invalid(queue->upload_texture(target, short_data)));
    CHECK(is_invalid(queue->upload_texture(target, short_pitch)));

    hardware_interface.wait_for(flushed(*queue));
    resources.destroy(target);
    resources.destroy(destination);
    resources.destroy(*upload);
}

TEST_CASE("upload_queue: concurrent uploads and flushes", "[graphics]")
{
    constexpr int threads = 8;
    constexpr int per_thread = 40;
    constexpr std::size_t size = 12 * kibibyte; // 8 × 40 × 12 KiB = 3.75 MiB through a 1 MiB ring

    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface);
    const rhi::buffer_handle destination = gpu_buffer(resources, size * threads * per_thread);

    std::atomic<int> finished{ 0 };
    std::atomic<int> failures{ 0 };
    {
        std::jthread flusher{ [&]
                              {
                                  while (finished.load() < threads)
                                  {
                                      if (!queue->flush().has_value())
                                      {
                                          ++failures;
                                      }
                                      std::this_thread::yield();
                                  }
                              } };
        std::vector<std::jthread> uploaders;
        for (int thread = 0; thread < threads; ++thread)
        {
            uploaders.emplace_back(
                [&, thread]
                {
                    for (int index = 0; index < per_thread; ++index)
                    {
                        const int slot = thread * per_thread + index;
                        const std::vector<std::byte> data = pattern(size, static_cast<std::uint32_t>(slot));
                        while (true)
                        {
                            const auto ticket = queue->upload_buffer(destination, slot * size, data);
                            if (ticket)
                            {
                                break;
                            }
                            if (!graphics::is_error(ticket.error(), graphics::error_kind::upload_queue_full))
                            {
                                ++failures;
                                break;
                            }
                            std::this_thread::yield(); // the flusher frees space once the GPU catches up
                        }
                    }
                    ++finished;
                });
        }
    }
    CHECK(failures == 0);

    const std::array waits{ flushed(*queue) };
    const std::vector<std::byte> bytes =
        read_buffer(hardware_interface, destination, size * threads * per_thread, waits);
    bool same = true;
    for (int slot = 0; slot < threads * per_thread; ++slot)
    {
        const std::vector<std::byte> expected = pattern(size, static_cast<std::uint32_t>(slot));
        same = same && std::memcmp(bytes.data() + slot * size, expected.data(), size) == 0;
    }
    CHECK(same);
    resources.destroy(destination);
}

// ---- asynchronous uploads and back-pressure (7.6) ----

namespace
{
    struct upload_outcome
    {
        std::optional<graphics::upload_ticket> ticket;
        std::optional<avis::error> error;
        std::atomic<int> values{ 0 };
        std::atomic<int> errors{ 0 };
        std::atomic<int> stops{ 0 };
        std::atomic<std::thread::id> thread{};

        int total() const noexcept
        {
            return values.load() + errors.load() + stops.load();
        }
    };

    struct upload_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value(const graphics::upload_ticket ticket) noexcept
        {
            outcome->ticket = ticket;
            record();
            ++outcome->values;
        }

        void set_error(avis::error error) noexcept
        {
            outcome->error.emplace(std::move(error));
            record();
            ++outcome->errors;
        }

        void set_stopped() noexcept
        {
            record();
            ++outcome->stops;
        }

        void record() const noexcept
        {
            outcome->thread = std::this_thread::get_id();
            if (order != nullptr)
            {
                order->push_back(id); // completions run on one thread in these tests
            }
        }

        auto get_env() const noexcept
        {
            return stdexec::prop{ stdexec::get_stop_token, token };
        }

        upload_outcome* outcome;
        stdexec::inplace_stop_token token;
        std::vector<int>* order;
        int id;
    };

    // A connected async upload with its outcome and stop source. Not movable; declare it after the queue.
    class started_upload
    {
    public:
        explicit started_upload(graphics::upload_sender sender, std::vector<int>* order = nullptr, const int id = 0) :
            operation_{ stdexec::connect(
                std::move(sender), upload_receiver{ &outcome, source.get_token(), order, id }) }
        {}

        started_upload(const started_upload&) = delete;
        started_upload& operator=(const started_upload&) = delete;

        void start() noexcept
        {
            stdexec::start(operation_);
        }

        upload_outcome outcome;
        stdexec::inplace_stop_source source;

    private:
        stdexec::connect_result_t<graphics::upload_sender, upload_receiver> operation_;
    };

    constexpr std::size_t small_ring = 64 * kibibyte;
} // namespace

static_assert(avis::sender_of<graphics::upload_sender, graphics::upload_ticket>);

TEST_CASE("upload_queue: an async upload waits for ring space without a thread", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::resources resources{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface, small_ring);
    const rhi::buffer_handle destination = gpu_buffer(resources, 128 * kibibyte);

    // Requests up to half the ring use it (larger ones get a dedicated buffer and never wait): 60 of 64 KiB used
    static_cast<void>(gate.point(queue_type::copy, 1)); // holds the copy queue back
    const std::vector<std::byte> first = pattern(60 * kibibyte, 20);
    REQUIRE(queue->upload_buffer(destination, 0, std::span{ first }.first(30 * kibibyte)).has_value());
    REQUIRE(queue->upload_buffer(destination, 30 * kibibyte, std::span{ first }.subspan(30 * kibibyte)).has_value());
    const rhi::synchronization_point held = flushed(*queue);

    const std::vector<std::byte> second = pattern(30 * kibibyte, 21);
    started_upload waiting{ queue->upload_buffer_async(destination, 64 * kibibyte, second) };
    waiting.start();
    CHECK(waiting.outcome.total() == 0);
    driver.poll_completions();
    CHECK(waiting.outcome.total() == 0);

    gate.open(1);
    hardware_interface.wait_for(held); // polls on this thread
    REQUIRE(waiting.outcome.values == 1);
    CHECK(waiting.outcome.thread.load() == std::this_thread::get_id());
    CHECK(waiting.outcome.ticket->flush_index == 2); // the next flush

    const rhi::synchronization_point second_flush = flushed(*queue);
    CHECK(point_of(*queue, *waiting.outcome.ticket) == second_flush);
    const std::array waits{ second_flush };
    const std::vector<std::byte> bytes = read_buffer(hardware_interface, destination, 128 * kibibyte, waits);
    CHECK(std::memcmp(bytes.data(), first.data(), first.size()) == 0);
    CHECK(std::memcmp(bytes.data() + 64 * kibibyte, second.data(), second.size()) == 0);
    resources.destroy(destination);
}

TEST_CASE("upload_queue: waiters complete in FIFO order, and a large one isn't overtaken", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::resources resources{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface, small_ring);
    const rhi::buffer_handle destination = gpu_buffer(resources, 256 * kibibyte);

    // Two flushes fill the ring, each held by its own gate value: 24 KiB, then 32 + 8 KiB
    const std::vector<std::byte> filler = pattern(32 * kibibyte, 30);
    static_cast<void>(gate.point(queue_type::copy, 1));
    REQUIRE(queue->upload_buffer(destination, 0, std::span{ filler }.first(24 * kibibyte)).has_value());
    const rhi::synchronization_point first_flush = flushed(*queue);
    static_cast<void>(gate.point(queue_type::copy, 2));
    REQUIRE(queue->upload_buffer(destination, 0, filler).has_value());
    REQUIRE(queue->upload_buffer(destination, 32 * kibibyte, std::span{ filler }.first(8 * kibibyte)).has_value());
    const rhi::synchronization_point second_flush = flushed(*queue);

    // A 32 KiB waiter at the front, which doesn't fit the first flush's 24 KiB, then nine 2 KiB ones that would
    std::vector<int> order;
    std::vector<std::vector<std::byte>> data;
    data.push_back(pattern(32 * kibibyte, 31));
    for (int index = 1; index < 10; ++index)
    {
        data.push_back(pattern(2 * kibibyte, 31 + index));
    }
    std::vector<std::unique_ptr<started_upload>> waiters;
    std::uint64_t offset = 64 * kibibyte;
    for (int index = 0; index < 10; ++index)
    {
        waiters.push_back(
            std::make_unique<started_upload>(
                queue->upload_buffer_async(destination, offset, data[index]), &order, index));
        waiters.back()->start();
        offset += data[index].size();
    }
    CHECK(order.empty());

    gate.open(1);
    hardware_interface.wait_for(first_flush);
    driver.poll_completions();
    CHECK(order.empty()); // the large one doesn't fit yet, and the small ones wait behind it

    gate.open(2);
    hardware_interface.wait_for(second_flush);
    CHECK(order == std::vector<int>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    for (const auto& waiter : waiters)
    {
        REQUIRE(waiter->outcome.values == 1);
        CHECK(waiter->outcome.ticket->flush_index == 3);
    }

    const std::array waits{ flushed(*queue) };
    const std::vector<std::byte> bytes = read_buffer(hardware_interface, destination, 256 * kibibyte, waits);
    bool same = true;
    offset = 64 * kibibyte;
    for (const std::vector<std::byte>& expected : data)
    {
        same = same && std::memcmp(bytes.data() + offset, expected.data(), expected.size()) == 0;
        offset += expected.size();
    }
    CHECK(same);
    resources.destroy(destination);
}

TEST_CASE("upload_queue: a stop request completes a queued waiter once", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::resources resources{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const rhi::synchronization synchronization{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface, small_ring);
    const rhi::buffer_handle destination = gpu_buffer(resources, 128 * kibibyte);
    const std::vector<std::byte> data = pattern(30 * kibibyte, 40);
    // Fills 60 of the ring's 64 KiB, so a 30 KiB upload has to wait
    const auto fill = [&]
    {
        REQUIRE(queue->upload_buffer(destination, 0, data).has_value());
        REQUIRE(queue->upload_buffer(destination, 30 * kibibyte, data).has_value());
    };

    SECTION("on the requesting thread, and not again from poll_completions")
    {
        static_cast<void>(gate.point(queue_type::copy, 1));
        fill();
        const rhi::synchronization_point held = flushed(*queue);

        started_upload waiting{ queue->upload_buffer_async(destination, 64 * kibibyte, data) };
        waiting.start();
        REQUIRE(waiting.outcome.total() == 0);
        waiting.source.request_stop();
        CHECK(waiting.outcome.stops == 1);
        CHECK(waiting.outcome.thread.load() == std::this_thread::get_id());

        gate.open(1);
        hardware_interface.wait_for(held);
        driver.poll_completions();
        CHECK(waiting.outcome.total() == 1);
    }

    SECTION("racing poll_completions on another thread")
    {
        constexpr int rounds = 1000;
        int wrong = 0;
        int stopped = 0;
        for (int round = 1; round <= rounds; ++round)
        {
            static_cast<void>(gate.point(queue_type::copy, static_cast<std::uint64_t>(round)));
            fill();
            const rhi::synchronization_point held = flushed(*queue);

            started_upload waiting{ queue->upload_buffer_async(destination, 64 * kibibyte, data) };
            waiting.start();
            wrong += waiting.outcome.total() == 0 ? 0 : 1;
            {
                std::jthread stopper{ [&waiting] { waiting.source.request_stop(); } };
                gate.open(static_cast<std::uint64_t>(round));
                while (!synchronization.is_complete(held))
                {
                    std::this_thread::yield();
                }
                driver.poll_completions();
            }
            wrong += waiting.outcome.total() == 1 ? 0 : 1;
            stopped += waiting.outcome.stops.load();

            // Retire everything, so the next round starts with an empty ring
            const rhi::synchronization_point drained = flushed(*queue);
            hardware_interface.wait_for(drained);
            static_cast<void>(queue->flush());
        }
        CHECK(wrong == 0);
        UNSCOPED_INFO("stopped " << stopped << " of " << rounds << " rounds");
        CHECK(stopped > 0);
    }
    resources.destroy(destination);
}

TEST_CASE("upload_queue: async uploads that fit or are invalid complete inline", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_upload_queue(hardware_interface);
    const rhi::buffer_handle destination = gpu_buffer(resources, 4 * kibibyte);
    const std::vector<std::byte> data = pattern(1024, 50);

    started_upload fits{ queue->upload_buffer_async(destination, 0, data) };
    fits.start();
    REQUIRE(fits.outcome.values == 1);
    CHECK(fits.outcome.ticket->flush_index == 1);

    started_upload invalid{ queue->upload_buffer_async(destination, 4000, data) };
    invalid.start();
    REQUIRE(invalid.outcome.errors == 1);
    CHECK(graphics::is_error(*invalid.outcome.error, graphics::error_kind::invalid_argument));

    const rhi::texture_handle target =
        texture(resources, { .width = 8, .height = 8, .format = rhi::format::rgba8_unorm });
    const std::vector<std::byte> texels = pattern(8 * 8 * 4, 51);
    const std::array subresources{ graphics::subresource_data{ .target = {}, .bytes = texels } };
    started_upload texture_upload{ queue->upload_texture_async(target, subresources) };
    texture_upload.start();
    REQUIRE(texture_upload.outcome.values == 1);
    CHECK(texture_upload.outcome.ticket->flush_index == 1);

    hardware_interface.wait_for(flushed(*queue));
    resources.destroy(target);
    resources.destroy(destination);
}
