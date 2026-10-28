#ifndef AVIS_RHI_D3D12_OPEN_LIST_EPOCHS_H
#define AVIS_RHI_D3D12_OPEN_LIST_EPOCHS_H

// Grace periods over open command lists, in the style of RCU (docs/design/09-rhi.md §23.7). Lock-free: two atomic
// increments per list.
//
// Invariant: while the epoch is e, the counter of e's parity holds the open lists of epoch e only, and the other one
// those of epoch e - 1 only. Lists of e - 2 have all left, because the epoch can't advance to e until they have. So a
// release parked in epoch p can be tagged once the lists of p and p - 1 have left, which is the case for every epoch
// before current() - 1.

#include <array>
#include <atomic>
#include <cstdint>

namespace avis::rhi::d3d12
{
    class open_list_epochs
    {
    public:
        // acquire_command_list: counts the list in the current epoch and returns that epoch
        std::uint64_t enter() noexcept
        {
            for (;;)
            {
                const std::uint64_t epoch = epoch_.load();
                open_[epoch & 1].fetch_add(1);
                // The epoch may have advanced between the load and the increment; then this list is counted in the
                // wrong parity, so undo and retry
                if (epoch_.load() == epoch)
                {
                    return epoch;
                }
                open_[epoch & 1].fetch_sub(1);
            }
        }

        // Submit or discard of a list that entered in `epoch`
        void leave(const std::uint64_t epoch) noexcept
        {
            open_[epoch & 1].fetch_sub(1);
        }

        std::uint64_t current() const noexcept
        {
            return epoch_.load();
        }

        // Advances the epoch while every list of the previous epoch has left (at most twice: with no list open, a
        // release parked in the current epoch becomes clean in one call). Returns the first epoch that isn't clean:
        // releases parked in an earlier epoch can be tagged.
        std::uint64_t try_advance() noexcept
        {
            for (int step = 0; step < 2; ++step)
            {
                std::uint64_t epoch = epoch_.load();
                if (open_[(epoch - 1) & 1].load() != 0)
                {
                    break;
                }
                // A concurrent caller may advance first; either way the epoch moved on
                epoch_.compare_exchange_strong(epoch, epoch + 1);
            }
            return epoch_.load() - 1;
        }

        // Lists open in the current and the previous epoch (tests, the debug warning)
        std::uint32_t open_lists() const noexcept
        {
            return open_[0].load() + open_[1].load();
        }

    private:
        // Starts at 1 so that try_advance() never returns a negative epoch
        std::atomic<std::uint64_t> epoch_{ 1 };
        std::array<std::atomic<std::uint32_t>, 2> open_{}; // by epoch parity
    };
} // namespace avis::rhi::d3d12

#endif
