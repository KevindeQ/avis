#ifndef AVIS_ENUM_FLAGS_H
#define AVIS_ENUM_FLAGS_H

#include <bitset>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <numeric>
#include <type_traits>
#include <utility>

template<typename enum_t, typename enabler = void>
struct is_flags : public std::false_type
{};

#define ALLOW_FLAGS_FOR_ENUM(name)         \
    template<>                             \
    struct is_flags<name> : std::true_type \
    {};

namespace avis
{
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

        using value_type = enum_t;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = const value_type;
        using iterator_category = std::forward_iterator_tag;

        constexpr flags_iterator() noexcept : internal_unsigned_value(0), internal_mask(0) {}
        constexpr flags_iterator(const flags_iterator& other) noexcept :
            internal_unsigned_value(other.internal_unsigned_value),
            internal_mask(other.internal_mask)
        {}

        flags_iterator& operator++() noexcept
        {
            next_mask();
            return *this;
        }

        flags_iterator operator++(int) noexcept
        {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        constexpr reference operator*() const noexcept
        {
            return static_cast<value_type>(internal_mask);
        }

        // Hidden friend: found through the iterator argument, no second declaration to clash with.
        // C++20 rewrites != in terms of ==.
        friend constexpr bool operator==(const flags_iterator& lhs, const flags_iterator& rhs) noexcept
        {
            return lhs.internal_mask == rhs.internal_mask;
        }

    private:
        template<class other_enum_t>
        friend class flags;

        using implementation_type = typename flags_type::implementation_type;

        explicit flags_iterator(implementation_type unsigned_value) noexcept :
            internal_unsigned_value(unsigned_value),
            internal_mask(1)
        {
            if (!(internal_mask & internal_unsigned_value))
            {
                next_mask();
            }
        }

        constexpr flags_iterator(implementation_type unsigned_value, enum_t enum_value) noexcept :
            internal_unsigned_value(unsigned_value),
            internal_mask(
                static_cast<implementation_type>(static_cast<implementation_type>(enum_value) & unsigned_value))
        {}

        void next_mask() noexcept
        {
            do
            {
                internal_mask <<= 1;
            }
            while (internal_mask && !(internal_mask & internal_unsigned_value));
        }

        implementation_type internal_unsigned_value;
        implementation_type internal_mask;
    };

    template<class enum_t>
    class flags
    {
    public:
        static_assert(
            is_flags<enum_t>::value, "Enum flags is disallowed for this type; use ALLOW_FLAGS_FOR_ENUM macro.");

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
        constexpr flags(enum_type enum_value) noexcept : internal_value(static_cast<implementation_type>(enum_value)) {}
        explicit constexpr flags(empty_t) noexcept : internal_value(0) {}

        flags(std::initializer_list<enum_type> il) noexcept : internal_value(0)
        {
            insert(il);
        }

        template<class... args_t>
        flags(enum_type enum_value, args_t... args) noexcept : flags{ enum_value, args... }
        {}

        template<class forward_iter_t>
        flags(
            forward_iter_t begin,
            forward_iter_t enum_value,
            typename convertable<decltype(*std::declval<forward_iter_t>())>::type =
                nullptr) noexcept(noexcept(std::declval<flags>()
                                               .insert(
                                                   std::declval<forward_iter_t>(), std::declval<forward_iter_t>()))) :
            internal_value(0)
        {
            insert(begin, enum_value);
        }

        flags(const flags& fl) noexcept = default;
        flags(flags&& fl) noexcept = default;

        flags& operator=(enum_type enum_value) noexcept
        {
            internal_value = static_cast<implementation_type>(enum_value);
            return *this;
        }

        flags& operator=(std::initializer_list<enum_type> il) noexcept
        {
            clear();
            insert(il);
            return *this;
        }

        flags& operator=(const flags& fl) noexcept = default;
        flags& operator=(flags&& fl) noexcept = default;

        constexpr explicit operator bool() const noexcept
        {
            return internal_value != 0;
        }

        constexpr bool operator!() const noexcept
        {
            return !internal_value;
        }

        constexpr flags operator~() const noexcept
        {
            return flags(~internal_value);
        }

        flags& operator|=(const flags& fl) noexcept
        {
            internal_value |= fl.internal_value;
            return *this;
        }

        flags& operator&=(const flags& fl) noexcept
        {
            internal_value &= fl.internal_value;
            return *this;
        }

        flags& operator^=(const flags& fl) noexcept
        {
            internal_value ^= fl.internal_value;
            return *this;
        }

        flags& operator|=(enum_type enum_value) noexcept
        {
            internal_value |= static_cast<implementation_type>(enum_value);
            return *this;
        }

        flags& operator&=(enum_type enum_value) noexcept
        {
            internal_value &= static_cast<implementation_type>(enum_value);
            return *this;
        }

        flags& operator^=(enum_type enum_value) noexcept
        {
            internal_value ^= static_cast<implementation_type>(enum_value);
            return *this;
        }

        // Hidden friends: found through a flags argument (argument-dependent lookup), so they can't clash with
        // other declarations. The enum operand converts through the implicit flags(enum_type) constructor, so
        // flags | enum, enum | flags and flags | flags all use these. enum | enum is the global template below the
        // class. C++20 rewrites != in terms of ==.
        friend constexpr flags operator|(const flags lhs, const flags rhs) noexcept
        {
            return flags(static_cast<implementation_type>(lhs.internal_value | rhs.internal_value));
        }

        friend constexpr flags operator&(const flags lhs, const flags rhs) noexcept
        {
            return flags(static_cast<implementation_type>(lhs.internal_value & rhs.internal_value));
        }

        friend constexpr flags operator^(const flags lhs, const flags rhs) noexcept
        {
            return flags(static_cast<implementation_type>(lhs.internal_value ^ rhs.internal_value));
        }

        friend constexpr bool operator==(const flags lhs, const flags rhs) noexcept
        {
            return lhs.internal_value == rhs.internal_value;
        }

        friend void swap(flags& lhs, flags& rhs) noexcept
        {
            lhs.swap(rhs);
        }

        void swap(flags& fl) noexcept
        {
            std::swap(internal_value, fl.internal_value);
        }

        constexpr underlying_type underlying_value() const noexcept
        {
            return static_cast<underlying_type>(internal_value);
        }

        void set_underlying_value(underlying_type new_value) noexcept
        {
            internal_value = static_cast<implementation_type>(new_value);
        }

        constexpr static std::size_t bit_size()
        {
            return sizeof(implementation_type) * CHAR_BIT;
        }

        constexpr explicit operator std::bitset<flags<enum_t>::bit_size()>() const noexcept
        {
            return to_bitset();
        }

        constexpr std::bitset<flags<enum_t>::bit_size()> to_bitset() const noexcept
        {
            return { internal_value };
        }

        constexpr bool empty() const noexcept
        {
            return !internal_value;
        }

        size_type size() const noexcept
        {
            return std::distance(this->begin(), this->end());
        }

        constexpr size_type max_size() const noexcept
        {
            return bit_size();
        }

        iterator begin() const noexcept
        {
            return cbegin();
        }

        constexpr iterator end() const noexcept
        {
            return cend();
        }

        iterator cbegin() const noexcept
        {
            return iterator{ internal_value };
        }

        constexpr iterator cend() const noexcept
        {
            return {};
        }

        constexpr iterator find(enum_type enum_value) const noexcept
        {
            return { internal_value, enum_value };
        }

        constexpr size_type count(enum_type enum_value) const noexcept
        {
            return find(enum_value) != end() ? 1 : 0;
        }

        std::pair<iterator, iterator> equal_range(enum_type enum_value) const noexcept
        {
            auto i = find(enum_value);
            auto j = i;
            return { i, ++j };
        }

        template<class... args_t>
        std::pair<iterator, bool> emplace(args_t&&... args) noexcept
        {
            return insert(enum_type{ args... });
        }

        template<class... args_t>
        iterator emplace_hint(iterator, args_t&&... args) noexcept
        {
            return emplace(args...).first;
        }

        std::pair<iterator, bool> insert(enum_type enum_value) noexcept
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

        std::pair<iterator, bool> insert(iterator, enum_type enum_value) noexcept
        {
            return insert(enum_value);
        }

        template<class forward_iter_t>
        auto insert(forward_iter_t begin, forward_iter_t end) noexcept(
            noexcept(++begin) && noexcept(*begin) && noexcept(begin == end)) ->
            typename convertable<decltype(*begin), void>::type
        {
            internal_value |= std::accumulate(
                begin,
                end,
                implementation_type{ 0 },
                [](implementation_type i, enum_type enum_value)
                { return i | static_cast<implementation_type>(enum_value); });
        }

        template<class container_t>
        auto insert(const container_t& ctn) noexcept -> decltype(std::begin(ctn), std::end(ctn), void())
        {
            insert(std::begin(ctn), std::end(ctn));
        }

        iterator erase(iterator i) noexcept
        {
            internal_value ^= i.internal_mask;
            update_uvalue(i);
            return ++i;
        }

        size_type erase(enum_type enum_value) noexcept
        {
            auto e_count = count(enum_value);
            internal_value &= ~static_cast<implementation_type>(enum_value);
            return e_count;
        }

        iterator erase(iterator begin, iterator end) noexcept
        {
            internal_value ^= flags(begin, end).internal_value;
            update_uvalue(end);
            return end;
        }

        void clear() noexcept
        {
            internal_value = 0;
        }

    private:
        constexpr explicit flags(implementation_type val) noexcept : internal_value(val) {}

        void update_uvalue(iterator& it) const noexcept
        {
            it.internal_unsigned_value = internal_value;
        }

        implementation_type internal_value;
    };
} // namespace avis

// enum | enum, enum & enum and enum ^ enum for enums marked with ALLOW_FLAGS_FOR_ENUM. Global, because the
// enums live in any namespace; the results are combined further by the hidden friends of avis::flags.
template<class enum_t>
constexpr auto operator|(enum_t lhs, enum_t rhs) noexcept ->
    typename std::enable_if<is_flags<enum_t>::value, avis::flags<enum_t>>::type
{
    return avis::flags<enum_t>(lhs) |
           rhs; // parentheses: the constexpr single-enum constructor, not the initializer list
}

template<class enum_t>
constexpr auto operator&(enum_t lhs, enum_t rhs) noexcept ->
    typename std::enable_if<is_flags<enum_t>::value, avis::flags<enum_t>>::type
{
    return avis::flags<enum_t>(lhs) &
           rhs; // parentheses: the constexpr single-enum constructor, not the initializer list
}

template<class enum_t>
constexpr auto operator^(enum_t lhs, enum_t rhs) noexcept ->
    typename std::enable_if<is_flags<enum_t>::value, avis::flags<enum_t>>::type
{
    return avis::flags<enum_t>(lhs) ^
           rhs; // parentheses: the constexpr single-enum constructor, not the initializer list
}

#endif
