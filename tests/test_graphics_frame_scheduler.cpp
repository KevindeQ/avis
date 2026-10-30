// Frame scheduler: pacing, the frame token and the per-frame services (docs/tasks/07-graphics-frames-and-transfers.md
// §7.2, §7.3). Needs a D3D12 device (WARP is enough). The tests drive completions themselves.

#include "helpers/graphics_fixture.h"
#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"
#include "rhi_test_compute_shader.h"

#include "avis/error.h"
#include "avis/graphics/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/error.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/root_signature.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace avis;
using rhi::buffer_usage;
using rhi::queue_type;
using rhi::texture_usage;

namespace
{
    std::size_t count_lines(const tests::log_capture& capture, const std::string& text)
    {
        const std::vector<std::string> lines = capture.lines();
        return static_cast<std::size_t>(
            std::ranges::count_if(lines, [&text](const std::string& line) { return line.contains(text); }));
    }

    constexpr const char* abandoned_message = "ended by its token without end_frame";

    // Could complete with set_value(), but always completes with set_stopped(): the "other branch" of a when_all that
    // stops after begin_frame delivered its token (sync_wait needs a value completion to accept the when_all)
    struct stopping_sender
    {
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;

        template<class Receiver>
        struct operation
        {
            using operation_state_concept = stdexec::operation_state_tag;

            void start() & noexcept
            {
                stdexec::set_stopped(std::move(receiver));
            }

            Receiver receiver;
        };

        template<stdexec::receiver Receiver>
        operation<Receiver> connect(Receiver receiver) const
        {
            return { std::move(receiver) };
        }
    };

    std::vector<rhi::synchronization_point> points_of(const graphics::frame_token& frame)
    {
        const auto points = frame.recycled_submissions();
        return { points.begin(), points.end() };
    }
} // namespace

static_assert(avis::sender_of<graphics::frame_sender, graphics::frame_token>);

TEST_CASE("frame_scheduler: create checks its configuration", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const rhi::synchronization synchronization{ *hardware_interface };
    const rhi::transient_objects transient{ *hardware_interface };

    const auto created = [&](const graphics::frame_scheduler_configuration& configuration)
    { return graphics::frame_scheduler::create(synchronization, transient, configuration); };

    for (const std::uint32_t frames : { 1u, 2u, 3u })
    {
        const auto scheduler = created(tests::test_frame_scheduler_configuration(frames));
        REQUIRE(scheduler.has_value());
        CHECK((*scheduler)->frames_in_flight() == frames);
    }

    for (const std::uint32_t frames : { 0u, 4u })
    {
        const auto failed = created(tests::test_frame_scheduler_configuration(frames));
        REQUIRE_FALSE(failed.has_value());
        CHECK(graphics::is_error(failed.error(), graphics::error_kind::invalid_argument));
    }

    auto configuration = tests::test_frame_scheduler_configuration();
    configuration.upload_memory_per_frame = 0;
    CHECK(graphics::is_error(created(configuration).error(), graphics::error_kind::invalid_argument));
    configuration = tests::test_frame_scheduler_configuration();
    configuration.descriptors_per_frame = 0;
    CHECK(graphics::is_error(created(configuration).error(), graphics::error_kind::invalid_argument));
}

TEST_CASE(
    "frame_scheduler: descriptor ranges that don't fit the transient region pass the RHI's error through", "[graphics]")
{
    rhi::render_hardware_interface_configuration few_descriptors;
    few_descriptors.descriptor_heaps.transient_descriptor_count = 100;
    tests::test_rhi hardware_interface{ few_descriptors };

    auto configuration = tests::test_frame_scheduler_configuration(2);
    configuration.descriptors_per_frame = 60; // 2 × 60 > 100
    const auto failed = graphics::frame_scheduler::create(
        rhi::synchronization{ *hardware_interface }, rhi::transient_objects{ *hardware_interface }, configuration);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().domain == error_domain::rhi);
}

TEST_CASE(
    "frame_scheduler: frame N + F waits for frame N's submissions and completes from poll_completions", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    graphics::frame_token first = tests::begin_frame_inline(*scheduler);
    CHECK(first.index() == 1);
    CHECK(first.context_index() == 1);
    CHECK(first.completed_index() == 0);
    CHECK(first.recycled_submissions().empty());
    const rhi::synchronization_point held = gate.point(queue_type::graphics, 1);
    const std::array first_points{ held };
    scheduler->end_frame(std::move(first), first_points);

    graphics::frame_token second = tests::begin_frame_inline(*scheduler);
    CHECK(second.index() == 2);
    CHECK(second.context_index() == 0);
    CHECK(second.recycled_submissions().empty());
    scheduler->end_frame(std::move(second), {});

    tests::started_frame third{ scheduler->begin_frame() };
    third.start();
    CHECK(third.outcome.total() == 0);
    driver.poll_completions();
    CHECK(third.outcome.total() == 0);

    gate.open(1);
    hardware_interface.wait_for(held); // waits on the fence, then polls on this thread
    REQUIRE(third.outcome.values == 1);
    CHECK(third.outcome.thread == std::this_thread::get_id());

    graphics::frame_token& token = *third.outcome.token;
    CHECK(token.index() == 3);
    CHECK(token.context_index() == 1);
    CHECK(token.completed_index() == 2); // frame 2 had no submissions, so it is complete too
    CHECK(points_of(token) == std::vector{ held });
    scheduler->end_frame(std::move(token), {});
}

TEST_CASE("frame_scheduler: indices, completed index and recycled submissions over many frames", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 3);

    std::vector<std::vector<rhi::synchronization_point>> recorded{ {} }; // by frame index; frame 0 unused
    for (std::uint64_t index = 1; index <= 9; ++index)
    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        CHECK(frame.index() == index);
        CHECK(frame.context_index() == index % 3);
        // Every earlier frame is complete: the newest one is reported, which is at least N − F
        CHECK(frame.completed_index() == index - 1);

        if (index <= 3)
        {
            CHECK(frame.recycled_submissions().empty());
        }
        else
        {
            CHECK(points_of(frame) == recorded[index - 3]);
        }

        // Two graphics points (only the latest is kept) and, on odd frames, a compute point
        const rhi::synchronization_point early = tests::submit_marker<queue_type::graphics>(hardware_interface);
        const rhi::synchronization_point late = tests::submit_marker<queue_type::graphics>(hardware_interface);
        std::vector<rhi::synchronization_point> submitted{ late, early };
        std::vector<rhi::synchronization_point> expected{ late };
        if (index % 2 == 1)
        {
            const rhi::synchronization_point compute = tests::submit_marker<queue_type::compute>(hardware_interface);
            submitted.push_back(compute);
            expected.push_back(compute);
        }
        recorded.push_back(expected);

        hardware_interface.wait_for(late);
        for (const rhi::synchronization_point point : expected)
        {
            hardware_interface.wait_for(point);
        }
        scheduler->end_frame(std::move(frame), submitted);
    }
}

TEST_CASE("frame_scheduler: try_begin_frame never waits", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    auto first = scheduler->try_begin_frame();
    REQUIRE(first.has_value());
    const std::array held{ gate.point(queue_type::compute, 1) };
    scheduler->end_frame(std::move(*first), held);

    auto second = scheduler->try_begin_frame();
    REQUIRE(second.has_value());
    CHECK(second->index() == 2);
    scheduler->end_frame(std::move(*second), {});

    CHECK_FALSE(scheduler->try_begin_frame().has_value());
    CHECK_FALSE(scheduler->try_begin_frame().has_value());

    gate.open(1);
    hardware_interface.wait_for(held[0]);
    auto third = scheduler->try_begin_frame();
    REQUIRE(third.has_value());
    CHECK(third->index() == 3);
    CHECK(points_of(*third) == std::vector{ held[0] });
    scheduler->end_frame(std::move(*third), {});

    // begin_frame and try_begin_frame share the pacing
    graphics::frame_token fourth = tests::begin_frame_inline(*scheduler);
    CHECK(fourth.index() == 4);
}

TEST_CASE("frame_scheduler: stopped and abandoned frames", "[graphics]")
{
    tests::log_capture graphics_log{ "graphics" };
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 1);

    SECTION("end_frame with no submissions frees the context at once, also with an unsubmitted list")
    {
        {
            graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
            auto list = rhi::command_lists{ *hardware_interface }.acquire<queue_type::graphics>("never submitted");
            REQUIRE(list.has_value());
            scheduler->end_frame(std::move(frame), {});
        } // the RHI discards the unsubmitted list

        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
        scheduler->end_frame(std::move(next), {});
        CHECK(count_lines(graphics_log, abandoned_message) == 0);
    }

    SECTION("dropping a token ends its frame")
    {
        {
            graphics::frame_token dropped = tests::begin_frame_inline(*scheduler);
            CHECK(dropped.index() == 1);
        }
        CHECK(count_lines(graphics_log, "frame 1 ended by its token without end_frame") == 1);
        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
    }

    SECTION("move-assigning over a token ends its frame; a moved-from token ends nothing")
    {
        graphics::frame_token moved_from = tests::begin_frame_inline(*scheduler);
        graphics::frame_token holder = std::move(moved_from);
        CHECK(holder.index() == 1);
        CHECK(moved_from.recycled_submissions().empty());

        holder = std::move(moved_from); // holder's frame 1 ends; it then holds nothing
        CHECK(count_lines(graphics_log, "frame 1 ended by its token") == 1);

        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
        scheduler->end_frame(std::move(next), {});
        // Destroying holder and moved_from (both empty) ends nothing more
    }

    SECTION("a when_all stopped by another branch drops the token it stored")
    {
        tests::recorded_assertions assertions;
        const auto outcome = stdexec::sync_wait(stdexec::when_all(scheduler->begin_frame(), stopping_sender{}));
        CHECK_FALSE(outcome.has_value());
        CHECK(count_lines(graphics_log, "frame 1 ended by its token") == 1);
        CHECK(assertions.count() == 0);

        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
        scheduler->end_frame(std::move(next), {});
    }
}

TEST_CASE("frame_scheduler: a stop request while waiting starts no frame", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    const rhi::synchronization_point held = gate.point(queue_type::graphics, 1);
    {
        graphics::frame_token first = tests::begin_frame_inline(*scheduler);
        const std::array points{ held };
        scheduler->end_frame(std::move(first), points);
    }
    scheduler->end_frame(tests::begin_frame_inline(*scheduler), {});

    {
        tests::started_frame third{ scheduler->begin_frame() };
        third.start();
        CHECK(third.outcome.total() == 0);
        third.source.request_stop();
        CHECK(third.outcome.stops == 1);
        CHECK(third.outcome.thread == std::this_thread::get_id());
    }

    // A later poll completes nothing more, and the next begin_frame is frame 3 again
    gate.open(1);
    hardware_interface.wait_for(held);
    graphics::frame_token third = tests::begin_frame_inline(*scheduler);
    CHECK(third.index() == 3);
    CHECK(points_of(third) == std::vector{ held });

    SECTION("a stop requested before start completes inline")
    {
        scheduler->end_frame(std::move(third), {});
        tests::started_frame stopped{ scheduler->begin_frame() };
        stopped.source.request_stop();
        stopped.start();
        CHECK(stopped.outcome.stops == 1);
        CHECK(tests::begin_frame_inline(*scheduler).index() == 4);
    }
}

TEST_CASE("frame_scheduler: device removal while a frame waits completes it with device_lost", "[graphics][removal]")
{
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors(); // the removal report and the debug layer's messages are errors
    tests::rhi_gate gate{ hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    {
        graphics::frame_token first = tests::begin_frame_inline(*scheduler);
        const std::array points{ gate.point(queue_type::graphics, 1) };
        scheduler->end_frame(std::move(first), points);
    }
    scheduler->end_frame(tests::begin_frame_inline(*scheduler), {});

    tests::started_frame third{ scheduler->begin_frame() };
    third.start();
    CHECK(third.outcome.total() == 0);

    hardware_interface->state().device().native().RemoveDevice();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
    while (third.outcome.total() == 0)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        driver.poll_completions();
        std::this_thread::yield();
    }
    REQUIRE(third.outcome.errors == 1);
    CHECK(rhi::is_error(*third.outcome.error, rhi::error_kind::device_lost));
    // No frame is outstanding: the scheduler's destructor doesn't assert
}

TEST_CASE("frame_scheduler: debug checks on pacing", "[graphics]")
{
    tests::recorded_assertions assertions;
    tests::log_capture graphics_log{ "graphics" };
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    SECTION("a second begin_frame while one is outstanding")
    {
        graphics::frame_token first = tests::begin_frame_inline(*scheduler);

        tests::started_frame second{ scheduler->begin_frame() };
        second.start();
        REQUIRE(second.outcome.errors == 1);
        CHECK(second.outcome.error->code == static_cast<std::int32_t>(generic_error_kind::logic_error));
        CHECK_FALSE(scheduler->try_begin_frame().has_value());
#ifndef NDEBUG
        CHECK(assertions.contains("begin_frame while a frame is outstanding"));
#endif

        // The outstanding frame is unaffected
        scheduler->end_frame(std::move(first), {});
        assertions.clear();
        CHECK(tests::begin_frame_inline(*scheduler).index() == 2);
        CHECK(assertions.count() == 0);
    }

    SECTION("end_frame with a token of another scheduler")
    {
        const auto other = tests::make_test_frame_scheduler(hardware_interface, 2);
        graphics::frame_token foreign = tests::begin_frame_inline(*other);

        other.get()->end_frame(std::move(foreign), {}); // fine
        graphics::frame_token mine = tests::begin_frame_inline(*scheduler);
        graphics::frame_token theirs = tests::begin_frame_inline(*other);
        scheduler->end_frame(std::move(theirs), {});
#ifndef NDEBUG
        CHECK(assertions.contains("isn't a frame of this scheduler"));
#endif
        // Refused: theirs is still outstanding on the other scheduler, and ends with its destructor
        CHECK(theirs.index() == 2);
        CHECK_FALSE(other->try_begin_frame().has_value());
        scheduler->end_frame(std::move(mine), {});
    }
}

// ---- per-frame services (7.3) ----

namespace
{
    template<std::size_t Size>
    std::span<const std::byte> bytes(const unsigned char (&code)[Size]) noexcept
    {
        return std::as_bytes(std::span{ code });
    }
} // namespace

TEST_CASE("frame_scheduler: write_frame_constants feeds a root CBV", "[graphics]")
{
    struct fill_constants
    {
        std::uint32_t buffer_index;
        std::uint32_t base;
        std::uint32_t count;
    };
    constexpr std::uint32_t count = 64;
    constexpr std::uint32_t base = 1000;

    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    rhi::resources resources{ *hardware_interface };
    rhi::pipelines pipelines{ *hardware_interface };

    // The fill shader reads its constants from b0; here b0 is a root CBV instead of root constants
    const std::array parameters{ rhi::root_parameter{ .kind = rhi::root_parameter_kind::constant_buffer,
                                                      .shader_register = 0 } };
    const auto signature = pipelines.create_root_signature({ .parameters = parameters, .debug_name = "fill cbv" });
    REQUIRE(signature.has_value());
    const auto pipeline = pipelines.create_pipeline(
        rhi::compute_pipeline_description{
            .root_signature = *signature, .compute_shader = bytes(rhi_test_compute_shader), .debug_name = "fill cbv" });
    REQUIRE(pipeline.has_value());

    const auto target = resources.create_buffer(
        { .size = count * 4u,
          .usage = buffer_usage::unordered_access | buffer_usage::copy_source,
          .debug_name = "target" });
    REQUIRE(target.has_value());
    const auto readback = resources.create_buffer(
        { .size = count * 4u, .usage = buffer_usage::copy_destination, .memory = rhi::memory_type::readback });
    REQUIRE(readback.has_value());

    graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
    REQUIRE(scheduler->allocate_frame_memory(frame, 4, 4).has_value()); // so the constants need aligning
    const auto address =
        scheduler->write_frame_constants(frame, fill_constants{ resources.uav(*target).value, base, count });
    REQUIRE(address.has_value());
    CHECK(address->value % 256 == 0);

    auto list = rhi::command_lists{ *hardware_interface }.acquire<queue_type::compute>("fill from frame constants");
    REQUIRE(list.has_value());
    list->set_pipeline(*pipeline);
    list->set_root_constant_buffer(0, *address);
    list->dispatch(1, 1, 1);
    const std::array barriers{ rhi::buffer_barrier{
        .buffer = *target,
        .before = { rhi::pipeline_synchronization::compute_shading, rhi::resource_access::unordered_access },
        .after = { rhi::pipeline_synchronization::copy, rhi::resource_access::copy_source } } };
    list->barrier({}, barriers, {});
    list->copy_buffer(*readback, 0, *target, 0, count * 4u);
    const auto point = rhi::queue<queue_type::compute>{ *hardware_interface }.submit(std::span{ &*list, 1 });
    REQUIRE(point.has_value());
    hardware_interface.wait_for(*point);

    const std::span<std::byte> values = resources.mapped_bytes(*readback);
    bool matches = true;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, values.data() + index * 4, 4);
        matches = matches && value == base + index;
    }
    CHECK(matches);

    const std::array points{ *point };
    scheduler->end_frame(std::move(frame), points);
    resources.destroy(*target);
    resources.destroy(*readback);
}

TEST_CASE("frame_scheduler: a recycle empties only its own context", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    constexpr std::size_t capacity = tests::test_frame_memory_size;

    const auto full_allocation = [&](const graphics::frame_token& frame)
    {
        const auto allocation = scheduler->allocate_frame_memory(frame, capacity);
        REQUIRE(allocation.has_value());
        return allocation->gpu;
    };

    graphics::frame_token first = tests::begin_frame_inline(*scheduler);
    const rhi::gpu_address first_base = full_allocation(first);
    CHECK_FALSE(scheduler->allocate_frame_memory(first, 1).has_value());
    scheduler->end_frame(std::move(first), {});

    graphics::frame_token second = tests::begin_frame_inline(*scheduler);
    const auto half = scheduler->allocate_frame_memory(second, capacity / 2);
    REQUIRE(half.has_value());
    const rhi::gpu_address second_base = half->gpu;
    CHECK(second_base != first_base); // the other context's allocator
    scheduler->end_frame(std::move(second), {});

    graphics::frame_token third = tests::begin_frame_inline(*scheduler); // recycles frame 1's context
    CHECK(full_allocation(third) == first_base);
    scheduler->end_frame(std::move(third), {});

    graphics::frame_token fourth = tests::begin_frame_inline(*scheduler); // recycles frame 2's context
    CHECK(full_allocation(fourth) == second_base);
    scheduler->end_frame(std::move(fourth), {});
}

TEST_CASE("frame_scheduler: frame views come from the context's range and are reused after the recycle", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    rhi::memory memory{ *hardware_interface };
    rhi::resources resources{ *hardware_interface };

    const auto heap = memory.create_heap({ .size = 1 << 20 });
    REQUIRE(heap.has_value());
    const auto texture = memory.create_placed_texture(
        *heap,
        0,
        { .width = 64,
          .height = 64,
          .format = rhi::format::rgba8_unorm,
          .usage = texture_usage::shader_resource | texture_usage::unordered_access });
    REQUIRE(texture.has_value());
    const auto buffer = resources.create_buffer(
        { .size = 4096, .usage = buffer_usage::shader_resource | buffer_usage::unordered_access, .debug_name = "raw" });
    REQUIRE(buffer.has_value());

    const std::uint32_t persistent =
        hardware_interface->state().descriptors().configuration().persistent_descriptor_count;
    constexpr std::uint32_t capacity = tests::test_frame_descriptor_count;

    // Every view of a frame, in creation order, until the range is full
    const auto fill_range = [&](const graphics::frame_token& frame)
    {
        std::vector<std::uint32_t> indices;
        const auto add = [&indices](const result<rhi::bindless_index>& view)
        {
            REQUIRE(view.has_value());
            indices.push_back(view->value);
        };
        add(scheduler->create_frame_srv(frame, *texture));
        add(scheduler->create_frame_uav(frame, *texture));
        add(scheduler->create_frame_srv(frame, *texture, { .first_mip = 0, .mip_count = 1 }));
        add(scheduler->create_frame_srv(frame, *buffer));
        add(scheduler->create_frame_uav(frame, *buffer));
        while (indices.size() < capacity)
        {
            add(scheduler->create_frame_srv(frame, *texture));
        }
        return indices;
    };

    graphics::frame_token first = tests::begin_frame_inline(*scheduler);
    const std::vector<std::uint32_t> first_views = fill_range(first);
    // One contiguous range in the transient region, after the persistent one
    CHECK(first_views.front() > persistent);
    bool contiguous = true;
    for (std::size_t index = 1; index < first_views.size(); ++index)
    {
        contiguous = contiguous && first_views[index] == first_views[0] + index;
    }
    CHECK(contiguous);
    scheduler->end_frame(std::move(first), {});

    graphics::frame_token second = tests::begin_frame_inline(*scheduler);
    const std::vector<std::uint32_t> second_views = fill_range(second);
    const bool separate = second_views.back() < first_views.front() || second_views.front() > first_views.back();
    CHECK(separate);
    scheduler->end_frame(std::move(second), {});

    graphics::frame_token third = tests::begin_frame_inline(*scheduler);
    CHECK(fill_range(third) == first_views);
    scheduler->end_frame(std::move(third), {});

    resources.destroy(*buffer);
    resources.destroy(*texture);
    memory.destroy(*heap);
}

TEST_CASE("frame_scheduler: concurrent frame allocations don't overlap", "[graphics]")
{
    constexpr int threads = 16;
    constexpr int per_thread = 60; // 16 × 60 × 64 bytes fit the 64 KiB of frame memory
    constexpr std::size_t size = 64;

    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    graphics::frame_token frame = tests::begin_frame_inline(*scheduler);

    std::vector<std::vector<std::uint64_t>> addresses(threads);
    std::vector<int> failures(threads, 0);
    {
        std::vector<std::jthread> workers;
        for (int thread = 0; thread < threads; ++thread)
        {
            workers.emplace_back(
                [&, thread]
                {
                    for (int index = 0; index < per_thread; ++index)
                    {
                        const auto allocation = scheduler->allocate_frame_memory(frame, size, size);
                        if (allocation)
                        {
                            addresses[thread].push_back(allocation->gpu.value);
                        }
                        else
                        {
                            ++failures[thread];
                        }
                    }
                });
        }
    }

    std::vector<std::uint64_t> all;
    for (const auto& list : addresses)
    {
        all.insert(all.end(), list.begin(), list.end());
    }
    CHECK(std::ranges::count(failures, 0) == threads);
    REQUIRE(all.size() == std::size_t{ threads } * per_thread);
    std::ranges::sort(all);
    bool overlapping = false;
    for (std::size_t index = 1; index < all.size(); ++index)
    {
        overlapping = overlapping || all[index] - all[index - 1] < size;
    }
    CHECK_FALSE(overlapping);
    scheduler->end_frame(std::move(frame), {});
}

TEST_CASE(
    "frame_scheduler: exhaustion fails, warns once per frame, and the next frame allocates normally", "[graphics]")
{
    tests::log_capture graphics_log{ "graphics" };
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 1);
    rhi::memory memory{ *hardware_interface };
    rhi::resources resources{ *hardware_interface };
    const auto heap = memory.create_heap({ .size = 1 << 20 });
    REQUIRE(heap.has_value());
    const auto texture = memory.create_placed_texture(
        *heap,
        0,
        { .width = 16, .height = 16, .format = rhi::format::rgba8_unorm, .usage = texture_usage::shader_resource });
    REQUIRE(texture.has_value());

    const auto exhaust = [&](const graphics::frame_token& frame)
    {
        REQUIRE(scheduler->allocate_frame_memory(frame, tests::test_frame_memory_size).has_value());
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto failed = scheduler->allocate_frame_memory(frame, 1);
            REQUIRE_FALSE(failed.has_value());
            CHECK(rhi::is_error(failed.error(), rhi::error_kind::transient_memory_exhausted));
            const auto constants = scheduler->write_frame_constants(frame, std::uint32_t{ 7 });
            REQUIRE_FALSE(constants.has_value());
            CHECK(rhi::is_error(constants.error(), rhi::error_kind::transient_memory_exhausted));
        }

        for (std::uint32_t index = 0; index < tests::test_frame_descriptor_count; ++index)
        {
            REQUIRE(scheduler->create_frame_srv(frame, *texture).has_value());
        }
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto failed = scheduler->create_frame_srv(frame, *texture);
            REQUIRE_FALSE(failed.has_value());
            CHECK(rhi::is_error(failed.error(), rhi::error_kind::transient_memory_exhausted));
        }
    };

    graphics::frame_token first = tests::begin_frame_inline(*scheduler);
    exhaust(first);
    CHECK(count_lines(graphics_log, "frame upload memory is exhausted") == 1);
    CHECK(
        count_lines(
            graphics_log,
            "[warning] frame 1: frame upload memory is exhausted (high-water mark 65536 of 65536 bytes)") == 1);
    CHECK(count_lines(graphics_log, "frame descriptors are exhausted") == 1);
    CHECK(count_lines(graphics_log, "frame 1: frame descriptors are exhausted (64 views)") == 1);
    scheduler->end_frame(std::move(first), {}); // the frame is skipped

    // The frame that reuses the context allocates normally, and warns again when it runs out
    graphics::frame_token second = tests::begin_frame_inline(*scheduler);
    exhaust(second);
    CHECK(count_lines(graphics_log, "frame 2: frame upload memory is exhausted") == 1);
    CHECK(count_lines(graphics_log, "frame 2: frame descriptors are exhausted") == 1);
    CHECK(count_lines(graphics_log, "are exhausted") + count_lines(graphics_log, "is exhausted") == 4);
    scheduler->end_frame(std::move(second), {});

    resources.destroy(*texture);
    memory.destroy(*heap);
}

TEST_CASE("frame_scheduler: per-frame services check the token", "[graphics]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
    graphics::frame_token holder = std::move(frame); // frame is moved from
    REQUIRE(scheduler->allocate_frame_memory(holder, 16).has_value());
    CHECK(assertions.count() == 0);

    const auto stale = scheduler->allocate_frame_memory(frame, 16);
    REQUIRE_FALSE(stale.has_value());
    CHECK(graphics::is_error(stale.error(), graphics::error_kind::invalid_argument));
    const auto stale_view = scheduler->create_frame_srv(frame, rhi::buffer_handle{});
    REQUIRE_FALSE(stale_view.has_value());
    CHECK(graphics::is_error(stale_view.error(), graphics::error_kind::invalid_argument));
#ifndef NDEBUG
    CHECK(assertions.contains("isn't the frame being recorded"));
#endif

    // A token of another scheduler
    assertions.clear();
    const auto other = tests::make_test_frame_scheduler(hardware_interface, 2);
    graphics::frame_token foreign = tests::begin_frame_inline(*other);
    CHECK_FALSE(scheduler->allocate_frame_memory(foreign, 16).has_value());
#ifndef NDEBUG
    CHECK(assertions.contains("isn't the frame being recorded"));
#endif
    other->end_frame(std::move(foreign), {});
    scheduler->end_frame(std::move(holder), {});
}
