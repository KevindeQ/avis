// The sort key's debug check (rendering/sort_key.h)

#include "rendering/sort_key.h"

#include "avis/detail/check.h"

#include <format>
#include <string>

namespace avis::visualizer::rendering::sort_key_detail
{
    void
        report_out_of_range(const std::string_view field, const std::uint64_t value, const std::uint64_t limit) noexcept
    {
        try
        {
            const std::string message = std::format("sort key: {} {} is out of range (limit {})", field, value, limit);
            AVIS_CHECK("renderer", false, message);
        }
        catch (...)
        {
            AVIS_CHECK("renderer", false, "sort key: a field is out of range");
        }
    }
} // namespace avis::visualizer::rendering::sort_key_detail
