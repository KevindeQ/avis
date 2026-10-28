#ifndef AVIS_RHI_D3D12_LEAF_MUTEX_H
#define AVIS_RHI_D3D12_LEAF_MUTEX_H

// The mutex of every RHI lock (docs/design/09-rhi.md §19.4). Every RHI lock is a leaf: no code takes another RHI lock
// while holding one. Debug builds check that rule with one thread_local flag shared by all instances; release builds
// use a plain std::mutex.

#include <mutex>

#ifndef NDEBUG
#    include "avis/rhi/d3d12/check.h"

#    include <format>
#endif

namespace avis::rhi::d3d12
{
#ifdef NDEBUG
    // A std::mutex that accepts the debug build's name. Not an alias: std::mutex can't be constructed from a name.
    class leaf_mutex : public std::mutex
    {
    public:
        explicit leaf_mutex(const char*) noexcept {}
    };
#else
    class leaf_mutex
    {
    public:
        // name: shown when the leaf rule is broken, for example "completion_tracker"
        explicit leaf_mutex(const char* const name) noexcept : name_{ name } {}

        leaf_mutex(const leaf_mutex&) = delete;
        leaf_mutex& operator=(const leaf_mutex&) = delete;

        void lock()
        {
            check_not_holding();
            mutex_.lock();
            holding() = true;
        }

        bool try_lock()
        {
            check_not_holding();
            if (!mutex_.try_lock())
            {
                return false;
            }
            holding() = true;
            return true;
        }

        void unlock()
        {
            holding() = false;
            mutex_.unlock();
        }

        // True while the calling thread holds any RHI lock
        static bool held_by_this_thread() noexcept
        {
            return holding();
        }

    private:
        static bool& holding() noexcept
        {
            thread_local bool held = false;
            return held;
        }

        void check_not_holding() const
        {
            AVIS_RHI_CHECK(
                !holding(), std::format("leaf_mutex: '{}' taken while this thread holds another RHI lock", name_));
        }

        std::mutex mutex_;
        const char* name_;
    };
#endif
} // namespace avis::rhi::d3d12

#endif
