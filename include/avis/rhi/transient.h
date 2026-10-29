#ifndef AVIS_RHI_TRANSIENT_H
#define AVIS_RHI_TRANSIENT_H

// Transient upload memory, descriptor ranges and timestamp query sets (docs/design/09-rhi.md §6). The RHI has no
// frames: each object's reset is its owner's decision, checked against the GPU timeline. Free of D3D12 and <windows.h>.

#include "avis/error.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace avis::rhi
{
    class render_hardware_interface;
    class transient_objects;

    namespace d3d12
    {
        class device_state;
        struct linear_upload_allocator_state;
        struct descriptor_range_state;
        struct timestamp_query_set_state;
    } // namespace d3d12

    // Memory from a linear allocator in an upload heap (write-combined): write only, never read back
    struct upload_allocation
    {
        std::span<std::byte> cpu;
        gpu_address gpu;
    };

    // Lock-free bump allocator over one persistently mapped upload buffer. Move-only; destroyed through the deferred
    // release, so it may be destroyed while the GPU still reads from it.
    class linear_upload_allocator
    {
    public:
        linear_upload_allocator(linear_upload_allocator&& other) noexcept;
        linear_upload_allocator& operator=(linear_upload_allocator&& other) noexcept;
        linear_upload_allocator(const linear_upload_allocator&) = delete;
        linear_upload_allocator& operator=(const linear_upload_allocator&) = delete;
        ~linear_upload_allocator();

        // Any thread, concurrently. `alignment` is a power of two. Fails with transient_memory_exhausted, logged once
        // per reset cycle with the debug name and capacity.
        result<upload_allocation> allocate(std::size_t size, std::size_t alignment = 256) noexcept;

        // Copies a trivially copyable T into a 256-byte-aligned allocation and returns its address
        template<class T>
            requires std::is_trivially_copyable_v<T>
        result<gpu_address> write(const T& value) noexcept
        {
            auto allocation = allocate(sizeof(T), 256);
            if (!allocation)
            {
                return std::unexpected(std::move(allocation.error()));
            }
            std::memcpy(allocation->cpu.data(), &value, sizeof(T));
            return allocation->gpu;
        }

        // Rewinds to empty. `uses` must cover every submission that read from the allocator since the last reset; debug
        // builds assert each is complete. Not concurrent with allocate.
        void reset(std::span<const synchronization_point> uses);

        std::size_t capacity() const noexcept;
        std::size_t high_water_mark() const noexcept; // since creation; for sizing

    private:
        friend class transient_objects;
        explicit linear_upload_allocator(d3d12::linear_upload_allocator_state* state) noexcept;
        void release() noexcept;

        d3d12::linear_upload_allocator_state* state_ = nullptr;
    };

    // A range of the shader-visible resource heap's transient region, for views of placed (transient) resources and
    // other short-lived views. Descriptors are bump-allocated and valid until reset.
    class descriptor_range
    {
    public:
        descriptor_range(descriptor_range&& other) noexcept;
        descriptor_range& operator=(descriptor_range&& other) noexcept;
        descriptor_range(const descriptor_range&) = delete;
        descriptor_range& operator=(const descriptor_range&) = delete;
        ~descriptor_range(); // returns the range to the transient region (deferred)

        // Any thread, concurrently. Fail with transient_memory_exhausted when the range is full, invalid_handle for a
        // stale handle.
        result<bindless_index> create_srv(texture_handle texture, subresource_range range = all_subresources);
        // Also written to the staging heap at the same index, for UAV clears (D-89)
        result<bindless_index> create_uav(texture_handle texture, std::uint32_t mip = 0);
        result<bindless_index> create_srv(buffer_handle buffer);
        result<bindless_index> create_uav(buffer_handle buffer);

        void reset(std::span<const synchronization_point> uses); // as linear_upload_allocator::reset

        // Views it holds between resets
        std::uint32_t capacity() const noexcept;

    private:
        friend class transient_objects;
        explicit descriptor_range(d3d12::descriptor_range_state* state) noexcept;
        void release() noexcept;

        d3d12::descriptor_range_state* state_ = nullptr;
    };

    // Timestamp queries of one queue type. Results are resolved automatically when a list that wrote into the set is
    // submitted, and readable once that submission is complete.
    class timestamp_query_set
    {
    public:
        timestamp_query_set(timestamp_query_set&& other) noexcept;
        timestamp_query_set& operator=(timestamp_query_set&& other) noexcept;
        timestamp_query_set(const timestamp_query_set&) = delete;
        timestamp_query_set& operator=(const timestamp_query_set&) = delete;
        ~timestamp_query_set();

        queue_type queue() const noexcept;
        std::uint32_t capacity() const noexcept;

        // Raw GPU ticks (see queue<Queue>::timestamp_frequency); the submissions that wrote them must be complete. An
        // out-of-range request asserts and returns an empty span.
        std::span<const std::uint64_t> ticks(std::uint32_t first, std::uint32_t count) const;

        void reset(std::span<const synchronization_point> uses); // as linear_upload_allocator::reset

    private:
        friend class transient_objects;
        template<queue_type>
        friend class command_recorder;
        explicit timestamp_query_set(d3d12::timestamp_query_set_state* state) noexcept;
        void release() noexcept;

        d3d12::timestamp_query_set_state* state_ = nullptr;
    };

    // A view (§4.3): non-owning, copyable, built from the RHI and never outliving it
    class transient_objects
    {
    public:
        explicit transient_objects(render_hardware_interface& hardware_interface) noexcept;

        result<linear_upload_allocator> create_linear_upload_allocator(std::size_t size, std::string_view debug_name = {});
        // `count` views, carved from the transient region (descriptor_heap_full when it has no room)
        result<descriptor_range> create_descriptor_range(std::uint32_t count, std::string_view debug_name = {});
        // unsupported_hardware for a copy-queue set without copy_queue_timestamps; invalid_argument for a count of 0
        result<timestamp_query_set>
            create_timestamp_query_set(queue_type queue, std::uint32_t count, std::string_view debug_name = {});

    private:
        d3d12::device_state* state_;
    };
} // namespace avis::rhi

#endif
