// Readback queue (docs/tasks/07-graphics-frames-and-transfers.md §7.7). Needs a D3D12 device (WARP is enough). Writes
// the sources through the upload queue.

#include "helpers/graphics_fixture.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/graphics/error.h"
#include "avis/graphics/readback_queue.h"
#include "avis/graphics/upload_queue.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace avis;
using rhi::buffer_usage;
using rhi::queue_type;
using rhi::texture_usage;

static_assert(avis::sender_of<graphics::readback_buffer_sender, graphics::readback_data>);
static_assert(avis::sender_of<graphics::readback_texture_sender, graphics::readback_image>);

namespace
{
    constexpr std::size_t kibibyte = 1024;
    constexpr std::size_t mebibyte = 1024 * kibibyte;

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

    rhi::synchronization_point flushed(graphics::upload_queue& queue)
    {
        const auto point = queue.flush();
        REQUIRE(point.has_value());
        return *point;
    }

    // A gpu_only buffer of `size` bytes holding `data` at `offset`, written and complete
    rhi::buffer_handle filled_buffer(
        tests::test_rhi& hardware_interface,
        graphics::upload_queue& uploads,
        const std::uint64_t size,
        const std::uint64_t offset,
        const std::span<const std::byte> data)
    {
        rhi::resources resources{ *hardware_interface };
        const rhi::buffer_handle buffer = gpu_buffer(resources, size);
        REQUIRE(uploads.upload_buffer(buffer, offset, data).has_value());
        hardware_interface.wait_for(flushed(uploads));
        return buffer;
    }

    template<class Value>
    struct readback_outcome
    {
        std::optional<Value> value;
        std::optional<avis::error> error;
        int values = 0;
        int errors = 0;
        int stops = 0;

        int total() const noexcept
        {
            return values + errors + stops;
        }
    };

    template<class Value>
    struct readback_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value(Value value) noexcept
        {
            outcome->value.emplace(std::move(value));
            ++outcome->values;
        }

        void set_error(avis::error error) noexcept
        {
            outcome->error.emplace(std::move(error));
            ++outcome->errors;
        }

        void set_stopped() noexcept
        {
            ++outcome->stops;
        }

        auto get_env() const noexcept
        {
            return stdexec::prop{ stdexec::get_stop_token, token };
        }

        readback_outcome<Value>* outcome;
        stdexec::inplace_stop_token token;
    };

    // A connected readback with its outcome and stop source. Not movable; declare it after the readback queue, and
    // destroy the lease it holds before the queue.
    template<class Value>
    class started_readback
    {
    public:
        using sender = graphics::detail::readback_sender<Value>;

        explicit started_readback(sender readback) :
            operation_{ stdexec::connect(
                std::move(readback), readback_receiver<Value>{ &outcome, source.get_token() }) }
        {}

        started_readback(const started_readback&) = delete;
        started_readback& operator=(const started_readback&) = delete;

        void start() noexcept
        {
            stdexec::start(operation_);
        }

        readback_outcome<Value> outcome;
        stdexec::inplace_stop_source source;

    private:
        stdexec::connect_result_t<sender, readback_receiver<Value>> operation_;
    };

    // Waits for the last submission on `queue` (the readback's copy) and polls: the readback completes
    template<class Value>
    void complete(tests::test_rhi& hardware_interface, started_readback<Value>& readback, const queue_type queue)
    {
        rhi::synchronization_point point;
        switch (queue)
        {
        case queue_type::graphics:
            point = rhi::queue<queue_type::graphics>{ *hardware_interface }.last_submitted();
            break;
        case queue_type::compute:
            point = rhi::queue<queue_type::compute>{ *hardware_interface }.last_submitted();
            break;
        case queue_type::copy:
            point = rhi::queue<queue_type::copy>{ *hardware_interface }.last_submitted();
            break;
        }
        hardware_interface.wait_for(point);
        REQUIRE(readback.outcome.total() == 1);
    }

    // Starts a buffer readback on the copy queue and requires it to deliver a lease
    graphics::readback_data read(
        tests::test_rhi& hardware_interface,
        graphics::readback_queue& queue,
        const rhi::buffer_handle source,
        const std::uint64_t size)
    {
        started_readback<graphics::readback_data> readback{ queue.read_buffer(source, 0, size) };
        readback.start();
        complete(hardware_interface, readback, queue_type::copy);
        REQUIRE(readback.outcome.values == 1);
        return std::move(*readback.outcome.value);
    }
} // namespace

TEST_CASE("readback_queue: a buffer read on the copy queue returns the uploaded bytes", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto uploads = tests::make_test_upload_queue(hardware_interface);
    const auto queue = tests::make_test_readback_queue(hardware_interface);

    const std::vector<std::byte> data = pattern(100 * kibibyte, 1);
    const rhi::buffer_handle source = filled_buffer(hardware_interface, *uploads, 128 * kibibyte, 0, data);

    {
        started_readback<graphics::readback_data> readback{ queue->read_buffer(
            source, 1000, 50 * kibibyte, queue_type::copy) };
        readback.start();
        CHECK(readback.outcome.total() == 0); // completes only from poll_completions()
        CHECK(queue->statistics().readbacks == 1);
        complete(hardware_interface, readback, queue_type::copy);

        REQUIRE(readback.outcome.values == 1);
        const std::span<const std::byte> bytes = readback.outcome.value->bytes();
        REQUIRE(bytes.size() == 50 * kibibyte);
        CHECK(std::memcmp(bytes.data(), data.data() + 1000, bytes.size()) == 0);

        const graphics::readback_pool_statistics statistics = queue->statistics();
        CHECK(statistics.readbacks == 0);
        CHECK(statistics.leases == 1);
    }
    CHECK(queue->statistics().leases == 0);
    CHECK(queue->statistics().pooled_buffers == 1);
    resources.destroy(source);
}

TEST_CASE("readback_queue: a texture read on the graphics queue returns the uploaded texels", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto uploads = tests::make_test_upload_queue(hardware_interface);
    const auto queue = tests::make_test_readback_queue(hardware_interface);

    // 40 texels of rgba8 are 160 bytes a row, padded to 256 in the footprint
    constexpr std::uint32_t width = 40;
    constexpr std::uint32_t height = 24;
    constexpr std::uint32_t row_size = width * 4;
    auto texture = resources.create_texture(
        { .width = width,
          .height = height,
          .format = rhi::format::rgba8_unorm,
          .usage = texture_usage::copy_destination | texture_usage::copy_source });
    REQUIRE(texture.has_value());
    const std::vector<std::byte> texels = pattern(std::size_t{ row_size } * height, 2);
    const std::array subresources{ graphics::subresource_data{ .target = {}, .bytes = texels, .row_pitch = row_size } };
    REQUIRE(uploads->upload_texture(*texture, subresources).has_value());
    const rhi::synchronization_point uploaded = flushed(*uploads);

    // The source must be in copy_source on the graphics queue
    auto list = rhi::command_lists{ *hardware_interface }.acquire<queue_type::graphics>("to copy_source");
    REQUIRE(list.has_value());
    const std::array barriers{ rhi::texture_barrier{
        .texture = *texture, .before = rhi::access_states::common, .after = rhi::access_states::copy_source } };
    list->barrier({}, {}, barriers);
    const std::array waits{ uploaded };
    REQUIRE(rhi::queue<queue_type::graphics>{ *hardware_interface }.submit(std::span{ &*list, 1 }, waits).has_value());

    {
        started_readback<graphics::readback_image> readback{ queue->read_texture(*texture) };
        readback.start();
        complete(hardware_interface, readback, queue_type::graphics);

        REQUIRE(readback.outcome.values == 1);
        const graphics::readback_image& image = *readback.outcome.value;
        CHECK(image.width() == width);
        CHECK(image.height() == height);
        CHECK(image.format() == rhi::format::rgba8_unorm);
        CHECK(image.row_pitch() == 256);
        REQUIRE(image.bytes().size() == std::size_t{ image.row_pitch() } * height);
        bool same = true;
        for (std::uint32_t row = 0; row < height; ++row)
        {
            same = same && std::memcmp(
                               image.bytes().data() + std::size_t{ row } * image.row_pitch(),
                               texels.data() + std::size_t{ row } * row_size,
                               row_size) == 0;
        }
        CHECK(same);
    }
    resources.destroy(*texture);
}

TEST_CASE("readback_queue: leases recycle their buffers, and full classes are trimmed", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto uploads = tests::make_test_upload_queue(hardware_interface);
    const auto queue = tests::make_test_readback_queue(hardware_interface);
    const std::vector<std::byte> data = pattern(10 * kibibyte, 3);
    const rhi::buffer_handle source = filled_buffer(hardware_interface, *uploads, 10 * kibibyte, 0, data);

    // Two reads of one size class, one after the other, share one buffer
    {
        const graphics::readback_data first = read(hardware_interface, *queue, source, data.size());
        CHECK(std::memcmp(first.bytes().data(), data.data(), data.size()) == 0);
    }
    {
        const graphics::readback_data second = read(hardware_interface, *queue, source, 4 * kibibyte);
        CHECK(second.bytes().size() == 4 * kibibyte);
        CHECK(std::memcmp(second.bytes().data(), data.data(), 4 * kibibyte) == 0);
    }
    CHECK(queue->statistics().buffers_created == 1);
    CHECK(queue->statistics().pooled_buffers == 1);

    // Five leases held at once: four new buffers, and one is trimmed when they all return
    {
        std::vector<graphics::readback_data> leases;
        for (int index = 0; index < 5; ++index)
        {
            leases.push_back(read(hardware_interface, *queue, source, data.size()));
        }
        CHECK(queue->statistics().buffers_created == 5);
        CHECK(queue->statistics().pooled_buffers == 0);
        CHECK(queue->statistics().leases == 5);

        // A moved-from lease returns nothing
        graphics::readback_data moved = std::move(leases.back());
        leases.pop_back();
        CHECK(queue->statistics().leases == 5);
        CHECK(std::memcmp(moved.bytes().data(), data.data(), data.size()) == 0);
    }
    const graphics::readback_pool_statistics statistics = queue->statistics();
    CHECK(statistics.leases == 0);
    CHECK(statistics.pooled_buffers == graphics::readback_queue::buffers_per_class);
    CHECK(statistics.buffers_destroyed == 1);
    resources.destroy(source);
}

TEST_CASE("readback_queue: a read larger than the largest class gets an exact-size buffer", "[graphics]")
{
    constexpr std::uint64_t size = 200 * mebibyte;

    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto uploads = tests::make_test_upload_queue(hardware_interface);
    const auto queue = tests::make_test_readback_queue(hardware_interface);
    const std::vector<std::byte> data = pattern(64 * kibibyte, 4);
    const rhi::buffer_handle source = filled_buffer(hardware_interface, *uploads, size, size - data.size(), data);

    {
        const graphics::readback_data lease = read(hardware_interface, *queue, source, size);
        REQUIRE(lease.bytes().size() == size);
        CHECK(std::memcmp(lease.bytes().data() + size - data.size(), data.data(), data.size()) == 0);
    }
    const graphics::readback_pool_statistics statistics = queue->statistics();
    CHECK(statistics.buffers_created == 1);
    CHECK(statistics.buffers_destroyed == 1);
    CHECK(statistics.pooled_buffers == 0);
    resources.destroy(source);
}

TEST_CASE("readback_queue: a stopped read destroys its buffer instead of pooling it", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::resources resources{ *hardware_interface };
    const auto uploads = tests::make_test_upload_queue(hardware_interface);
    const auto queue = tests::make_test_readback_queue(hardware_interface);
    const std::vector<std::byte> data = pattern(4 * kibibyte, 5);
    const rhi::buffer_handle source = filled_buffer(hardware_interface, *uploads, 4 * kibibyte, 0, data);
    auto& lifetime = hardware_interface->state().lifetime();

    SECTION("while the copy is in flight")
    {
        static_cast<void>(gate.point(queue_type::copy, 1)); // holds the copy back
        started_readback<graphics::readback_data> readback{ queue->read_buffer(source, 0, data.size()) };
        readback.start();
        REQUIRE(readback.outcome.total() == 0);
        CHECK(lifetime.pending() == 0);

        readback.source.request_stop();
        CHECK(readback.outcome.stops == 1);
        graphics::readback_pool_statistics statistics = queue->statistics();
        CHECK(statistics.readbacks == 0);
        CHECK(statistics.leases == 0);
        CHECK(statistics.buffers_created == 1);
        CHECK(statistics.buffers_destroyed == 1);
        CHECK(statistics.pooled_buffers == 0);
        CHECK(lifetime.pending() >= 1); // the GPU may still write it

        gate.open(1);
        hardware_interface.wait_for(tests::submit_marker<queue_type::copy>(hardware_interface));
        CHECK(lifetime.pending() == 0); // freed once the copy completed
        CHECK(readback.outcome.total() == 1);
    }

    SECTION("before it starts: nothing is submitted")
    {
        started_readback<graphics::readback_data> readback{ queue->read_buffer(source, 0, data.size()) };
        readback.source.request_stop();
        readback.start();
        CHECK(readback.outcome.stops == 1);
        CHECK(queue->statistics().buffers_created == 0);
    }
    resources.destroy(source);
}

TEST_CASE("readback_queue: invalid reads complete with invalid_argument", "[graphics]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const auto queue = tests::make_test_readback_queue(hardware_interface);
    const rhi::buffer_handle source = gpu_buffer(resources, 4 * kibibyte);
    auto texture = resources.create_texture({ .width = 8, .height = 8, .usage = texture_usage::copy_source });
    REQUIRE(texture.has_value());

    const auto fails = [&](graphics::readback_buffer_sender sender)
    {
        started_readback<graphics::readback_data> readback{ std::move(sender) };
        readback.start();
        REQUIRE(readback.outcome.errors == 1);
        CHECK(graphics::is_error(*readback.outcome.error, graphics::error_kind::invalid_argument));
    };
    fails(queue->read_buffer(source, 4000, 200));
    fails(queue->read_buffer(source, 0, 0));
    fails(queue->read_buffer({}, 0, 16));

    started_readback<graphics::readback_image> outside{ queue->read_texture(*texture, { .mip = 1 }) };
    outside.start();
    REQUIRE(outside.outcome.errors == 1);
    CHECK(graphics::is_error(*outside.outcome.error, graphics::error_kind::invalid_argument));

    const graphics::readback_pool_statistics statistics = queue->statistics();
    CHECK(statistics.buffers_created == 0);
    CHECK(statistics.readbacks == 0);
    resources.destroy(*texture);
    resources.destroy(source);
}
