#include "avis/runtime/data/page_view.h"

namespace data
{
    page_view::page_view() : page_id{ 0 }, data_view{}, current_offset{ 0 } {}

    page_view::page_view(const std::uint64_t id, std::span<unsigned char> page_data_view) :
        page_id{ id },
        data_view{ page_data_view },
        current_offset{ 0 }
    {}

    void page_view::clear()
    {
        std::fill(data_view.begin(), data_view.end(), 0);
    }
} // namespace data
