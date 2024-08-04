#ifndef INPUT_INPUT_TYPE_ID_H
#define INPUT_INPUT_TYPE_ID_H

#include "avis/core/common.h"

namespace input::details
{
    template<auto id>
    struct counter
    {
        using tag = counter;

        struct generator
        {
            friend consteval auto is_defined(tag)
            {
                return true;
            }
        };
        friend consteval auto is_defined(tag);

        template<typename tag_t = tag, auto = is_defined(tag_t{})>
        static consteval auto exists(auto)
        {
            return true;
        }

        static consteval auto exists(...)
        {
            return generator(), false;
        }
    };

    template<auto id = int{}, typename = decltype([] {})>
    consteval auto unique_id()
    {
        if constexpr (not counter<id>::exists(id))
        {
            return id;
        }
        else
        {
            return unique_id<id + 1>();
        }
    }

    template<typename type>
    class type_index
    {
    public:
        static constexpr int id();

    private:
        static constexpr int type_id =  unique_id();
    };

    template<typename type>
    constexpr int type_index<type>::id()
    {
        return type_id;
    }
} // namespace input

#endif
