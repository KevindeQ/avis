#include "avis/rhi/transient.h"

#include "avis/detail/safe_error.h"
#include "avis/log.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/deferred_release.h"
#include "avis/rhi/d3d12/descriptor_heaps.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/d3d12/timestamp_query_set_state.h"
#include "avis/rhi/d3d12/transient_upload.h"
#include "avis/rhi/d3d12/view_descriptions.h"
#include "avis/rhi/error.h"
#include "avis/rhi/render_hardware_interface.h"

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace avis::rhi
{
    namespace
    {
        std::string label(const std::string_view kind, const std::string_view name)
        {
            return name.empty() ? std::string{ kind } : std::format("{} '{}'", kind, name);
        }

        // Hands a state to the deferred release (§23.8): the GPU may still read from it
        template<class State>
        void defer(State* const state) noexcept
        {
            if (state == nullptr)
            {
                return;
            }
            d3d12::lifetime_manager* const lifetime = state->links.lifetime;
            lifetime->destroy(d3d12::release_entry{ std::unique_ptr<State>{ state } }); // out of memory terminates
        }

        avis::error stale(const char* const what)
        {
            return make_error(error_kind::invalid_handle, std::format("descriptor range: stale or invalid {} handle", what));
        }
    } // namespace

    // ---- linear_upload_allocator ----

    linear_upload_allocator::linear_upload_allocator(d3d12::linear_upload_allocator_state* const state) noexcept :
        state_{ state }
    {}

    linear_upload_allocator::linear_upload_allocator(linear_upload_allocator&& other) noexcept :
        state_{ std::exchange(other.state_, nullptr) }
    {}

    linear_upload_allocator& linear_upload_allocator::operator=(linear_upload_allocator&& other) noexcept
    {
        if (this != &other)
        {
            release();
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    linear_upload_allocator::~linear_upload_allocator()
    {
        release();
    }

    void linear_upload_allocator::release() noexcept
    {
        defer(std::exchange(state_, nullptr));
    }

    result<upload_allocation> linear_upload_allocator::allocate(const std::size_t size, const std::size_t alignment) noexcept
    {
        d3d12::linear_upload_allocator_state& state = *state_;
        const std::size_t capacity = state.buffer.mapped.size();
        std::size_t offset = state.offset.load(std::memory_order_relaxed);
        std::size_t aligned = 0;
        for (;;)
        {
            aligned = (offset + alignment - 1) & ~(alignment - 1);
            if (aligned > capacity || size > capacity - aligned)
            {
                if (!state.exhaustion_logged.exchange(true, std::memory_order_relaxed))
                {
                    try
                    {
                        log::get("rhi")->warn(
                            "{} is exhausted ({} bytes); allocations fail until reset",
                            label("linear upload allocator", state.buffer.name),
                            capacity);
                    }
                    catch (...)
                    {
                    }
                }
                return std::unexpected(avis::detail::safe_error(
                    error_domain::rhi,
                    static_cast<std::int32_t>(error_kind::transient_memory_exhausted),
                    "linear upload allocator is exhausted"));
            }
            if (state.offset.compare_exchange_weak(offset, aligned + size, std::memory_order_relaxed))
            {
                break;
            }
        }

        const std::size_t end = aligned + size;
        std::size_t mark = state.high_water_mark.load(std::memory_order_relaxed);
        while (mark < end && !state.high_water_mark.compare_exchange_weak(mark, end, std::memory_order_relaxed))
        {
        }
        return upload_allocation{ .cpu = state.buffer.mapped.subspan(aligned, size),
                                  .gpu = state.buffer.address.offset_by(aligned) };
    }

    void linear_upload_allocator::reset(const std::span<const synchronization_point> uses)
    {
        d3d12::check_reset_allowed(*state_->links.completions, uses, label("linear upload allocator", state_->buffer.name));
        state_->offset.store(0, std::memory_order_relaxed);
        state_->exhaustion_logged.store(false, std::memory_order_relaxed);
    }

    std::size_t linear_upload_allocator::capacity() const noexcept
    {
        return state_->buffer.mapped.size();
    }

    std::size_t linear_upload_allocator::high_water_mark() const noexcept
    {
        return state_->high_water_mark.load(std::memory_order_relaxed);
    }

    // ---- descriptor_range ----

    descriptor_range::descriptor_range(d3d12::descriptor_range_state* const state) noexcept : state_{ state } {}

    descriptor_range::descriptor_range(descriptor_range&& other) noexcept : state_{ std::exchange(other.state_, nullptr) }
    {}

    descriptor_range& descriptor_range::operator=(descriptor_range&& other) noexcept
    {
        if (this != &other)
        {
            release();
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    descriptor_range::~descriptor_range()
    {
        release();
    }

    void descriptor_range::release() noexcept
    {
        defer(std::exchange(state_, nullptr));
    }

    result<bindless_index> descriptor_range::create_srv(const texture_handle texture, const subresource_range range)
    {
        const d3d12::texture_record* const record = state_->resources->find(texture);
        if (record == nullptr)
        {
            return std::unexpected(stale("texture"));
        }
        return state_->heaps->create_transient_srv(
            *state_, *record->resource.get(), d3d12::texture_srv(record->description, range));
    }

    result<bindless_index> descriptor_range::create_uav(const texture_handle texture, const std::uint32_t mip)
    {
        const d3d12::texture_record* const record = state_->resources->find(texture);
        if (record == nullptr)
        {
            return std::unexpected(stale("texture"));
        }
        return state_->heaps->create_transient_uav(
            *state_, *record->resource.get(), d3d12::texture_uav(record->description, mip));
    }

    result<bindless_index> descriptor_range::create_srv(const buffer_handle buffer)
    {
        const d3d12::buffer_record* const record = state_->resources->find(buffer);
        if (record == nullptr)
        {
            return std::unexpected(stale("buffer"));
        }
        return state_->heaps->create_transient_srv(*state_, *record->resource.get(), d3d12::buffer_srv(record->description));
    }

    result<bindless_index> descriptor_range::create_uav(const buffer_handle buffer)
    {
        const d3d12::buffer_record* const record = state_->resources->find(buffer);
        if (record == nullptr)
        {
            return std::unexpected(stale("buffer"));
        }
        return state_->heaps->create_transient_uav(*state_, *record->resource.get(), d3d12::buffer_uav(record->description));
    }

    void descriptor_range::reset(const std::span<const synchronization_point> uses)
    {
        d3d12::check_reset_allowed(*state_->links.completions, uses, "descriptor range");
        state_->used.store(1, std::memory_order_relaxed); // past the null descriptor
    }

    std::uint32_t descriptor_range::capacity() const noexcept
    {
        return state_->count - 1;
    }

    // ---- timestamp_query_set ----

    timestamp_query_set::timestamp_query_set(d3d12::timestamp_query_set_state* const state) noexcept : state_{ state } {}

    timestamp_query_set::timestamp_query_set(timestamp_query_set&& other) noexcept :
        state_{ std::exchange(other.state_, nullptr) }
    {}

    timestamp_query_set& timestamp_query_set::operator=(timestamp_query_set&& other) noexcept
    {
        if (this != &other)
        {
            release();
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    timestamp_query_set::~timestamp_query_set()
    {
        release();
    }

    void timestamp_query_set::release() noexcept
    {
        defer(std::exchange(state_, nullptr));
    }

    queue_type timestamp_query_set::queue() const noexcept
    {
        return state_->queue;
    }

    std::uint32_t timestamp_query_set::capacity() const noexcept
    {
        return state_->count;
    }

    std::span<const std::uint64_t> timestamp_query_set::ticks(const std::uint32_t first, const std::uint32_t count) const
    {
        if (!AVIS_RHI_CHECK(
                first <= state_->count && count <= state_->count - first, "timestamp_query_set::ticks: out of range"))
        {
            return {};
        }
        const auto* const values = reinterpret_cast<const std::uint64_t*>(state_->resolved.mapped.data());
        return { values + first, count };
    }

    void timestamp_query_set::reset(const std::span<const synchronization_point> uses)
    {
        // Query heaps need no clearing: reset is a correctness check (§24.4)
        d3d12::check_reset_allowed(*state_->links.completions, uses, label("timestamp query set", state_->name));
    }

    // ---- transient_objects ----

    transient_objects::transient_objects(render_hardware_interface& hardware_interface) noexcept :
        state_{ &hardware_interface.state() }
    {}

    result<linear_upload_allocator>
        transient_objects::create_linear_upload_allocator(const std::size_t size, const std::string_view debug_name)
    {
        auto buffer = state_->resources().create_internal_buffer(
            { .size = size, .usage = buffer_usage::copy_source, .memory = memory_type::upload, .debug_name = debug_name });
        if (!buffer)
        {
            return std::unexpected(std::move(buffer.error()));
        }
        auto state = std::make_unique<d3d12::linear_upload_allocator_state>();
        state->links = { .completions = &state_->queues(), .lifetime = &state_->lifetime() };
        state->buffer = std::move(*buffer);
        return linear_upload_allocator{ state.release() };
    }

    result<descriptor_range>
        transient_objects::create_descriptor_range(const std::uint32_t count, [[maybe_unused]] const std::string_view debug_name)
    {
        if (count == 0)
        {
            return std::unexpected(make_error(error_kind::invalid_argument, "create_descriptor_range: count is 0"));
        }
        auto range = state_->descriptors().create_range(count + 1); // + the range's null descriptor
        if (!range)
        {
            return std::unexpected(std::move(range.error()));
        }
        (*range)->links = { .completions = &state_->queues(), .lifetime = &state_->lifetime() };
        (*range)->resources = &state_->resources();
        return descriptor_range{ range->release() };
    }

    result<timestamp_query_set> transient_objects::create_timestamp_query_set(
        const queue_type queue, const std::uint32_t count, const std::string_view debug_name)
    {
        const std::string name = label("timestamp query set", debug_name);
        if (count == 0)
        {
            return std::unexpected(make_error(error_kind::invalid_argument, std::format("{}: count is 0", name)));
        }
        d3d12::device& device = state_->device();
        if (queue == queue_type::copy && !device.capabilities().copy_queue_timestamps)
        {
            return std::unexpected(make_error(
                error_kind::unsupported_hardware,
                std::format("{}: the adapter has no copy-queue timestamps (D3D12_OPTIONS3)", name)));
        }

        auto state = std::make_unique<d3d12::timestamp_query_set_state>();
        state->links = { .completions = &state_->queues(), .lifetime = &state_->lifetime() };
        state->queue = queue;
        state->count = count;
        state->name = std::string{ debug_name };

        const D3D12_QUERY_HEAP_DESC heap{ .Type = queue == queue_type::copy ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP :
                                                                              D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                          .Count = count,
                                          .NodeMask = 0 };
        if (auto created = d3d12::check(
                device.native().CreateQueryHeap(&heap, IID_PPV_ARGS(&state->heap)), std::format("CreateQueryHeap ({})", name));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        d3d12::set_debug_name(device.debug(), *state->heap.get(), debug_name);

        auto resolved = state_->resources().create_internal_buffer({ .size = std::uint64_t{ count } * sizeof(std::uint64_t),
                                                                      .usage = buffer_usage::copy_destination,
                                                                      .memory = memory_type::readback,
                                                                      .debug_name = debug_name });
        if (!resolved)
        {
            return std::unexpected(std::move(resolved.error()));
        }
        state->resolved = std::move(*resolved);
        return timestamp_query_set{ state.release() };
    }
} // namespace avis::rhi
