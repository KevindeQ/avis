#include "avis/middleware/geometry/data_layout.h"

namespace geometry
{
    void data_layout::add_element(const data_element_descriptor& descriptor)
    {
        std::vector<data_element_descriptor>::iterator insert_point = std::upper_bound(
            descriptors.begin(),
            descriptors.end(),
            descriptor,
            [](const data_element_descriptor& a, const data_element_descriptor& b) { return a.slot < b.slot; });
        descriptors.insert(insert_point, descriptor);
    }

    void data_layout::add_element_back(
        const std::string_view name,
        const data_element_format format,
        const std::size_t byte_count,
        const std::size_t byte_stride)
    {
        std::size_t byte_offset = sum_element_stride();
        data_element_descriptor new_descriptor{ .name = std::string{ name.begin(), name.end() },
                                                .slot = 0,
                                                .format = format,
                                                .byte_offset = byte_offset,
                                                .byte_count = byte_count,
                                                .byte_stride = byte_stride };

        if (!descriptors.empty())
        {
            const data_element_descriptor& last = descriptors.back();
            new_descriptor.slot = last.slot + 1;
        }

        descriptors.push_back(std::move(new_descriptor));
    }

    void data_layout::reset()
    {
        descriptors.clear();
    }

    std::size_t data_layout::stride() const
    {
        return sum_element_stride();
    }

    std::size_t data_layout::offset_by_id(const std::size_t id) const
    {
        // TODO: Take into account slot id (instead of looking at the element index)
        return descriptors.at(id).byte_offset;
    }

    std::size_t data_layout::sum_element_stride() const
    {
        std::size_t total = 0;
        for (data_element_descriptor descriptor : descriptors)
        {
            total += descriptor.byte_stride;
        }

        return total;
    }
} // namespace geometry
