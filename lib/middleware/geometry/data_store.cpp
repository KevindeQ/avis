#include "avis/middleware/geometry/data_store.h"

namespace geometry
{
    std::size_t data_store::reserve_block(const data_layout& element_layout)
    {
        std::size_t new_index = data_blocks.size();
        data_blocks.push_back(data_block{ .element_layout = element_layout, .data = {} });

        return new_index;
    }

    void data_store::set_block_data(const std::size_t block_id, const std::vector<unsigned char>& data)
    {
        if (block_id >= data_blocks.size())
        {
            throw std::out_of_range{ "block index out-of-range" };
        }

        data_blocks[block_id].data = data;
    }

    const data_layout& data_store::block_layout(const std::size_t block_id) const
    {
        if (block_id >= data_blocks.size())
        {
            throw std::out_of_range{ "block index out-of-range" };
        }
        
        return data_blocks[block_id].element_layout;
    }

    const std::vector<unsigned char>& data_store::block_data(const std::size_t block_id) const
    {
        if (block_id >= data_blocks.size())
        {
            throw std::out_of_range{ "block index out-of-range" };
        }

        return data_blocks[block_id].data;
    }

    std::vector<unsigned char>& data_store::block_data(const std::size_t block_id)
    {
        if (block_id >= data_blocks.size())
        {
            throw std::out_of_range{ "block index out-of-range" };
        }

        return data_blocks[block_id].data;
    }
} // namespace geometry
