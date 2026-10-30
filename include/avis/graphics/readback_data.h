#ifndef AVIS_GRAPHICS_READBACK_DATA_H
#define AVIS_GRAPHICS_READBACK_DATA_H

// The leases a readback_queue delivers (docs/design/17-graphics-frames-and-transfers.md §8, D-99). A separate header so
// detail/readback_sender.h can name them without the queue. Free of <windows.h> and D3D12.

#include "avis/rhi/formats.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace avis::graphics
{
    class readback_queue;

    namespace detail
    {
        class readback_operation;

        // A pooled readback buffer, handed back to its queue when destroyed. Move-only.
        class readback_lease
        {
        public:
            readback_lease() noexcept = default;
            readback_lease(readback_lease&& other) noexcept;
            readback_lease& operator=(readback_lease&& other) noexcept;
            ~readback_lease();

            std::span<const std::byte> bytes() const noexcept
            {
                return bytes_;
            }

        private:
            friend class readback_operation;

            readback_lease(
                readback_queue& queue,
                rhi::buffer_handle buffer,
                int size_class,
                std::span<const std::byte> bytes) noexcept;

            void release() noexcept;

            readback_queue* queue_ = nullptr;
            rhi::buffer_handle buffer_;
            int size_class_ = -1; // -1: an exact-size buffer that isn't pooled
            std::span<const std::byte> bytes_;
        };
    } // namespace detail

    // A lease on a pooled readback buffer: the bytes stay mapped until the lease is destroyed, then the buffer returns
    // to the pool (Q6). Move-only; must not outlive the readback queue. Any thread may destroy it.
    class readback_data
    {
    public:
        readback_data(readback_data&&) noexcept = default;
        readback_data& operator=(readback_data&&) noexcept = default;
        ~readback_data() = default;

        // The bytes read, exactly the requested size. Empty for a moved-from lease.
        std::span<const std::byte> bytes() const noexcept
        {
            return lease_.bytes();
        }

    private:
        friend class detail::readback_operation;

        explicit readback_data(detail::readback_lease lease) noexcept : lease_{ std::move(lease) } {}

        detail::readback_lease lease_;
    };

    // A lease holding one texture subresource in the copyable footprint's layout: rows of row_pitch() bytes
    class readback_image
    {
    public:
        readback_image(readback_image&&) noexcept = default;
        readback_image& operator=(readback_image&&) noexcept = default;
        ~readback_image() = default;

        // row_pitch × rows × depth bytes, where rows is height() (block rows for BC formats)
        std::span<const std::byte> bytes() const noexcept
        {
            return lease_.bytes();
        }

        std::uint32_t width() const noexcept
        {
            return width_;
        }

        std::uint32_t height() const noexcept
        {
            return height_;
        }

        std::uint32_t row_pitch() const noexcept // 256-byte aligned (D3D12 pitch rule)
        {
            return row_pitch_;
        }

        rhi::format format() const noexcept
        {
            return format_;
        }

    private:
        friend class detail::readback_operation;

        readback_image(
            detail::readback_lease lease,
            const std::uint32_t width,
            const std::uint32_t height,
            const std::uint32_t row_pitch,
            const rhi::format format) noexcept :
            lease_{ std::move(lease) },
            width_{ width },
            height_{ height },
            row_pitch_{ row_pitch },
            format_{ format }
        {}

        detail::readback_lease lease_;
        std::uint32_t width_ = 0;
        std::uint32_t height_ = 0;
        std::uint32_t row_pitch_ = 0;
        rhi::format format_ = rhi::format::unknown;
    };
} // namespace avis::graphics

#endif
