#ifndef RUNTIME_DATA_SPATIAL_INDEX  
#    define RUNTIME_DATA_SPATIAL_INDEX

#include "avis/core/common.h"

namespace data
{
    template<typename value_t>
    class r_tree
    {
    public:
        using value_type = value_t;

    private:
        template<std::size_t child_count>
        struct node
        {
            using node_ptr = std::unique_ptr<node>;
            std::array<node_ptr, child_count> children;
        };

    public:
    private:
        // TODO: Determine best child count for max. performance
        static constexpr std::size_t max_child_count = 8;

        using node_t = node<max_child_count>;
        std::unique_ptr<node<max_child_count>> root;
    };

    class spatial_index
    {
    public:
    private:
        r_tree<int> data;
    };
}

#endif