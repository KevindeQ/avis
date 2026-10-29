#include "avis/rhi/d3d12/transient_upload.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/completion_source.h"

#include <format>

namespace avis::rhi::d3d12
{
    void check_reset_allowed(
        [[maybe_unused]] const completion_source& completions,
        [[maybe_unused]] const std::span<const synchronization_point> uses,
        [[maybe_unused]] const std::string_view object_name) noexcept
    {
#ifndef NDEBUG
        for (const synchronization_point point : uses)
        {
            AVIS_RHI_CHECK(
                completions.is_complete(point),
                std::format(
                    "{}: reset while a use isn't complete (queue {}, value {})",
                    object_name,
                    static_cast<int>(point.queue),
                    point.value));
        }
#endif
    }
} // namespace avis::rhi::d3d12
