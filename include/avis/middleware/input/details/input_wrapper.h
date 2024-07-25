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
        template<typename value_t, typename std::enable_if<std::is_enum_v<value_t>>::type* = nullptr>
        input_wrapper(value_t& value);

        bool operator==(const input_wrapper& other) const;

    private:
        friend struct std::hash<input_wrapper>;

        static constexpr std::size_t max_size_object = 8;

        int type_id;

        std::array<std::byte, max_size_object> input_value;
        std::size_t input_size;
    };

    template<typename value_t, typename std::enable_if<std::is_enum_v<value_t>>::type*>
    input_wrapper::input_wrapper(value_t& value) :
        type_id{ details::type_index<value_t>::id() },
        input_value{},
        input_size{ sizeof(value_t) }
    {
        static_assert(sizeof(value_t) <= max_size_object, "Given datatype doesn't fit in the storage");

        // Store a local copy of the given value for later use
        std::copy_n(
            reinterpret_cast<std::array<std::byte, max_size_object>::pointer>(&value), input_size, input_value.begin());
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
            hash_combine(seed, key.type_id);
            hash_combine(seed, key.input_size);
            for (const std::byte& b : key.input_value)
            {
                hash_combine(seed, b);
            }

            return seed;
        }
    };
} // namespace std

#endif
