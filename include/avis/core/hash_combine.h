#ifndef CORE_HASH_COMBINE_H
#    define CORE_HASH_COMBINE_H

inline std::size_t hash_mix(std::size_t value)
{
    if constexpr (sizeof(std::size_t) == 8)
    {
        std::uint64_t const m = 0xe9846af9b1a615d;

        value ^= value >> 32;
        value *= m;
        value ^= value >> 32;
        value *= m;
        value ^= value >> 28;

        return value;
    }
    else if constexpr (sizeof(std::size_t) == 4)
    {
        std::uint32_t const m1 = 0x21f0aaad;
        std::uint32_t const m2 = 0x735a2d97;

        value ^= value >> 16;
        value *= m1;
        value ^= value >> 15;
        value *= m2;
        value ^= value >> 15;

        return value;
    }
}

// Based on the implementation of boost::hash_combine
template<class value_type>
inline void hash_combine(std::size_t& seed, value_type const& value)
{
    using std::hash;
    seed = hash_mix(seed + 0x9e3779b9 + hash<value_type>()(value));
}

#endif