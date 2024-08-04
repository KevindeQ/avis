#ifndef MIDDLEWARE_INPUT_DETAILS_INPUT_WRAPPER_H
#define MIDDLEWARE_INPUT_DETAILS_INPUT_WRAPPER_H

#include "avis/core/common.h"
#include "avis/core/hash_combine.h"
#include "avis/middleware/input/details/input_type_id.h"

namespace input::details
{
    class input_wrapper
    {
    public:
        template<enumeration type_value_t>
        input_wrapper(type_value_t& type_value);
        template<enumeration type_value_t, typename value_t>
        input_wrapper(type_value_t& type_value, value_t& value);

        bool operator==(const input_wrapper& other) const;

    private:
        friend struct std::hash<input_wrapper>;

        static constexpr std::size_t max_size_object = 4;

        int input_type_id;
        std::array<std::byte, max_size_object> input_type_value;
        std::size_t input_type_size;
        std::array<std::byte, max_size_object> input_value;
        std::size_t input_value_size;
    };

    template<enumeration type_value_t>
    input_wrapper::input_wrapper(type_value_t& type_value) :
        input_type_id{ details::type_index<type_value_t>::id() },
        input_type_value{},
        input_type_size{ sizeof(type_value_t) },
        input_value{},
        input_value_size{ 0 }
    {
        static_assert(sizeof(type_value_t) <= max_size_object, "Given datatype doesn't fit in the storage");

        // Store a local copy of the given value for later use
        std::copy_n(
            reinterpret_cast<typename decltype(input_type_value)::pointer>(&type_value),
            input_type_size,
            input_type_value.begin());
    }

    template<enumeration type_value_t, typename value_t>
    input_wrapper::input_wrapper(type_value_t& type_value, value_t& value) :
        input_type_id{ details::type_index<type_value_t>::id() },
        input_type_value{},
        input_type_size{ sizeof(type_value_t) },
        input_value{},
        input_value_size{ sizeof(value_t) }
    {
        static_assert(sizeof(type_value_t) <= max_size_object, "Given datatype doesn't fit in the storage");

        // Store a local copy of the given type_value for later comparison
        std::copy_n(
            reinterpret_cast<typename decltype(input_type_value)::pointer>(&type_value),
            input_type_size,
            input_type_value.begin());

        // Store a local copy of the given value for later retrieval
        std::copy_n(
            reinterpret_cast<typename decltype(input_value)::pointer>(&value), input_value_size, input_value.begin());
    }
} // namespace input::details

namespace std
{
    template<>
    struct std::hash<typename input::details::input_wrapper>
    {
    public:
        std::size_t operator()(const typename input::details::input_wrapper& key) const
        {
            using std::hash;

            std::size_t seed = 0;
            hash_combine(seed, key.input_type_id);
            hash_combine(seed, key.input_type_size);
            for (const std::byte& b : key.input_type_value)
            {
                hash_combine(seed, b);
            }

            return seed;
        }
    };
} // namespace std

#endif
