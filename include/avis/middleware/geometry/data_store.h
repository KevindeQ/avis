#ifndef MIDDLEWARE_GEOMETRY_DATA_STORE_H
#define MIDDLEWARE_GEOMETRY_DATA_STORE_H

#include "avis/core/common.h"
#include "avis/middleware/geometry/data_layout.h"

namespace geometry
{
    struct data_block
    {
    public:
        data_layout element_layout;
        std::vector<unsigned char> data;
    };

    class data_store
    {
    public:
        std::size_t reserve_block(const data_layout& element_layout);
        void set_block_data(const std::size_t block_id, const std::vector<unsigned char>& data);

        const data_layout& block_layout(const std::size_t block_id) const;
        const std::vector<unsigned char>& block_data(const std::size_t block_id) const;
        std::vector<unsigned char>& block_data(const std::size_t block_id);

    private:
        std::vector<data_block> data_blocks;
    };
} // namespace geometry

#endif
