#include "avis/graphics/pass_context.h"

#include "avis/detail/check.h"
#include "avis/error.h"
#include "avis/graphics/error.h"
#include "avis/graphics/frame_scheduler.h"

#include <cstring>
#include <string_view>
#include <utility>

namespace avis::graphics::detail
{
    rhi::gpu_address pass_context_base::allocate_constant_bytes(const std::span<const std::byte> data) const
    {
        constexpr std::string_view logger_name = "graphics";
        if (!AVIS_CHECK(
                logger_name,
                frame_ != nullptr && frames_ != nullptr,
                "allocate_constants called outside a pass's execute callback"))
        {
            throw avis::runtime_error{ make_error(
                error_kind::invalid_argument, "allocate_constants called outside a pass's execute callback") };
        }

        auto allocation = frames_->allocate_frame_memory(*frame_, data.size(), 256);
        if (!allocation)
        {
            throw avis::runtime_error{ std::move(allocation.error()) };
        }
        if (!data.empty())
        {
            std::memcpy(allocation->cpu.data(), data.data(), data.size());
        }
        return allocation->gpu;
    }
} // namespace avis::graphics::detail
