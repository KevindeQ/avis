#ifndef CORE_DYNAMIC_BITSET_H
#define CORE_DYNAMIC_BITSET_H

#include "avis/core/common.h"

template<typename block_t>
class dynamic_bitset;

// Forward declaration is necessary for the friend equality operators
template<typename block_t>
bool operator==(const dynamic_bitset<block_t>& lhs, const dynamic_bitset<block_t>& rhs);
template<typename block_t>
bool operator!=(const dynamic_bitset<block_t>& lhs, const dynamic_bitset<block_t>& rhs);

template<typename block_t = std::uint64_t>
class dynamic_bitset
{
public:
    using block_type = block_t;
    using size_type = std::size_t;

    static constexpr std::size_t bits_per_block = sizeof(block_type) * CHAR_BIT;

    template<typename block_input_iterator>
    dynamic_bitset(block_input_iterator first, block_input_iterator last);

    void resize(const size_type bit_count, bool value = false);

    void append(block_type block);

    template<typename block_input_iterator>
    void append(block_input_iterator first, block_input_iterator last);

    void clear();

    dynamic_bitset& set(size_type index, bool value = true);
    dynamic_bitset& reset(size_type index);
    dynamic_bitset& flip(size_type index);

    bool test(size_type index) const;

    bool empty() const noexcept;

    size_type size() const noexcept;
    size_type capacity() const noexcept;

    friend bool operator==<>(const dynamic_bitset<block_t>& lhs, const dynamic_bitset<block_t>& rhs);
    friend bool operator!=<>(const dynamic_bitset<block_t>& lhs, const dynamic_bitset<block_t>& rhs);

private:
    std::vector<block_type> blocks;
    size_type data_size;
};

template<typename block_t>
template<typename block_input_iterator>
dynamic_bitset<block_t>::dynamic_bitset(block_input_iterator first, block_input_iterator last) :
    blocks{ first, last },
    data_size{ blocks.size() * bits_per_block }
{}

template<typename block_t>
void dynamic_bitset<block_t>::resize(const size_type bit_count, bool value)
{
    size_type block_count = round_up(bit_count, bits_per_block);

    // Set all bits to 1 if value is true. Otherwise, set all bits to 0.
    block_type block_value = value ? ~0 : 0;

    blocks.resize(block_count, value);
}

template<typename block_t>
void dynamic_bitset<block_t>::append(block_type block)
{
    blocks.push_back(block);
    data_size += bits_per_block;
}

template<typename block_t>
template<typename block_input_iterator>
void dynamic_bitset<block_t>::append(block_input_iterator first, block_input_iterator last)
{
    blocks.insert(blocks.end(), first, last);
    data_size = blocks.size() * bits_per_block;
}

template<typename block_t>
void dynamic_bitset<block_t>::clear()
{
    blocks.clear();
    data_size = 0;
}

template<typename block_t>
dynamic_bitset<block_t>& dynamic_bitset<block_t>::set(size_type index, bool value)
{
    if (index >= data_size)
    {
        throw std::out_of_range{ "Bit index out-of-range" };
    }

    size_type block_index = index / bits_per_block;
    size_type bit_index = index % bits_per_block;

    block_type block = blocks[block_index];
    block = (block & ~(1u << bit_index)) | (static_cast<block_type>(value) << bit_index);
    blocks[block_index] = block;

    return *this;
}

template<typename block_t>
dynamic_bitset<block_t>& dynamic_bitset<block_t>::reset(size_type index)
{
    if (index >= data_size)
    {
        throw std::out_of_range{ "Bit index out-of-range" };
    }

    size_type block_index = index / bits_per_block;
    size_type bit_index = index % bits_per_block;
    blocks[block_index] & ~(1u << bit_index);

    return *this;
}

template<typename block_t>
dynamic_bitset<block_t>& dynamic_bitset<block_t>::flip(size_type index)
{
    if (index >= data_size)
    {
        throw std::out_of_range{ "Bit index out-of-range" };
    }

    size_type block_index = index / bits_per_block;
    size_type bit_index = index % bits_per_block;
    blocks[block_index] ^ (1u << bit_index);

    return *this;
}

template<typename block_t>
bool dynamic_bitset<block_t>::test(size_type index) const
{
    if (index >= data_size)
    {
        throw std::out_of_range{ "Bit index out-of-range" };
    }

    size_type block_index = index / bits_per_block;
    size_type bit_index = index % bits_per_block;
    return (blocks[block_index] >> bit_index) & 1u;
}

template<typename block_t>
bool dynamic_bitset<block_t>::empty() const noexcept
{
    return data_size == 0;
}

template<typename block_t>
dynamic_bitset<block_t>::size_type dynamic_bitset<block_t>::size() const noexcept
{
    return data_size;
}

template<typename block_t>
dynamic_bitset<block_t>::size_type dynamic_bitset<block_t>::capacity() const noexcept
{
    return blocks.size() * bits_per_block;
}

template<typename block_t>
bool operator==(const dynamic_bitset<block_t>& lhs, const dynamic_bitset<block_t>& rhs)
{
    return lhs.blocks == rhs.blocks && lhs.data_size == rhs.data_size;
}

template<typename block_t>
bool operator!=(const dynamic_bitset<block_t>& lhs, const dynamic_bitset<block_t>& rhs)
{
    return lhs.blocks != rhs.blocks || lhs.data_size != rhs.data_size;
}

#endif
