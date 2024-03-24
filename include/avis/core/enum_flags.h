#ifndef CORE_ENUM_FLAGS_H
#define CORE_ENUM_FLAGS_H

#include "avis/core/common.h"

template<typename enum_t, typename enabler = void>
struct is_flags : public std::false_type
{};

#define ALLOW_FLAGS_FOR_ENUM(name)         \
    template<>                             \
    struct is_flags<name> : std::true_type \
    {};

constexpr struct empty_t
{
    constexpr empty_t() noexcept = default;
} empty;

template<class enum_t>
class flags;

template<class enum_t>
class flags_iterator
{
public:
    using flags_type = flags<enum_t>;
    using difference_type = std::ptrdiff_t;
    using value_type = enum_t;
    using pointer = value_type*;
    using reference = const value_type;
    using iterator_category = std::forward_iterator_tag;

    constexpr flags_iterator() noexcept;
    constexpr flags_iterator(const flags_iterator& other) noexcept;

    flags_iterator& operator++() noexcept;
    flags_iterator operator++(int) noexcept;

    constexpr reference operator*() const noexcept;

    template<class other_enum_t>
    friend inline constexpr bool
        operator==(const flags_iterator<other_enum_t>& lhs, const flags_iterator<other_enum_t>& rhs) noexcept;
    template<class other_enum_t>
    friend inline constexpr bool
        operator!=(const flags_iterator<other_enum_t>& lhs, const flags_iterator<other_enum_t>& rhs) noexcept;

private:
    template<class other_enum_t>
    friend class flags;

    using implementation_type = typename flags_type::implementation_type;

    explicit flags_iterator(implementation_type unsigned_value) noexcept;
    constexpr flags_iterator(implementation_type unsigned_value, enum_t enum_value) noexcept;

    void next_mask() noexcept;

    implementation_type internal_unsigned_value;
    implementation_type internal_mask;
};

template<class enum_t>
class flags
{
public:
    static_assert(is_flags<enum_t>::value, "Enum flags is disallowed for this type; use ALLOW_FLAGS_FOR_ENUM macro.");

    using enum_type = typename std::decay<enum_t>::type;
    using underlying_type = typename std::underlying_type<enum_type>::type;
    using implementation_type = typename std::make_unsigned<underlying_type>::type;

    using iterator = flags_iterator<enum_type>;
    using const_iterator = iterator;
    using value_type = typename iterator::value_type;
    using reference = typename iterator::reference;
    using const_reference = typename iterator::reference;
    using pointer = enum_type*;
    using const_pointer = const enum_type*;
    using size_type = std::size_t;
    using difference_type = typename iterator::difference_type;

private:
    template<class convertable_t, class result_t = std::nullptr_t>
    using convertable = std::enable_if<std::is_convertible<convertable_t, enum_type>::value, result_t>;

public:
    flags() noexcept = default;
    constexpr flags(enum_type enum_value) noexcept;
    explicit constexpr flags(empty_t) noexcept;
    flags(std::initializer_list<enum_type> il) noexcept;

    template<class... args_t>
    flags(enum_type enum_value, args_t... args) noexcept;

    template<class forward_iter_t>
    flags(
        forward_iter_t begin,
        forward_iter_t enum_value,
        typename convertable<decltype(*std::declval<forward_iter_t>())>::type =
            nullptr) noexcept(noexcept(std::declval<flags>()
                                           .insert(std::declval<forward_iter_t>(), std::declval<forward_iter_t>())));

    flags(const flags& fl) noexcept = default;
    flags(flags&& fl) noexcept = default;

    flags& operator=(enum_type enum_value) noexcept;
    flags& operator=(std::initializer_list<enum_type> il) noexcept;
    flags& operator=(const flags& fl) noexcept = default;
    flags& operator=(flags&& fl) noexcept = default;

    constexpr explicit operator bool() const noexcept;

    constexpr bool operator!() const noexcept;

    constexpr flags operator~() const noexcept;

    flags& operator|=(const flags& fl) noexcept;
    flags& operator&=(const flags& fl) noexcept;
    flags& operator^=(const flags& fl) noexcept;
    flags& operator|=(enum_type enum_value) noexcept;
    flags& operator&=(enum_type enum_value) noexcept;
    flags& operator^=(enum_type enum_value) noexcept;

    template<class other_enum_t>
    friend constexpr flags<other_enum_t> operator|(flags<other_enum_t> lhs, other_enum_t rhs) noexcept;
    template<class other_enum_t>
    friend constexpr flags<other_enum_t> operator|(flags<other_enum_t> lhs, flags<other_enum_t> rhs) noexcept;
    template<class other_enum_t>
    friend constexpr flags<other_enum_t> operator&(flags<other_enum_t> lhs, other_enum_t rhs) noexcept;
    template<class other_enum_t>
    friend constexpr flags<other_enum_t> operator&(flags<other_enum_t> lhs, flags<other_enum_t> rhs) noexcept;
    template<class other_enum_t>
    friend constexpr flags<other_enum_t> operator^(flags<other_enum_t> lhs, other_enum_t rhs) noexcept;
    template<class other_enum_t>
    friend constexpr flags<other_enum_t> operator^(flags<other_enum_t> lhs, flags<other_enum_t> rhs) noexcept;

    template<class other_enum_t>
    friend constexpr bool operator==(flags<other_enum_t> fl1, flags<other_enum_t> fl2);
    template<class other_enum_t>
    friend constexpr bool operator!=(flags<other_enum_t> fl1, flags<other_enum_t> fl2);

    void swap(flags& fl) noexcept;

    constexpr underlying_type underlying_value() const noexcept;
    void set_underlying_value(underlying_type new_value) noexcept;

    constexpr static std::size_t bit_size()
    {
        return sizeof(implementation_type) * CHAR_BIT;
    }

    constexpr explicit operator std::bitset<flags<enum_t>::bit_size()>() const noexcept;

    constexpr std::bitset<flags<enum_t>::bit_size()> to_bitset() const noexcept;

    constexpr bool empty() const noexcept;

    size_type size() const noexcept;
    constexpr size_type max_size() const noexcept;

    iterator begin() const noexcept;
    constexpr iterator end() const noexcept;

    iterator cbegin() const noexcept;
    constexpr iterator cend() const noexcept;

    constexpr iterator find(enum_type enum_value) const noexcept;

    constexpr size_type count(enum_type enum_value) const noexcept;

    std::pair<iterator, iterator> equal_range(enum_type enum_value) const noexcept;

    template<class... args_t>
    std::pair<iterator, bool> emplace(args_t&&... args) noexcept;

    template<class... args_t>
    iterator emplace_hint(iterator, args_t&&... args) noexcept;

    std::pair<iterator, bool> insert(enum_type enum_value) noexcept;
    std::pair<iterator, bool> insert(iterator, enum_type enum_value) noexcept;

    template<class forward_iter_t>
    auto insert(forward_iter_t begin, forward_iter_t end) noexcept(noexcept(++begin) && noexcept(*begin) && noexcept(
        begin == end)) -> typename convertable<decltype(*begin), void>::type;

    template<class container_t>
    auto insert(const container_t& ctn) noexcept -> decltype(std::begin(ctn), std::end(ctn), void());

    iterator erase(iterator i) noexcept;
    size_type erase(enum_type enum_value) noexcept;
    iterator erase(iterator begin, iterator end) noexcept;

    void clear() noexcept;

private:
    constexpr explicit flags(implementation_type val) noexcept;

    void update_uvalue(iterator& it) const noexcept;

    implementation_type internal_value;
};

template<class enum_t>
constexpr flags_iterator<enum_t>::flags_iterator() noexcept : internal_unsigned_value(0), internal_mask(0)
{}

template<class enum_t>
constexpr flags_iterator<enum_t>::flags_iterator(const flags_iterator& other) noexcept :
    internal_unsigned_value(other.internal_unsigned_value),
    internal_mask(other.internal_mask)
{}

template<class enum_t>
flags_iterator<enum_t>& flags_iterator<enum_t>::operator++() noexcept
{
    next_mask();
    return *this;
}

template<class enum_t>
flags_iterator<enum_t> flags_iterator<enum_t>::operator++(int) noexcept
{
    auto copy = *this;
    ++(*this);
    return copy;
}

template<class enum_t>
constexpr typename flags_iterator<enum_t>::reference flags_iterator<enum_t>::operator*() const noexcept
{
    return static_cast<value_type>(internal_mask);
}

template<class enum_t>
inline constexpr bool operator==(const flags_iterator<enum_t>& lhs, const flags_iterator<enum_t>& rhs) noexcept
{
    return lhs.internal_mask == rhs.internal_mask;
}

template<class enum_t>
inline constexpr bool operator!=(const flags_iterator<enum_t>& lhs, const flags_iterator<enum_t>& rhs) noexcept
{
    return lhs.internal_mask != rhs.internal_mask;
}

template<class enum_t>
flags_iterator<enum_t>::flags_iterator(implementation_type unsigned_value) noexcept :
    internal_unsigned_value(unsigned_value),
    internal_mask(1)
{
    if (!(internal_mask & internal_unsigned_value))
    {
        next_mask();
    }
}

template<class enum_t>
constexpr flags_iterator<enum_t>::flags_iterator(implementation_type unsigned_value, enum_t enum_value) noexcept :
    internal_unsigned_value(unsigned_value),
    internal_mask(static_cast<implementation_type>(static_cast<implementation_type>(enum_value) & unsigned_value))
{}

template<class enum_t>
void flags_iterator<enum_t>::next_mask() noexcept
{
    do
    {
        internal_mask <<= 1;
    }
    while (internal_mask && !(internal_mask & internal_unsigned_value));
}

template<class enum_t>
constexpr flags<enum_t>::flags(empty_t) noexcept : internal_value(0)
{}

template<class enum_t>
constexpr flags<enum_t>::flags(enum_type enum_value) noexcept :
    internal_value(static_cast<implementation_type>(enum_value))
{}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator=(enum_type enum_value) noexcept
{
    internal_value = static_cast<implementation_type>(enum_value);
    return *this;
}

template<class enum_t>
flags<enum_t>::flags(std::initializer_list<enum_type> il) noexcept : internal_value(0)
{
    insert(il);
}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator=(std::initializer_list<enum_type> il) noexcept
{
    clear();
    insert(il);
    return *this;
}

template<class enum_t>
template<class... args_t>
flags<enum_t>::flags(enum_type enum_value, args_t... args) noexcept : flags{ enum_value, args... }
{}

template<class enum_t>
template<class forward_iter_t>
flags<enum_t>::flags(
    forward_iter_t begin,
    forward_iter_t enum_value,
    typename convertable<decltype(*std::declval<forward_iter_t>())>::
        type) noexcept(noexcept(std::declval<flags>()
                                    .insert(std::declval<forward_iter_t>(), std::declval<forward_iter_t>()))) :
    internal_value(0)
{
    insert(begin, enum_value);
}

template<class enum_t>
constexpr flags<enum_t>::operator bool() const noexcept
{
    return internal_value != 0;
}

template<class enum_t>
constexpr bool flags<enum_t>::operator!() const noexcept
{
    return !internal_value;
}

template<class enum_t>
constexpr bool operator==(flags<enum_t> fl1, flags<enum_t> fl2)
{
    return fl1.internal_value == fl2.internal_value;
}

template<class enum_t>
constexpr bool operator!=(flags<enum_t> fl1, flags<enum_t> fl2)
{
    return fl1.internal_value != fl2.internal_value;
}

template<class enum_t>
constexpr flags<enum_t> flags<enum_t>::operator~() const noexcept
{
    return flags(~internal_value);
}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator|=(const flags& fl) noexcept
{
    internal_value |= fl.internal_value;
    return *this;
}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator&=(const flags& fl) noexcept
{
    internal_value &= fl.internal_value;
    return *this;
}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator^=(const flags& fl) noexcept
{
    internal_value ^= fl.internal_value;
    return *this;
}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator|=(enum_type enum_value) noexcept
{
    internal_value |= static_cast<implementation_type>(enum_value);
    return *this;
}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator&=(enum_type enum_value) noexcept
{
    internal_value &= static_cast<implementation_type>(enum_value);
    return *this;
}

template<class enum_t>
flags<enum_t>& flags<enum_t>::operator^=(enum_type enum_value) noexcept
{
    internal_value ^= static_cast<implementation_type>(enum_value);
    return *this;
}

template<class enum_t>
constexpr flags<enum_t> operator|(flags<enum_t> lhs, enum_t rhs) noexcept
{
    return flags<enum_t>{ lhs.internal_value | static_cast<typename flags<enum_t>::implementation_type>(rhs) };
}

template<class enum_t>
constexpr flags<enum_t> operator|(flags<enum_t> lhs, flags<enum_t> rhs) noexcept
{
    return flags{ static_cast<typename flags<enum_t>::implementation_type>(lhs.internal_value | rhs.internal_value) };
}

template<class enum_t>
constexpr flags<enum_t> operator&(flags<enum_t> lhs, enum_t rhs) noexcept
{
    return flags<enum_t>{ lhs.internal_value & static_cast<typename flags<enum_t>::implementation_type>(rhs) };
}

template<class enum_t>
constexpr flags<enum_t> operator&(flags<enum_t> lhs, flags<enum_t> rhs) noexcept
{
    return flags{ static_cast<typename flags<enum_t>::implementation_type>(lhs.internal_value & rhs.internal_value) };
}

template<class enum_t>
constexpr flags<enum_t> operator^(flags<enum_t> lhs, enum_t rhs) noexcept
{
    return flags<enum_t>{ lhs.internal_value ^ static_cast<typename flags<enum_t>::implementation_type>(rhs) };
}

template<class enum_t>
constexpr flags<enum_t> operator^(flags<enum_t> lhs, flags<enum_t> rhs) noexcept
{
    return flags{ static_cast<typename flags<enum_t>::implementation_type>(lhs.internal_value ^ rhs.internal_value) };
}

template<class enum_t>
void flags<enum_t>::swap(flags<enum_t>& fl) noexcept
{
    std::swap(internal_value, fl.internal_value);
}

template<class enum_t>
constexpr typename flags<enum_t>::underlying_type flags<enum_t>::underlying_value() const noexcept
{
    return static_cast<underlying_type>(internal_value);
}

template<class enum_t>
void flags<enum_t>::set_underlying_value(underlying_type new_value) noexcept
{
    internal_value = static_cast<implementation_type>(new_value);
}

template<class enum_t>
constexpr flags<enum_t>::operator std::bitset<flags<enum_t>::bit_size()>() const noexcept
{
    return to_bitset();
}

template<class enum_t>
constexpr std::bitset<flags<enum_t>::bit_size()> flags<enum_t>::to_bitset() const noexcept
{
    return { internal_value };
}

template<class enum_t>
constexpr bool flags<enum_t>::empty() const noexcept
{
    return !internal_value;
}

template<class enum_t>
typename flags<enum_t>::size_type flags<enum_t>::size() const noexcept
{
    return std::distance(this->begin(), this->end());
}

template<class enum_t>
constexpr typename flags<enum_t>::size_type flags<enum_t>::max_size() const noexcept
{
    return bit_size();
}

template<class enum_t>
flags<enum_t>::iterator flags<enum_t>::begin() const noexcept
{
    return cbegin();
}

template<class enum_t>
flags<enum_t>::iterator flags<enum_t>::cbegin() const noexcept
{
    return iterator{ internal_value };
}

template<class enum_t>
constexpr flags<enum_t>::iterator flags<enum_t>::end() const noexcept
{
    return cend();
}

template<class enum_t>
constexpr flags<enum_t>::iterator flags<enum_t>::cend() const noexcept
{
    return {};
}

template<class enum_t>
constexpr flags<enum_t>::iterator flags<enum_t>::find(enum_type enum_value) const noexcept
{
    return { internal_value, enum_value };
}

template<class enum_t>
constexpr flags<enum_t>::size_type flags<enum_t>::count(enum_type enum_value) const noexcept
{
    return find(enum_value) != end() ? 1 : 0;
}

template<class enum_t>
std::pair<typename flags<enum_t>::iterator, typename flags<enum_t>::iterator>
    flags<enum_t>::equal_range(enum_type enum_value) const noexcept
{
    auto i = find(enum_value);
    auto j = i;
    return { i, ++j };
}

template<class enum_t>
template<class... args_t>
std::pair<typename flags<enum_t>::iterator, bool> flags<enum_t>::emplace(args_t&&... args) noexcept
{
    return insert(enum_type{ args... });
}

template<class enum_t>
template<class... args_t>
typename flags<enum_t>::iterator flags<enum_t>::emplace_hint(iterator, args_t&&... args) noexcept
{
    return emplace(args...).first;
}

template<class enum_t>
std::pair<typename flags<enum_t>::iterator, bool> flags<enum_t>::insert(enum_type enum_value) noexcept
{
    auto i = find(enum_value);
    if (i == end())
    {
        i.internal_mask = static_cast<implementation_type>(enum_value);
        internal_value |= i.internal_mask;
        update_uvalue(i);
        return { i, true };
    }
    return { i, false };
}

template<class enum_t>
std::pair<typename flags<enum_t>::iterator, bool> flags<enum_t>::insert(iterator, enum_type enum_value) noexcept
{
    return insert(enum_value);
}

template<class enum_t>
template<class forward_iter_t>
auto flags<enum_t>::insert(forward_iter_t begin, forward_iter_t end) noexcept(noexcept(++begin) && noexcept(
    *begin) && noexcept(begin == end)) -> typename convertable<decltype(*begin), void>::type
{
    internal_value |= std::accumulate(
        begin,
        end,
        implementation_type{ 0 },
        [](implementation_type i, enum_type enum_value) { return i | static_cast<implementation_type>(enum_value); });
}

template<class enum_t>
template<class container_t>
auto flags<enum_t>::insert(const container_t& ctn) noexcept -> decltype(std::begin(ctn), std::end(ctn), void())
{
    insert(std::begin(ctn), std::end(ctn));
}

template<class enum_t>
typename flags<enum_t>::iterator flags<enum_t>::erase(iterator i) noexcept
{
    internal_value ^= i.internal_mask;
    update_uvalue(i);
    return ++i;
}

template<class enum_t>
typename flags<enum_t>::size_type flags<enum_t>::erase(enum_type enum_value) noexcept
{
    auto e_count = count(enum_value);
    internal_value &= ~static_cast<implementation_type>(enum_value);
    return e_count;
}

template<class enum_t>
typename flags<enum_t>::iterator flags<enum_t>::erase(iterator begin, iterator end) noexcept
{
    internal_value ^= flags(begin, end).internal_value;
    update_uvalue(end);
    return end;
}

template<class enum_t>
void flags<enum_t>::clear() noexcept
{
    internal_value = 0;
}

template<class enum_t>
constexpr flags<enum_t>::flags(implementation_type val) noexcept : internal_value(val)
{}

template<class enum_t>
void flags<enum_t>::update_uvalue(iterator& it) const noexcept
{
    it.internal_unsigned_value = internal_value;
}

template<class enum_t>
void swap(flags<enum_t>& fl1, flags<enum_t>& fl2) noexcept
{
    fl1.swap(fl2);
}

template<class enum_t>
constexpr auto operator|(enum_t lhs, enum_t rhs) noexcept ->
    typename std::enable_if<is_flags<enum_t>::value, flags<enum_t>>::type
{
    return flags<enum_t>{ lhs } | rhs;
}

template<class enum_t>
constexpr auto operator&(enum_t lhs, enum_t rhs) noexcept ->
    typename std::enable_if<is_flags<enum_t>::value, flags<enum_t>>::type
{
    return flags<enum_t>{ lhs } & rhs;
}

template<class enum_t>
constexpr auto operator^(enum_t lhs, enum_t rhs) noexcept ->
    typename std::enable_if<is_flags<enum_t>::value, flags<enum_t>>::type
{
    return flags<enum_t>{ lhs } ^ rhs;
}

#endif
