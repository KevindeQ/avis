#include "avis/scene/snapshot.h"

#include "avis/detail/check.h"
#include "avis/log.h"

#include <chrono>
#include <immintrin.h>
#include <string_view>
#include <thread>
#include <utility>

namespace avis::scene
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";

        // begin_write's wait past which debug builds warn (08 §9.8)
        [[maybe_unused]] constexpr auto slow_wait = std::chrono::milliseconds{ 2 };

        // Pauses before begin_write's wait starts yielding the thread
        constexpr int spins_before_yield = 64;
    } // namespace

    // ---- snapshot ----

    void snapshot::clear() noexcept
    {
        instances.clear();
        lights.clear();
        meshes.clear();
        materials.clear();
        textures.clear();
    }

    // ---- snapshot_reference ----

    snapshot_reference::snapshot_reference(snapshot_buffer& buffer, const std::uint32_t slot) noexcept :
        buffer_{ &buffer },
        slot_{ slot }
    {}

    snapshot_reference::snapshot_reference(snapshot_reference&& other) noexcept :
        buffer_{ std::exchange(other.buffer_, nullptr) },
        slot_{ other.slot_ }
    {}

    snapshot_reference& snapshot_reference::operator=(snapshot_reference&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            buffer_ = std::exchange(other.buffer_, nullptr);
            slot_ = other.slot_;
        }
        return *this;
    }

    snapshot_reference::~snapshot_reference()
    {
        reset();
    }

    const snapshot& snapshot_reference::operator*() const noexcept
    {
        return buffer_->slots_[slot_];
    }

    const snapshot* snapshot_reference::operator->() const noexcept
    {
        return &buffer_->slots_[slot_];
    }

    snapshot_reference::operator bool() const noexcept
    {
        return buffer_ != nullptr;
    }

    void snapshot_reference::reset() noexcept
    {
        if (buffer_ != nullptr)
        {
            std::exchange(buffer_, nullptr)->release(slot_);
        }
    }

    // ---- snapshot_buffer ----

    snapshot_buffer::~snapshot_buffer()
    {
        lock();
        const std::uint32_t held = held_;
        unlock();
        AVIS_CHECK(logger_name, held == 0, "snapshot_buffer: destroyed while a snapshot_reference is alive");
    }

    snapshot& snapshot_buffer::begin_write()
    {
        AVIS_CHECK(logger_name, !writing_, "snapshot_buffer::begin_write: called again before publish");
        writing_ = true;

        // Before the first publish the front index points at a slot nobody reads, so write the other one
        const std::uint32_t back = published_ ? 1 - front_ : 1;

        // The reader can hold the back slot only when it acquired it before the last publish (08 §6.2)
#ifndef NDEBUG
        const auto start = std::chrono::steady_clock::now();
        bool warned = false;
#endif
        for (int spins = 0;; ++spins)
        {
            lock();
            const bool held = (held_ & (1u << back)) != 0;
            unlock();
            if (!held)
            {
                break;
            }

            if (spins < spins_before_yield)
            {
                _mm_pause();
            }
            else
            {
                std::this_thread::yield();
            }

#ifndef NDEBUG
            if (!warned && std::chrono::steady_clock::now() - start > slow_wait)
            {
                warned = true;
                log::get(logger_name)
                    ->warn(
                        "snapshot_buffer::begin_write: waited more than 2 ms for the reader to release its snapshot; "
                        "hold a snapshot_reference only while extracting (08 §6.2)");
            }
#endif
        }

        snapshot& target = slots_[back];
        target.clear();
        return target;
    }

    void snapshot_buffer::publish()
    {
        if (!AVIS_CHECK(logger_name, writing_, "snapshot_buffer::publish: no begin_write before it"))
        {
            return;
        }
        writing_ = false;

        const std::uint32_t back = published_ ? 1 - front_ : 1;
        const std::uint64_t sequence = publish_sequence_.load(std::memory_order_relaxed) + 1;
        slots_[back].publish_sequence = sequence;

        lock();
        front_ = back;
        published_ = true;
        unlock();
        publish_sequence_.store(sequence, std::memory_order_relaxed);
    }

    snapshot_reference snapshot_buffer::acquire()
    {
        lock();
        if (!published_)
        {
            unlock();
            return {};
        }
        const bool held = held_ != 0;
        const std::uint32_t front = front_;
        if (!held)
        {
            held_ |= 1u << front;
        }
        unlock();

        if (!AVIS_CHECK(
                logger_name,
                !held,
                "snapshot_buffer::acquire: a snapshot_reference is still alive; there is one reader (08 §9.8)"))
        {
            return {};
        }
        return snapshot_reference{ *this, front };
    }

    std::uint64_t snapshot_buffer::publish_sequence() const noexcept
    {
        return publish_sequence_.load(std::memory_order_relaxed);
    }

    void snapshot_buffer::lock() noexcept
    {
        while (lock_.test_and_set(std::memory_order_acquire))
        {
            while (lock_.test(std::memory_order_relaxed))
            {
                _mm_pause();
            }
        }
    }

    void snapshot_buffer::unlock() noexcept
    {
        lock_.clear(std::memory_order_release);
    }

    void snapshot_buffer::release(const std::uint32_t slot) noexcept
    {
        lock();
        held_ &= ~(1u << slot);
        unlock();
    }
} // namespace avis::scene
