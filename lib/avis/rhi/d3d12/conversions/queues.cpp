#include "avis/rhi/d3d12/conversions/queues.h"

#include "avis/rhi/d3d12/conversions/table.h"

#include <array>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::array<std::pair<queue_type, D3D12_COMMAND_LIST_TYPE>, 3> queues{ {
            { queue_type::graphics, D3D12_COMMAND_LIST_TYPE_DIRECT },
            { queue_type::compute, D3D12_COMMAND_LIST_TYPE_COMPUTE },
            { queue_type::copy, D3D12_COMMAND_LIST_TYPE_COPY },
        } };

        static_assert(detail::rows_in_enum_order(queues), "one row per queue type, in enum order");
        static_assert(queues.back().first == queue_type::copy, "the table covers the last queue type");
    } // namespace

    D3D12_COMMAND_LIST_TYPE to_native(const queue_type value) noexcept
    {
        return detail::look_up(queues, value, D3D12_COMMAND_LIST_TYPE_DIRECT);
    }
} // namespace avis::rhi::d3d12
