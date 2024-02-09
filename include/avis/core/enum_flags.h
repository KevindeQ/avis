#ifndef CORE_ENUM_FLAGS_H
#define CORE_ENUM_FLAGS_H

#include "avis/core/common.h"

template<typename enum_t>
concept unsigned_enum = std::is_enum_v<enum_t> && std::is_unsigned_v<std::underlying_type_t<enum_t>>;

template<unsigned_enum enum_t>
struct enable_flag_support : public std::false_type
{};

#define DECLARE_ENUM_FLAGS(enum_name)                             \
    template<>                                                    \
    struct enable_flag_support<enum_name> : public std::true_type \
    {}

namespace details
{
    template<typename enum_t>
    constexpr bool enable_flags()
    {
        return enable_flag_support<enum_t>::value;
    }
} // namespace details

template<typename enum_t>
concept enum_flag = details::enable_flags<enum_t>();

template<enum_flag enum_t>
constexpr auto underlying_value(enum_t enum_value)
{
    return static_cast<std::underlying_type_t<enum_t>>(enum_value);
}

template<enum_flag enum_t>
constexpr auto operator~(enum_t value)
{
    return static_cast<std::underlying_type_t<enum_t>>(~underlying_value(value));
}

template<enum_flag enum_t>
constexpr auto operator|(enum_t lhs, enum_t rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(underlying_value(lhs) | underlying_value(rhs));
}

template<enum_flag enum_t>
constexpr auto operator|(std::underlying_type_t<enum_t> lhs, enum_t rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(lhs | underlying_value(rhs));
}

template<enum_flag enum_t>
constexpr auto operator|(enum_t lhs, std::underlying_type_t<enum_t> rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(underlying_value(lhs) | rhs);
}

template<enum_flag enum_t>
constexpr auto operator|=(std::underlying_type_t<enum_t>& value, enum_t const flag)
{
    return value = value | flag;
}

template<enum_flag enum_t>
constexpr auto operator&(enum_t lhs, enum_t rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(underlying_value(lhs) & underlying_value(rhs));
}

template<enum_flag enum_t>
constexpr auto operator&(std::underlying_type_t<enum_t> lhs, enum_t rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(lhs & underlying_value(rhs));
}

template<enum_flag enum_t>
constexpr auto operator&(enum_t lhs, std::underlying_type_t<enum_t> rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(underlying_value(lhs) & rhs);
}

template<enum_flag enum_t>
constexpr auto operator&=(std::underlying_type_t<enum_t>& value, enum_t const flag)
{
    return value = value & flag;
}

template<enum_flag enum_t>
constexpr auto operator^(enum_t lhs, enum_t rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(underlying_value(lhs) ^ underlying_value(rhs));
}

template<enum_flag enum_t>
constexpr auto operator^(std::underlying_type_t<enum_t> lhs, enum_t rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(lhs ^ underlying_value(rhs));
}

template<enum_flag enum_t>
constexpr auto operator^(enum_t lhs, std::underlying_type_t<enum_t> rhs)
{
    return static_cast<std::underlying_type_t<enum_t>>(underlying_value(lhs) ^ rhs);
}

template<enum_flag enum_t>
constexpr auto operator^=(std::underlying_type_t<enum_t>& value, enum_t const flag)
{
    return value = value ^ flag;
}

template<auto mask>
constexpr bool has(std::underlying_type_t<decltype(mask)> value)
{
    return (value & mask) == underlying_value(mask);
}

#endif
