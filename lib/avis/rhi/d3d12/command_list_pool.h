#ifndef AVIS_RHI_D3D12_COMMAND_LIST_POOL_H
#define AVIS_RHI_D3D12_COMMAND_LIST_POOL_H

// Per-thread command allocators and list records behind command_list<Queue> (docs/design/09-rhi.md §7.1, §24.2).
// Allocators are reused by synchronization point, not by frame.

#include "avis/error.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/leaf_mutex.h"
#include "avis/rhi/d3d12/timestamp_query_set_state.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace avis::rhi::d3d12
{
    class command_list_pool;
    class completion_source;
    class descriptor_heaps;
    class device;
    class lifetime_manager;
    class pipeline_factory;
    class resource_manager;
    struct root_signature_record;

    // Process-wide per-thread slot number (D-64), shared by every RHI in the process. Assigned on a thread's first
    // acquire: the lowest free number, else the next new one. Given back when the thread exits, by the destructor of a
    // thread_local, so the next new thread reuses it.
    std::uint32_t current_thread_slot() noexcept;

    // Behind current_thread_slot(); one instance per process
    class thread_slot_numbers
    {
    public:
        static thread_slot_numbers& instance() noexcept;

        std::uint32_t take() noexcept; // lowest free number, else next_++
        void give_back(std::uint32_t slot) noexcept; // from the thread_local's destructor
        std::uint32_t live_count() const noexcept;

    private:
        mutable leaf_mutex mutex_{ "thread_slot_numbers::mutex_" }; // only at thread start and exit
        std::vector<std::uint32_t> free_; // sorted, so numbers stay low
        std::uint32_t next_ = 0;
        std::atomic<std::uint32_t> live_{ 0 };
    };

    enum class list_state : std::uint8_t
    {
        free,
        recording
    };

    enum class bound_pipeline : std::uint8_t
    {
        none,
        graphics,
        compute
    };

    // What recording calls look up (§24.3). Built once by the composition root and shared by every record; read-only
    // after construction.
    struct recording_context
    {
        const resource_manager& resources;
        pipeline_factory& pipelines;
        descriptor_heaps& descriptors;
        device& device; // GetCopyableFootprints1 for copies; debug names
    };

    struct command_allocator_pool;

    // A command allocator and the point of the last submission that used it
    struct allocator_entry
    {
        com_ptr<ID3D12CommandAllocator> allocator;
        command_allocator_pool* home = nullptr; // the pool it returns to
        std::optional<synchronization_point> last_use; // set by submitted(); empty after discard()
        allocator_entry* next_returned = nullptr; // link in home->returned
    };

    // The owning record behind a command_list<Queue> and the recorders it hands out. Owned by its thread slot; lent to
    // one command_list at a time. Not movable (atomics).
    struct command_list_record
    {
        const recording_context* context = nullptr;
        command_list_pool* pool = nullptr; // submitted() and discard() from the public list
        com_ptr<graphics_list_interface> list;
        allocator_entry* allocator = nullptr; // lent while recording
        queue_type queue = queue_type::graphics; // the submission queue
        std::atomic<std::uint32_t> generation{ 0 }; // bumped on reuse; old lists and recorders become stale
        // free → recording by the owning thread (acquire); recording → free by whichever thread submits or discards,
        // as the last write to the record (release)
        std::atomic<list_state> state{ list_state::free };
        bound_pipeline pipeline = bound_pipeline::none;
        const root_signature_record* graphics_root = nullptr; // bound root signatures; none after acquire
        const root_signature_record* compute_root = nullptr;
        std::uint64_t epoch = 0; // open-list epoch (§23.7)
        std::vector<timestamp_range> timestamps; // runs of query indices written, resolved at submit (§24.4)
        // Debug: the thread inside a recording call, empty between calls
        std::atomic<std::thread::id> recording_thread{};
    };

    // The allocators of one queue type in one thread slot. Only the owning thread touches entries, in_flight and free;
    // any thread may push onto returned.
    struct command_allocator_pool
    {
        std::vector<std::unique_ptr<allocator_entry>> entries; // owns every allocator of this pool
        std::deque<allocator_entry*> in_flight; // submitted, by point value (oldest first)
        std::vector<allocator_entry*> free; // ready to Reset
        // Hand-back from submitted() and discard(): an intrusive lock-free stack. Producers push with a CAS; the owner
        // takes the whole stack with exchange, so there's no ABA problem.
        std::atomic<allocator_entry*> returned{ nullptr };
    };

    // Everything one thread needs to record: an allocator pool and the list records per queue type
    struct command_list_thread_slot
    {
        std::array<command_allocator_pool, 3> allocators; // by queue_type
        std::array<std::vector<std::unique_ptr<command_list_record>>, 3> records; // by queue_type
    };

    class command_list_pool
    {
    public:
        static constexpr std::size_t maximum_free_allocators = 8; // per queue type and slot

        static result<std::unique_ptr<command_list_pool>> create(
            device& device,
            const completion_source& completions,
            descriptor_heaps& descriptors,
            lifetime_manager& lifetime,
            const recording_context& recording);

        ~command_list_pool();

        command_list_pool(const command_list_pool&) = delete;
        command_list_pool& operator=(const command_list_pool&) = delete;

        // On the calling thread's slot, lock-free after its first call. Drains the hand-back stack, then takes a free
        // allocator, else the oldest in-flight one if its point is complete, else creates one; trims surplus free
        // allocators. Takes a free record, else creates one. device_lost after removal.
        result<command_list_record*> acquire(queue_type queue, std::string_view debug_name);

        // queue::submit, after execute, on any thread: the allocator's last_use, the hand-back, list_closed, then the
        // record is free (last write)
        void submitted(command_list_record& record, synchronization_point point) noexcept;

        // A command_list destroyed without submit, on any thread: Close, hand the allocator back without a point
        // (reusable at once), list_closed, free the record
        void discard(command_list_record& record) noexcept;

        // White-box counters (§19.5): allocators created and released over the pool's lifetime
        std::size_t allocators_created() const noexcept;
        std::size_t allocators_released() const noexcept;

    private:
        command_list_pool(
            device& device,
            const completion_source& completions,
            descriptor_heaps& descriptors,
            lifetime_manager& lifetime,
            const recording_context& recording) noexcept;

        // Grows the slot table under mutex_ on a thread's first call
        result<command_list_thread_slot*> current_slot();
        result<allocator_entry*> take_allocator(command_allocator_pool& pool, queue_type queue);
        result<command_list_record*> take_record(command_list_thread_slot& slot, queue_type queue, allocator_entry& entry);
        static void hand_back(allocator_entry& entry) noexcept;

        device& device_;
        const completion_source& completions_;
        descriptor_heaps& descriptors_;
        lifetime_manager& lifetime_;
        const recording_context& recording_;

        leaf_mutex mutex_{ "command_list_pool::mutex_" };
        using slot_table = std::vector<command_list_thread_slot*>;
        std::atomic<slot_table*> slot_table_{ nullptr }; // replaced, never mutated, on growth
        std::vector<std::unique_ptr<slot_table>> tables_; // the current one and the retired ones; freed at shutdown
        std::vector<std::unique_ptr<command_list_thread_slot>> slots_; // guarded by mutex_
        std::atomic<std::size_t> allocators_created_{ 0 };
        std::atomic<std::size_t> allocators_released_{ 0 };
        std::atomic<bool> slot_warning_logged_{ false };
    };
} // namespace avis::rhi::d3d12

#endif
