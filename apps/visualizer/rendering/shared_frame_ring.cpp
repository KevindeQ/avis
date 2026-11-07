#include "rendering/shared_frame_ring.h"

#include "avis/rhi/formats.h"

#include <format>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::uint64_t pack_size(const graphics::texture_extent size) noexcept
        {
            return (std::uint64_t{ size.width } << 32) | size.height;
        }

        constexpr graphics::texture_extent unpack_size(const std::uint64_t packed) noexcept
        {
            return { .width = static_cast<std::uint32_t>(packed >> 32), .height = static_cast<std::uint32_t>(packed) };
        }
    } // namespace

    namespace detail
    {
        std::variant<ring_slot, rhi::gpu_wait_sender> slot_policy::attempt() noexcept
        {
            ring_protocol& protocol = ring->protocol();
            const slot_acquisition acquired = protocol.try_acquire_slot(ring->fence_values());
            if (acquired.slot)
            {
                return ring_slot{ protocol, *acquired.slot };
            }
            return ring->wait_for(acquired.wait);
        }

        std::variant<std::monostate, rhi::gpu_wait_sender> release_policy::attempt() noexcept
        {
            const release_check check = ring->protocol().check_released(ring->fence_values());
            if (check.released)
            {
                return std::monostate{};
            }
            return ring->wait_for(check.wait);
        }
    } // namespace detail

    shared_frame_ring::shared_frame_ring(rhi::sharing sharing, rhi::resources resources) noexcept :
        sharing_{ sharing },
        resources_{ resources },
        state_{ std::make_shared<shared_ring_state>() }
    {}

    result<std::unique_ptr<shared_frame_ring>> shared_frame_ring::create(
        rhi::sharing sharing,
        rhi::resources resources,
        const graphics::texture_extent size,
        const std::uint64_t generation)
    {
        if (size.width == 0 || size.height == 0)
        {
            return std::unexpected{ avis::error{
                .message = std::format("a shared frame ring needs a size, got {}x{}", size.width, size.height) } };
        }

        std::unique_ptr<shared_frame_ring> ring{ new shared_frame_ring{ sharing, resources } };
        shared_ring_state& state = *ring->state_;
        state.size = size;
        state.generation = generation;

        for (std::uint32_t slot = 0; slot < ring_slot_count; ++slot)
        {
            const std::string name = std::format("shared frame ring {} slot {}", generation, slot);
            auto texture = sharing.create_shared_texture(
                { .width = size.width,
                  .height = size.height,
                  .format = rhi::format::rgba8_unorm,
                  .usage = rhi::texture_usage::shader_resource | rhi::texture_usage::copy_destination |
                           rhi::texture_usage::shared,
                  .initial_layout = rhi::texture_layout::common,
                  .debug_name = name });
            if (!texture)
            {
                return std::unexpected{ std::move(texture.error()) };
            }
            ring->textures_[slot] = *texture;
            auto handle = sharing.export_shared_handle(*texture);
            if (!handle)
            {
                return std::unexpected{ std::move(handle.error()) };
            }
            state.textures[slot] = *handle;
        }

        for (auto [fence, handle] :
             { std::pair{ &ring->produced_, &state.produced }, std::pair{ &ring->consumed_, &state.consumed } })
        {
            auto created = sharing.create_shared_fence();
            if (!created)
            {
                return std::unexpected{ std::move(created.error()) };
            }
            fence->emplace(std::move(*created));
            auto exported = sharing.export_shared_handle(**fence);
            if (!exported)
            {
                return std::unexpected{ std::move(exported.error()) };
            }
            *handle = *exported;
        }
        return ring;
    }

    shared_frame_ring::~shared_frame_ring()
    {
        for (const rhi::texture_handle texture : textures_)
        {
            if (texture.is_valid())
            {
                resources_.destroy(texture);
            }
        }
    }

    ring_fence_values shared_frame_ring::fence_values() const noexcept
    {
        return { .produced = produced_->completed_value(), .consumed = consumed_->completed_value() };
    }

    void shared_frame_ring::signal_produced(
        rhi::queue<rhi::queue_type::graphics>& queue, ring_slot slot, const std::uint64_t frame)
    {
        protocol().bind(slot.index(), frame);
        slot.protocol_ = nullptr; // bound: no longer a reservation to release
        queue.signal(*produced_, frame);
    }

    rhi::gpu_wait_sender shared_frame_ring::when_produced(const std::uint64_t frame)
    {
        return sharing_.when_reached(*produced_, frame);
    }

    bool shared_frame_ring::publish(const std::uint64_t frame, const std::uint32_t slot)
    {
        return protocol().publish(frame, slot);
    }

    void shared_frame_ring::retire()
    {
        protocol().retire();
    }

    bool shared_frame_ring::released() const
    {
        return state_->protocol.check_released(fence_values()).released;
    }

    rhi::gpu_wait_sender shared_frame_ring::wait_for(const ring_wait wait)
    {
        return sharing_.when_reached(wait.fence == ring_fence::produced ? *produced_ : *consumed_, wait.value);
    }

    shared_frame_rings::shared_frame_rings(rhi::sharing sharing, rhi::resources resources) noexcept :
        sharing_{ sharing },
        resources_{ resources }
    {}

    shared_frame_rings::~shared_frame_rings()
    {
        qt_current_.store(nullptr);
    }

    void shared_frame_rings::request_resize(const graphics::texture_extent size) noexcept
    {
        if (size.width == 0 || size.height == 0)
        {
            return;
        }
        pending_size_.store(pack_size(size));
    }

    result<shared_frame_ring*> shared_frame_rings::update()
    {
        const std::uint64_t pending = pending_size_.exchange(0);
        if (pending == 0 || (current_ && pack_size(current_->size()) == pending))
        {
            return current_.get();
        }

        auto created = shared_frame_ring::create(sharing_, resources_, unpack_size(pending), generation_ + 1);
        if (!created)
        {
            return std::unexpected{ std::move(created.error()) };
        }
        ++generation_;

        // Published to Qt first, then retired: Qt either attached to the old ring before (and detaches when it sees the
        // new one) or can't attach to it any more
        qt_current_.store((*created)->state());
        if (current_)
        {
            current_->retire();
            retired_.push_back(std::move(current_));
        }
        current_ = std::move(*created);
        return current_.get();
    }

    std::size_t shared_frame_rings::destroy_released()
    {
        std::erase_if(retired_, [](const std::unique_ptr<shared_frame_ring>& ring) { return ring->released(); });
        return retired_.size();
    }
} // namespace avis::visualizer::rendering
