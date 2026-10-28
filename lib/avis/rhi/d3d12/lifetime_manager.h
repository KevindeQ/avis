#ifndef AVIS_RHI_D3D12_LIFETIME_MANAGER_H
#define AVIS_RHI_D3D12_LIFETIME_MANAGER_H

// Open-list epochs and deferred release behind one interface (docs/design/09-rhi.md §23.8.1): "this list opened or
// closed", "this object is destroyed", "do your housekeeping". Depends on the device queues only.

#include "avis/error.h"
#include "avis/rhi/d3d12/deferred_release.h"
#include "avis/rhi/d3d12/open_list_epochs.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace avis::rhi::d3d12
{
    class device_queues;

    class lifetime_manager
    {
    public:
        static constexpr std::size_t release_limit_per_call = 256;

        static result<std::unique_ptr<lifetime_manager>> create(device_queues& queues);

        lifetime_manager(const lifetime_manager&) = delete;
        lifetime_manager& operator=(const lifetime_manager&) = delete;

        // Command list pool (§24.2): open_list_epochs::enter and leave. The epoch is kept by the list.
        std::uint64_t list_opened() noexcept;
        void list_closed(std::uint64_t epoch) noexcept;

        // Destroyed objects: parked until every list open now has left, then tagged (§23.7, §23.8). Tagged at once
        // when no list is open. Terminates on out of memory (§23.8).
        void destroy(release_entry entry);

        // From device_state::poll_completions: advance the epoch, tag what's clean, free at most `limit` completed
        // entries (D-77). Returns true when ready entries remain, so the caller re-signals the completion event.
        bool housekeeping(std::size_t limit = release_limit_per_call) noexcept;

        // Frees everything; after wait_idle at shutdown
        void drain() noexcept;

        // Destroyed entries not freed yet (completion_driver::has_pending_work)
        std::size_t pending() const noexcept;

        // White-box checks (§19.5)
        const deferred_release& releases() const noexcept;
        const open_list_epochs& epochs() const noexcept;

    private:
        explicit lifetime_manager(device_queues& queues) noexcept;

        // try_advance, then tag_clean with each queue's last submitted value
        void advance_and_tag();

        // Debug builds: warns, at most once a second, while parked releases have been held back by an open list for
        // over a second. The list's debug name and thread arrive with the command list pool (6.10).
        void check_open_lists(std::uint64_t epoch_before) noexcept;

        device_queues& queues_;
        open_list_epochs epochs_;
        deferred_release releases_;
        std::atomic<std::chrono::steady_clock::rep> blocked_since_{ 0 }; // 0 = not blocked
        std::atomic<std::chrono::steady_clock::rep> last_warning_{ 0 };
    };
} // namespace avis::rhi::d3d12

#endif
