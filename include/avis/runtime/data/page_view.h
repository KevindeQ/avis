#ifndef RUNTIME_DATA_PAGE_VIEW_H
#define RUNTIME_DATA_PAGE_VIEW_H

#include "avis/core/common.h"

namespace data
{
    struct page_view
    {
    public:
        page_view();
        page_view(std::uint64_t id, std::span<unsigned char> page_data_view);

        inline std::optional<bool> read_bool_le();
        inline std::optional<bool> read_bool_le(const std::size_t offset) const;
        inline std::optional<bool> read_bool_be();
        inline std::optional<bool> read_bool_be(const std::size_t offset) const;

        inline std::optional<std::int8_t> read_int8_le();
        inline std::optional<std::int8_t> read_int8_le(const std::size_t offset) const;
        inline std::optional<std::int8_t> read_int8_be();
        inline std::optional<std::int8_t> read_int8_be(const std::size_t offset) const;
        inline std::optional<std::int16_t> read_int16_le();
        inline std::optional<std::int16_t> read_int16_le(const std::size_t offset) const;
        inline std::optional<std::int16_t> read_int16_be();
        inline std::optional<std::int16_t> read_int16_be(const std::size_t offset) const;
        inline std::optional<std::int32_t> read_int32_le();
        inline std::optional<std::int32_t> read_int32_le(const std::size_t offset) const;
        inline std::optional<std::int32_t> read_int32_be();
        inline std::optional<std::int32_t> read_int32_be(const std::size_t offset) const;
        inline std::optional<std::int64_t> read_int64_le();
        inline std::optional<std::int64_t> read_int64_le(const std::size_t offset) const;
        inline std::optional<std::int64_t> read_int64_be();
        inline std::optional<std::int64_t> read_int64_be(const std::size_t offset) const;

        inline std::optional<std::uint8_t> read_uint8_le();
        inline std::optional<std::uint8_t> read_uint8_le(const std::size_t offset) const;
        inline std::optional<std::uint8_t> read_uint8_be();
        inline std::optional<std::uint8_t> read_uint8_be(const std::size_t offset) const;
        inline std::optional<std::uint16_t> read_uint16_le();
        inline std::optional<std::uint16_t> read_uint16_le(const std::size_t offset) const;
        inline std::optional<std::uint16_t> read_uint16_be();
        inline std::optional<std::uint16_t> read_uint16_be(const std::size_t offset) const;
        inline std::optional<std::uint32_t> read_uint32_le();
        inline std::optional<std::uint32_t> read_uint32_le(const std::size_t offset) const;
        inline std::optional<std::uint32_t> read_uint32_be();
        inline std::optional<std::uint32_t> read_uint32_be(const std::size_t offset) const;
        inline std::optional<std::uint64_t> read_uint64_le();
        inline std::optional<std::uint64_t> read_uint64_le(const std::size_t offset) const;
        inline std::optional<std::uint64_t> read_uint64_be();
        inline std::optional<std::uint64_t> read_uint64_be(const std::size_t offset) const;

        inline std::optional<float> read_float_le();
        inline std::optional<float> read_float_le(const std::size_t offset) const;
        inline std::optional<float> read_float_be();
        inline std::optional<float> read_float_be(const std::size_t offset) const;

        inline std::optional<std::vector<unsigned char>> read_bytes(const std::size_t count) const;
        inline std::optional<std::vector<unsigned char>>
            read_bytes(const std::size_t count, const std::size_t offset) const;

        inline void write_bool_le(const bool value);
        inline void write_bool_le(const bool value, const std::size_t offset);
        inline void write_bool_be(const bool value);
        inline void write_bool_be(const bool value, const std::size_t offset);

        inline void write_int8_le(const std::int8_t value);
        inline void write_int8_le(const std::int8_t value, const std::size_t offset);
        inline void write_int8_be(const std::int8_t value);
        inline void write_int8_be(const std::int8_t value, const std::size_t offset);
        inline void write_int16_le(const std::int16_t value);
        inline void write_int16_le(const std::int16_t value, const std::size_t offset);
        inline void write_int16_be(const std::int16_t value);
        inline void write_int16_be(const std::int16_t value, const std::size_t offset);
        inline void write_int32_le(const std::int32_t value);
        inline void write_int32_le(const std::int32_t value, const std::size_t offset);
        inline void write_int32_be(const std::int32_t value);
        inline void write_int32_be(const std::int32_t value, const std::size_t offset);
        inline void write_int64_le(const std::int64_t value);
        inline void write_int64_le(const std::int64_t value, const std::size_t offset);
        inline void write_int64_be(const std::int64_t value);
        inline void write_int64_be(const std::int64_t value, const std::size_t offset);

        inline void write_uint8_le(const std::uint8_t value);
        inline void write_uint8_le(const std::uint8_t value, const std::size_t offset);
        inline void write_uint8_be(const std::uint8_t value);
        inline void write_uint8_be(const std::uint8_t value, const std::size_t offset);
        inline void write_uint16_le(const std::uint16_t value);
        inline void write_uint16_le(const std::uint16_t value, const std::size_t offset);
        inline void write_uint16_be(const std::uint16_t value);
        inline void write_uint16_be(const std::uint16_t value, const std::size_t offset);
        inline void write_uint32_le(const std::uint32_t value);
        inline void write_uint32_le(const std::uint32_t value, const std::size_t offset);
        inline void write_uint32_be(const std::uint32_t value);
        inline void write_uint32_be(const std::uint32_t value, const std::size_t offset);
        inline void write_uint64_le(const std::uint64_t value);
        inline void write_uint64_le(const std::uint64_t value, const std::size_t offset);
        inline void write_uint64_be(const std::uint64_t value);
        inline void write_uint64_be(const std::uint64_t value, const std::size_t offset);

        inline void write_float_le(const float value);
        inline void write_float_le(const float value, const std::size_t offset);
        inline void write_float_be(const float value);
        inline void write_float_be(const float value, const std::size_t offset);

        void clear();

        inline const std::uint64_t id() const;

        friend bool operator==(const page_view& lhs, const page_view& rhs);
        friend bool operator!=(const page_view& lhs, const page_view& rhs);

    private:
        struct tag_little_endian
        {};
        struct tag_big_endian
        {};

        template<typename data_type, typename tag_endianness>
        std::optional<data_type> read_bytes_sequential(tag_endianness endianness);
        template<typename data_type>
        std::optional<data_type> read_bytes_raw(const std::size_t offset, [[maybe_unused]] tag_little_endian) const;
        template<typename data_type>
        std::optional<data_type> read_bytes_raw(const std::size_t offset, [[maybe_unused]] tag_big_endian) const;

        template<typename data_type, typename tag_endianness>
        bool write_bytes_sequential(const data_type& value, tag_endianness endianness);
        template<typename data_type>
        bool write_bytes_raw(const data_type& value, const std::size_t offset, [[maybe_unused]] tag_little_endian);
        template<typename data_type>
        bool write_bytes_raw(const data_type& value, const std::size_t offset, [[maybe_unused]] tag_big_endian);

        std::uint64_t page_id;
        std::span<unsigned char> data_view;

        // Used to keep track of sequential reading and/or writing
        std::size_t current_offset;
    };

    inline std::optional<bool> page_view::read_bool_le()
    {
        return read_bytes_sequential<bool>(tag_little_endian{});
    }

    inline std::optional<bool> page_view::read_bool_le(const std::size_t offset) const
    {
        return read_bytes_raw<bool>(offset, tag_little_endian{});
    }

    inline std::optional<bool> page_view::read_bool_be()
    {
        return read_bytes_sequential<bool>(tag_big_endian{});
    }

    inline std::optional<bool> page_view::read_bool_be(const std::size_t offset) const
    {
        return read_bytes_raw<bool>(offset, tag_big_endian{});
    }

    inline std::optional<std::int8_t> page_view::read_int8_le()
    {
        return read_bytes_sequential<std::int8_t>(tag_little_endian{});
    }

    inline std::optional<std::int8_t> page_view::read_int8_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::int8_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::int8_t> page_view::read_int8_be()
    {
        return read_bytes_sequential<bool>(tag_big_endian{});
    }

    inline std::optional<std::int8_t> page_view::read_int8_be(const std::size_t offset) const
    {
        return read_bytes_raw<bool>(offset, tag_big_endian{});
    }

    inline std::optional<std::int16_t> page_view::read_int16_le()
    {
        return read_bytes_sequential<std::int16_t>(tag_little_endian{});
    }

    inline std::optional<std::int16_t> page_view::read_int16_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::int16_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::int16_t> page_view::read_int16_be()
    {
        return read_bytes_sequential<std::int16_t>(tag_big_endian{});
    }

    inline std::optional<std::int16_t> page_view::read_int16_be(const std::size_t offset) const
    {
        return read_bytes_raw<std::int16_t>(offset, tag_big_endian{});
    }

    inline std::optional<std::int32_t> page_view::read_int32_le()
    {
        return read_bytes_sequential<std::int32_t>(tag_little_endian{});
    }

    inline std::optional<std::int32_t> page_view::read_int32_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::int32_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::int32_t> page_view::read_int32_be()
    {
        return read_bytes_sequential<std::int32_t>(tag_big_endian{});
    }

    inline std::optional<std::int32_t> page_view::read_int32_be(const std::size_t offset) const
    {
        return read_bytes_raw<std::int32_t>(offset, tag_big_endian{});
    }

    inline std::optional<std::int64_t> page_view::read_int64_le()
    {
        return read_bytes_sequential<std::int64_t>(tag_little_endian{});
    }

    inline std::optional<std::int64_t> page_view::read_int64_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::int64_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::int64_t> page_view::read_int64_be()
    {
        return read_bytes_sequential<std::int64_t>(tag_big_endian{});
    }

    inline std::optional<std::int64_t> page_view::read_int64_be(const std::size_t offset) const
    {
        return read_bytes_raw<std::int64_t>(offset, tag_big_endian{});
    }

    inline std::optional<std::uint8_t> page_view::read_uint8_le()
    {
        return read_bytes_sequential<std::uint8_t>(tag_little_endian{});
    }

    inline std::optional<std::uint8_t> page_view::read_uint8_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint8_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::uint8_t> page_view::read_uint8_be()
    {
        return read_bytes_sequential<std::uint8_t>(tag_big_endian{});
    }

    inline std::optional<std::uint8_t> page_view::read_uint8_be(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint8_t>(offset, tag_big_endian{});
    }

    inline std::optional<std::uint16_t> page_view::read_uint16_le()
    {
        return read_bytes_sequential<std::uint16_t>(tag_little_endian{});
    }

    inline std::optional<std::uint16_t> page_view::read_uint16_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint16_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::uint16_t> page_view::read_uint16_be()
    {
        return read_bytes_sequential<std::uint16_t>(tag_big_endian{});
    }

    inline std::optional<std::uint16_t> page_view::read_uint16_be(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint16_t>(offset, tag_big_endian{});
    }

    inline std::optional<std::uint32_t> page_view::read_uint32_le()
    {
        return read_bytes_sequential<std::uint32_t>(tag_little_endian{});
    }

    inline std::optional<std::uint32_t> page_view::read_uint32_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint32_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::uint32_t> page_view::read_uint32_be()
    {
        return read_bytes_sequential<std::uint32_t>(tag_big_endian{});
    }

    inline std::optional<std::uint32_t> page_view::read_uint32_be(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint32_t>(offset, tag_big_endian{});
    }

    inline std::optional<std::uint64_t> page_view::read_uint64_le()
    {
        return read_bytes_sequential<std::uint64_t>(tag_little_endian{});
    }

    inline std::optional<std::uint64_t> page_view::read_uint64_le(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint64_t>(offset, tag_little_endian{});
    }

    inline std::optional<std::uint64_t> page_view::read_uint64_be()
    {
        return read_bytes_sequential<std::uint64_t>(tag_big_endian{});
    }

    inline std::optional<std::uint64_t> page_view::read_uint64_be(const std::size_t offset) const
    {
        return read_bytes_raw<std::uint64_t>(offset, tag_big_endian{});
    }

    inline std::optional<float> page_view::read_float_le()
    {
        return read_bytes_sequential<float>(tag_little_endian{});
    }

    inline std::optional<float> page_view::read_float_le(const std::size_t offset) const
    {
        return read_bytes_raw<float>(offset, tag_little_endian{});
    }

    inline std::optional<float> page_view::read_float_be()
    {
        return read_bytes_sequential<float>(tag_big_endian{});
    }

    inline std::optional<float> page_view::read_float_be(const std::size_t offset) const
    {
        return read_bytes_raw<float>(offset, tag_big_endian{});
    }

    inline std::optional<std::vector<unsigned char>> page_view::read_bytes(const std::size_t count) const
    {
        return read_bytes(count, current_offset);
    }

    inline std::optional<std::vector<unsigned char>>
        page_view::read_bytes(const std::size_t count, const std::size_t offset) const
    {
        const std::size_t byte_count = sizeof(unsigned char);

        // Check if read won't pass the end of the buffer
        if (offset + (count * byte_count) > data_view.size())
        {
            return std::nullopt;
        }

        std::vector<unsigned char> result{};
        result.resize(count);

        unsigned char* source_ptr = data_view.data() + offset;
        unsigned char* destination_ptr = reinterpret_cast<unsigned char*>(&result);
        std::copy(source_ptr, source_ptr + byte_count, destination_ptr);

        return std::make_optional(result);
    }

    inline void page_view::write_bool_le(const bool value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_bool_le(const bool value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_bool_be(const bool value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_bool_be(const bool value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_int8_le(const std::int8_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_int8_le(const std::int8_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_int8_be(const std::int8_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_int8_be(const std::int8_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_int16_le(const std::int16_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_int16_le(const std::int16_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_int16_be(const std::int16_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_int16_be(const std::int16_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_int32_le(const std::int32_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_int32_le(const std::int32_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_int32_be(const std::int32_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_int32_be(const std::int32_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_int64_le(const std::int64_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_int64_le(const std::int64_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_int64_be(const std::int64_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_int64_be(const std::int64_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_uint8_le(const std::uint8_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_uint8_le(const std::uint8_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_uint8_be(const std::uint8_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_uint8_be(const std::uint8_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_uint16_le(const std::uint16_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_uint16_le(const std::uint16_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_uint16_be(const std::uint16_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_uint16_be(const std::uint16_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_uint32_le(const std::uint32_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_uint32_le(const std::uint32_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_uint32_be(const std::uint32_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_uint32_be(const std::uint32_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_uint64_le(const std::uint64_t value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_uint64_le(const std::uint64_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_uint64_be(const std::uint64_t value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_uint64_be(const std::uint64_t value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline void page_view::write_float_le(const float value)
    {
        write_bytes_sequential(value, tag_little_endian{});
    }

    inline void page_view::write_float_le(const float value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_little_endian{});
    }

    inline void page_view::write_float_be(const float value)
    {
        write_bytes_sequential(value, tag_big_endian{});
    }

    inline void page_view::write_float_be(const float value, const std::size_t offset)
    {
        write_bytes_raw(value, offset, tag_big_endian{});
    }

    inline const std::uint64_t page_view::id() const
    {
        return page_id;
    }

    template<typename data_type, typename tag_endianness>
    std::optional<data_type> page_view::read_bytes_sequential(tag_endianness endianness)
    {
        std::optional<data_type> value = read_bytes_raw<data_type>(current_offset, endianness);
        current_offset += sizeof(data_type);

        return value;
    }

    template<typename data_type>
    std::optional<data_type>
        page_view::read_bytes_raw(const std::size_t offset, [[maybe_unused]] tag_little_endian) const
    {
        const std::size_t byte_count = sizeof(data_type);

        // Check if read won't pass the end of the buffer
        if (offset + byte_count > data_view.size())
        {
            return std::nullopt;
        }

        data_type result{};

        unsigned char* source_ptr = data_view.data() + current_offset;
        unsigned char* destination_ptr = reinterpret_cast<unsigned char*>(&result);
        std::copy(source_ptr, source_ptr + byte_count, destination_ptr);

        return std::make_optional(result);
    }

    template<typename data_type>
    std::optional<data_type> page_view::read_bytes_raw(const std::size_t offset, [[maybe_unused]] tag_big_endian) const
    {
        const std::size_t byte_count = sizeof(data_type);

        // Check if read won't pass the end of the buffer
        if (offset + byte_count > data_view.size())
        {
            return std::nullopt;
        }

        data_type result{};

        unsigned char* source_ptr = data_view.data() + current_offset;
        unsigned char* destination_ptr = reinterpret_cast<unsigned char*>(&result);
        std::reverse_copy(source_ptr, source_ptr + byte_count, destination_ptr);

        return std::make_optional(result);
    }

    template<typename data_type, typename tag_endianness>
    bool page_view::write_bytes_sequential(const data_type& value, tag_endianness endianness)
    {
        bool result = write_bytes_raw<data_type>(value, current_offset, endianness);
        current_offset += sizeof(data_type);

        return result;
    }

    template<typename data_type>
    bool
        page_view::write_bytes_raw(const data_type& value, const std::size_t offset, [[maybe_unused]] tag_little_endian)
    {
        const std::size_t byte_count = sizeof(data_type);

        // Check if no bytes will be written past the end of the buffer
        if (offset + byte_count > data_view.size())
        {
            return false;
        }

        const unsigned char* source_ptr = reinterpret_cast<const unsigned char*>(&value);
        unsigned char* destination_ptr = data_view.data() + current_offset;
        std::copy(source_ptr, source_ptr + byte_count, destination_ptr);

        return true;
    }

    template<typename data_type>
    bool page_view::write_bytes_raw(const data_type& value, const std::size_t offset, [[maybe_unused]] tag_big_endian)
    {
        const std::size_t byte_count = sizeof(data_type);

        // Check if no bytes will be written past the end of the buffer
        if (offset + byte_count > data_view.size())
        {
            return false;
        }

        const unsigned char* source_ptr = reinterpret_cast<const unsigned char*>(&value) + byte_count - 1;
        unsigned char* destination_ptr = data_view.data() + current_offset;
        for (std::size_t index = 0; index < byte_count; ++destination_ptr)
        {
            *destination_ptr = *(source_ptr - index);
        }

        return true;
    }

    inline bool operator==(const page_view& lhs, const page_view& rhs)
    {
        return lhs.page_id == rhs.page_id;
    }

    inline bool operator!=(const page_view& lhs, const page_view& rhs)
    {
        return lhs.page_id != rhs.page_id;
    }
} // namespace data

#endif
