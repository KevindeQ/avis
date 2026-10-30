#ifndef AVIS_GRAPHICS_UPLOAD_TICKET_H
#define AVIS_GRAPHICS_UPLOAD_TICKET_H

// The value types of the upload queue (docs/design/17-graphics-frames-and-transfers.md §7). Their own header because
// detail/upload_sender.h completes with upload_ticket and upload_queue.h returns that sender; include upload_queue.h.

#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace avis::graphics
{
    // Names the flush that carries an upload's copy. Resolves to a copy-queue point once that flush happened
    // (upload_queue::point). Only valid with the queue that issued it.
    struct upload_ticket
    {
        std::uint64_t flush_index = 0; // 0 = nothing to wait for
        std::uint32_t queue_id = 0; // the issuing queue; point() asserts it (Q7)

        friend constexpr bool operator==(upload_ticket, upload_ticket) noexcept = default;
    };

    // One subresource of a texture upload, in the caller's layout
    struct subresource_data
    {
        rhi::subresource target;
        std::span<const std::byte> bytes;
        std::uint32_t row_pitch = 0; // bytes per row (per block row for BC formats); 0 = tightly packed
        std::uint32_t slice_pitch = 0; // bytes per depth slice (3D textures); 0 = row_pitch × rows
    };
} // namespace avis::graphics

#endif
