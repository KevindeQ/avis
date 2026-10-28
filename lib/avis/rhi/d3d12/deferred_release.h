#ifndef AVIS_RHI_D3D12_DEFERRED_RELEASE_H
#define AVIS_RHI_D3D12_DEFERRED_RELEASE_H

// Destroyed objects kept alive until the GPU is done with them (docs/design/09-rhi.md §23.8, D-91). Knows no payload
// type and no epoch policy: the lifetime manager decides when entries are parked, tagged and collected.

#include "avis/rhi/d3d12/completion_source.h"
#include "avis/rhi/d3d12/leaf_mutex.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace avis::rhi::d3d12
{
    // A destroyed object. Freeing it is destroying it: every payload releases what it owns in its destructor (COM
    // objects through com_ptr, descriptor indices through leases, handles through unique_handle), so the release
    // queue knows no payload type (D-91). One allocation per entry; destroy is rare next to recording.
    class release_entry
    {
    public:
        template<class Payload>
            requires(
                !std::is_same_v<std::remove_cvref_t<Payload>, release_entry> &&
                std::is_nothrow_move_constructible_v<std::remove_cvref_t<Payload>> &&
                std::is_nothrow_destructible_v<std::remove_cvref_t<Payload>>)
        explicit release_entry(Payload&& payload) :
            holder_{ std::make_unique<typed_holder<std::remove_cvref_t<Payload>>>(std::forward<Payload>(payload)) }
        {}

        release_entry(release_entry&&) noexcept = default;
        release_entry& operator=(release_entry&&) noexcept = default;
        release_entry(const release_entry&) = delete;
        release_entry& operator=(const release_entry&) = delete;
        ~release_entry() = default;

    private:
        struct holder
        {
            virtual ~holder() = default;
        };

        template<class Payload>
        struct typed_holder final : holder
        {
            explicit typed_holder(Payload&& value) noexcept : payload{ std::move(value) } {}
            explicit typed_holder(const Payload& value) : payload{ value } {}

            Payload payload;
        };

        std::unique_ptr<holder> holder_;
    };

    class deferred_release
    {
    public:
        struct collect_result
        {
            std::size_t freed = 0;
            bool more_ready = false; // ready entries remain after the limit
        };

        // Parks an entry under the open-list epoch of its destroy (§23.7)
        void park(std::uint64_t epoch, release_entry entry);

        // Tags every entry parked before `clean_before` with each queue's last submitted value
        void tag_clean(std::uint64_t clean_before, const release_tag& last_submitted);

        // Frees at most `limit` entries whose tag every queue has completed (D-77), destroying them outside the mutex
        collect_result collect(const release_tag& completed, std::size_t limit);

        // Frees everything, tagged or not; after wait_idle at shutdown
        void drain() noexcept;

        // Entries not freed yet
        std::size_t pending() const noexcept;
        std::size_t parked() const noexcept;
        std::size_t tagged() const noexcept;

    private:
        static bool reached(const release_tag& tag, const release_tag& completed) noexcept;

        mutable leaf_mutex mutex_{ "deferred_release::mutex_" };
        std::deque<std::pair<std::uint64_t, std::vector<release_entry>>> parked_; // by epoch, ascending
        std::deque<std::pair<release_tag, std::vector<release_entry>>> tagged_; // in tag order
        std::atomic<std::size_t> parked_count_{ 0 };
        std::atomic<std::size_t> tagged_count_{ 0 };
    };
} // namespace avis::rhi::d3d12

#endif
