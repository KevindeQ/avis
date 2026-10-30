#include "avis/graphics/readback_queue.h"

#include "texture_subresources.h"

#include "avis/detail/check.h"
#include "avis/detail/safe_error.h"
#include "avis/graphics/error.h"
#include "avis/log.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";

        constexpr int class_count =
            std::bit_width(readback_queue::largest_pooled_size / readback_queue::smallest_pooled_size);
        static_assert(readback_queue::smallest_pooled_size << (class_count - 1) == readback_queue::largest_pooled_size);

        // The size class holding `size` bytes, or -1 for an exact-size buffer
        constexpr int size_class_of(const std::uint64_t size) noexcept
        {
            if (size > readback_queue::largest_pooled_size)
            {
                return -1;
            }
            if (size <= readback_queue::smallest_pooled_size)
            {
                return 0;
            }
            return static_cast<int>(std::bit_width((size - 1) / readback_queue::smallest_pooled_size));
        }

        static_assert(size_class_of(1) == 0);
        static_assert(size_class_of(readback_queue::smallest_pooled_size) == 0);
        static_assert(size_class_of(readback_queue::smallest_pooled_size + 1) == 1);
        static_assert(size_class_of(readback_queue::smallest_pooled_size * 2) == 1);
        static_assert(size_class_of(readback_queue::smallest_pooled_size * 2 + 1) == 2);
        static_assert(size_class_of(readback_queue::largest_pooled_size) == class_count - 1);
        static_assert(size_class_of(readback_queue::largest_pooled_size + 1) == -1);

        avis::error invalid(std::string message)
        {
            return make_error(error_kind::invalid_argument, std::move(message));
        }
    } // namespace

    namespace detail
    {
        struct pooled_buffer
        {
            rhi::buffer_handle buffer;
            std::span<const std::byte> bytes; // the whole mapped buffer
            int size_class = -1;
        };

        struct readback_queue_state
        {
            mutable std::mutex mutex;
            std::array<std::vector<pooled_buffer>, class_count> classes; // free buffers, guarded by mutex
            readback_pool_statistics statistics; // guarded by mutex; pooled_buffers is counted when read
        };
    } // namespace detail

    // ---- readback_queue ----

    result<std::unique_ptr<readback_queue>> readback_queue::create(
        const rhi::resources resources,
        const rhi::command_lists command_lists,
        const rhi::queue<rhi::queue_type::graphics> graphics_queue,
        const rhi::queue<rhi::queue_type::compute> compute_queue,
        const rhi::queue<rhi::queue_type::copy> copy_queue,
        const rhi::synchronization synchronization)
    {
        try
        {
            return std::unique_ptr<readback_queue>{ new readback_queue{
                resources,
                command_lists,
                graphics_queue,
                compute_queue,
                copy_queue,
                synchronization,
                std::make_unique<detail::readback_queue_state>() } };
        }
        catch (...)
        {
            return std::unexpected(avis::detail::current_exception_error());
        }
    }

    readback_queue::readback_queue(
        const rhi::resources resources,
        const rhi::command_lists command_lists,
        const rhi::queue<rhi::queue_type::graphics> graphics_queue,
        const rhi::queue<rhi::queue_type::compute> compute_queue,
        const rhi::queue<rhi::queue_type::copy> copy_queue,
        const rhi::synchronization synchronization,
        std::unique_ptr<detail::readback_queue_state> state) noexcept :
        resources_{ resources },
        command_lists_{ command_lists },
        graphics_queue_{ graphics_queue },
        compute_queue_{ compute_queue },
        copy_queue_{ copy_queue },
        synchronization_{ synchronization },
        state_{ std::move(state) }
    {}

    readback_queue::~readback_queue()
    {
        const std::scoped_lock lock{ state_->mutex };
        AVIS_CHECK(logger_name, state_->statistics.leases == 0, "readback_queue destroyed while leases are alive");
        AVIS_CHECK(
            logger_name, state_->statistics.readbacks == 0, "readback_queue destroyed while readbacks are outstanding");
        // A lease returns its buffer to the pool, so every buffer the GPU wrote is complete; destroy them now
        for (std::vector<detail::pooled_buffer>& size_class : state_->classes)
        {
            for (const detail::pooled_buffer& entry : size_class)
            {
                resources_.destroy(entry.buffer);
            }
            size_class.clear();
        }
    }

    readback_buffer_sender readback_queue::read_buffer(
        const rhi::buffer_handle source,
        const std::uint64_t offset,
        const std::uint64_t size,
        const rhi::queue_type queue) noexcept
    {
        return readback_buffer_sender{ *this, { .buffer = source, .offset = offset, .size = size, .queue = queue } };
    }

    readback_texture_sender readback_queue::read_texture(
        const rhi::texture_handle source,
        const rhi::subresource source_subresource,
        const rhi::queue_type queue) noexcept
    {
        return readback_texture_sender{ *this,
                                        { .texture = source, .subresource = source_subresource, .queue = queue } };
    }

    readback_pool_statistics readback_queue::statistics() const
    {
        const std::scoped_lock lock{ state_->mutex };
        readback_pool_statistics statistics = state_->statistics;
        statistics.pooled_buffers = 0;
        for (const std::vector<detail::pooled_buffer>& size_class : state_->classes)
        {
            statistics.pooled_buffers += size_class.size();
        }
        return statistics;
    }

    result<detail::pooled_buffer> readback_queue::take_buffer(const std::uint64_t size)
    {
        detail::readback_queue_state& state = *state_;
        const int size_class = size_class_of(size);
        if (size_class >= 0)
        {
            const std::scoped_lock lock{ state.mutex };
            std::vector<detail::pooled_buffer>& free = state.classes[static_cast<std::size_t>(size_class)];
            if (!free.empty())
            {
                const detail::pooled_buffer entry = free.back();
                free.pop_back();
                return entry;
            }
        }

        // Created without the lock; another thread may create one of the same class meanwhile, which is harmless
        const std::uint64_t buffer_size =
            size_class >= 0 ? smallest_pooled_size << static_cast<unsigned>(size_class) : size;
        auto created = resources_.create_buffer(
            { .size = buffer_size,
              .usage = rhi::buffer_usage::copy_destination,
              .memory = rhi::memory_type::readback,
              .debug_name = size_class >= 0 ? "readback queue pool" : "readback queue exact" });
        if (!created)
        {
            return std::unexpected(std::move(created.error()));
        }
        const detail::pooled_buffer entry{ .buffer = *created,
                                           .bytes = resources_.mapped_bytes(*created).first(buffer_size),
                                           .size_class = size_class };
        const std::scoped_lock lock{ state.mutex };
        ++state.statistics.buffers_created;
        return entry;
    }

    void readback_queue::return_buffer(
        const rhi::buffer_handle buffer, const int size_class, const bool ends_lease) noexcept
    {
        detail::readback_queue_state& state = *state_;
        {
            const std::scoped_lock lock{ state.mutex };
            if (ends_lease)
            {
                --state.statistics.leases;
            }
            if (size_class >= 0)
            {
                std::vector<detail::pooled_buffer>& free = state.classes[static_cast<std::size_t>(size_class)];
                if (free.size() < buffers_per_class)
                {
                    try
                    {
                        const std::uint64_t buffer_size = smallest_pooled_size << static_cast<unsigned>(size_class);
                        free.push_back(
                            { .buffer = buffer,
                              .bytes = resources_.mapped_bytes(buffer).first(buffer_size),
                              .size_class = size_class });
                        return;
                    }
                    catch (...)
                    {} // out of memory: destroy it instead
                }
            }
            ++state.statistics.buffers_destroyed;
        }
        // The copy into it completed before the lease was delivered, so the RHI frees it at its next housekeeping
        resources_.destroy(buffer);
    }

    void readback_queue::discard_buffer(const rhi::buffer_handle buffer) noexcept
    {
        {
            const std::scoped_lock lock{ state_->mutex };
            ++state_->statistics.buffers_destroyed;
        }
        // The copy may still be writing it: the RHI's deferred release frees it once the GPU is done (17 §8)
        resources_.destroy(buffer);
    }

    template<rhi::queue_type Queue>
    result<rhi::synchronization_point>
        readback_queue::record_and_submit(const detail::readback_request& request, const rhi::buffer_handle destination)
    {
        auto list = command_lists_.acquire<Queue>("readback queue copy");
        if (!list)
        {
            return std::unexpected(std::move(list.error()));
        }
        if (request.texture.is_valid())
        {
            list->copy_texture_to_buffer(destination, 0, request.texture, request.subresource);
        }
        else
        {
            list->copy_buffer(destination, 0, request.buffer, request.offset, request.size);
        }

        if constexpr (Queue == rhi::queue_type::graphics)
        {
            return graphics_queue_.submit(std::span{ &*list, 1 });
        }
        else if constexpr (Queue == rhi::queue_type::compute)
        {
            return compute_queue_.submit(std::span{ &*list, 1 });
        }
        else
        {
            return copy_queue_.submit(std::span{ &*list, 1 });
        }
    }

    // ---- detail::readback_operation ----

    namespace detail
    {
        result<rhi::gpu_wait_sender> readback_operation::submit() noexcept
        {
            readback_queue& queue = *queue_;
            readback_queue_state& state = *queue.state_;
            try
            {
                // Validate, and size the destination
                std::uint64_t size = 0;
                if (request_.texture.is_valid())
                {
                    const rhi::texture_description& description = queue.resources_.description(request_.texture);
                    const std::uint32_t mips = mip_count(description);
                    const std::uint32_t layers = layer_count(description);
                    if (request_.subresource.mip >= mips || request_.subresource.layer >= layers)
                    {
                        return std::unexpected(invalid(
                            std::format(
                                "read_texture: subresource (mip {}, layer {}) is outside a texture of {} mips and {} "
                                "layers",
                                request_.subresource.mip,
                                request_.subresource.layer,
                                mips,
                                layers)));
                    }
                    const rhi::copy_footprint footprint =
                        queue.resources_.copyable_footprint(request_.texture, request_.subresource);
                    // Whole rows, so bytes() is row_pitch × rows × depth even though the copy leaves the last row's
                    // padding unwritten
                    size = std::max<std::uint64_t>(
                        footprint.total_size,
                        std::uint64_t{ footprint.row_pitch } * footprint.row_count * footprint.depth);
                    width_ = footprint.width;
                    height_ = footprint.height;
                    row_pitch_ = footprint.row_pitch;
                    format_ = description.format;
                }
                else
                {
                    if (!request_.buffer.is_valid())
                    {
                        return std::unexpected(invalid("read_buffer: invalid source buffer"));
                    }
                    const rhi::buffer_description& description = queue.resources_.description(request_.buffer);
                    if (request_.size == 0 || request_.offset > description.size ||
                        request_.size > description.size - request_.offset)
                    {
                        return std::unexpected(invalid(
                            std::format(
                                "read_buffer: {} bytes at offset {} aren't inside a buffer of {} bytes",
                                request_.size,
                                request_.offset,
                                description.size)));
                    }
                    size = request_.size;
                }

                auto taken = queue.take_buffer(size);
                if (!taken)
                {
                    return std::unexpected(std::move(taken.error()));
                }
                buffer_ = taken->buffer;
                size_class_ = taken->size_class;
                bytes_ = taken->bytes.first(size);
                {
                    const std::scoped_lock lock{ state.mutex };
                    ++state.statistics.readbacks;
                }

                result<rhi::synchronization_point> point;
                try
                {
                    switch (request_.queue)
                    {
                    case rhi::queue_type::graphics:
                        point = queue.record_and_submit<rhi::queue_type::graphics>(request_, buffer_);
                        break;
                    case rhi::queue_type::compute:
                        point = queue.record_and_submit<rhi::queue_type::compute>(request_, buffer_);
                        break;
                    case rhi::queue_type::copy:
                        point = queue.record_and_submit<rhi::queue_type::copy>(request_, buffer_);
                        break;
                    }
                }
                catch (...)
                {
                    point = std::unexpected(avis::detail::current_exception_error());
                }
                if (!point)
                {
                    // Nothing was submitted (or the device is gone), so the buffer goes back to the pool
                    {
                        const std::scoped_lock lock{ state.mutex };
                        --state.statistics.readbacks;
                    }
                    queue.return_buffer(buffer_, size_class_, false);
                    buffer_ = {};
                    return std::unexpected(std::move(point.error()));
                }
                return queue.synchronization_.when_complete(*point);
            }
            catch (...)
            {
                return std::unexpected(avis::detail::current_exception_error());
            }
        }

        readback_lease readback_operation::complete() noexcept
        {
            readback_queue_state& state = *queue_->state_;
            {
                const std::scoped_lock lock{ state.mutex };
                --state.statistics.readbacks;
                ++state.statistics.leases;
            }
            return readback_lease{ *queue_, std::exchange(buffer_, {}), size_class_, bytes_ };
        }

        readback_data readback_operation::take_data() noexcept
        {
            return readback_data{ complete() };
        }

        readback_image readback_operation::take_image() noexcept
        {
            return readback_image{ complete(), width_, height_, row_pitch_, format_ };
        }

        void readback_operation::abandon() noexcept
        {
            readback_queue_state& state = *queue_->state_;
            {
                const std::scoped_lock lock{ state.mutex };
                --state.statistics.readbacks;
            }
            queue_->discard_buffer(std::exchange(buffer_, {}));
        }

        // ---- readback_lease ----

        readback_lease::readback_lease(
            readback_queue& queue,
            const rhi::buffer_handle buffer,
            const int size_class,
            const std::span<const std::byte> bytes) noexcept :
            queue_{ &queue },
            buffer_{ buffer },
            size_class_{ size_class },
            bytes_{ bytes }
        {}

        readback_lease::readback_lease(readback_lease&& other) noexcept :
            queue_{ std::exchange(other.queue_, nullptr) },
            buffer_{ std::exchange(other.buffer_, {}) },
            size_class_{ std::exchange(other.size_class_, -1) },
            bytes_{ std::exchange(other.bytes_, {}) }
        {}

        readback_lease& readback_lease::operator=(readback_lease&& other) noexcept
        {
            if (this != &other)
            {
                release();
                queue_ = std::exchange(other.queue_, nullptr);
                buffer_ = std::exchange(other.buffer_, {});
                size_class_ = std::exchange(other.size_class_, -1);
                bytes_ = std::exchange(other.bytes_, {});
            }
            return *this;
        }

        readback_lease::~readback_lease()
        {
            release();
        }

        void readback_lease::release() noexcept
        {
            if (queue_ != nullptr)
            {
                std::exchange(queue_, nullptr)->return_buffer(std::exchange(buffer_, {}), size_class_, true);
                bytes_ = {};
            }
        }
    } // namespace detail
} // namespace avis::graphics
