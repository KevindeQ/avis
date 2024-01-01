#ifndef RUNTIME_DATA_FIELD_H
#define RUNTIME_DATA_FIELD_H

#include "avis/core/common.h"
#include "avis/runtime/data/page_view.h"

namespace data
{
    namespace details
    {
        template<class T>
        struct member_type_helper;

        template<class C, class T>
        struct member_type_helper<T C::*>
        {
            using class_type = C;
            using field_type = T;
        };

        template<class T>
        struct member_type : member_type_helper<typename std::remove_cvref<T>::type>
        {};
    } // namespace details

    template<auto member_ptr>
    struct field
    {
        static_assert(
            std::is_member_object_pointer_v<decltype(member_ptr)>, "Only pointer to member variables are supported");

    public:
        using class_type = typename details::member_type<decltype(member_ptr)>::class_type;
        using field_type = typename details::member_type<decltype(member_ptr)>::field_type;

        static field_type get_value(class_type& instance);
        static void set_value(class_type& instance, field_type&& value);

        static std::size_t size() noexcept;
    };

    template<auto member_ptr>
    inline typename field<member_ptr>::field_type field<member_ptr>::get_value(class_type& instance)
    {
        return instance.*member_ptr;
    }

    template<auto member_ptr>
    inline void field<member_ptr>::set_value(class_type& instance, field_type&& value)
    {
        instance.*member_ptr = std::forward<field_type>(value);
    }

    template<auto member_ptr>
    inline std::size_t field<member_ptr>::size() noexcept
    {
        // TODO: Replace this with specific fixed values for the different data types. Currently, this is tied to the
        // specific c++ implementation.
        return sizeof(field_type);
    }

    class basic_field_reader
    {
    protected:
        template<typename field_type>
        static std::optional<field_type> read_value(page_view& data, const std::size_t offset_in_bytes);
    };

    template<typename data_type>
    class field_reader : public basic_field_reader
    {
    public:
        static data_type read(page_view& data, const std::size_t offset_in_bytes);
    };

    template<>
    inline std::optional<bool> basic_field_reader::read_value<bool>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_bool_le();
    }

    template<>
    inline std::optional<std::int8_t>
        basic_field_reader::read_value<std::int8_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_int8_le(offset_in_bytes);
    }

    template<>
    inline std::optional<std::int16_t>
        basic_field_reader::read_value<std::int16_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_int16_le(offset_in_bytes);
    }

    template<>
    inline std::optional<std::int32_t>
        basic_field_reader::read_value<std::int32_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_int32_le(offset_in_bytes);
    }

    template<>
    inline std::optional<std::int64_t>
        basic_field_reader::read_value<std::int64_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_int64_le(offset_in_bytes);
    }

    template<>
    inline std::optional<std::uint8_t>
        basic_field_reader::read_value<std::uint8_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_uint8_le(offset_in_bytes);
    }

    template<>
    inline std::optional<std::uint16_t>
        basic_field_reader::read_value<std::uint16_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_uint16_le(offset_in_bytes);
    }

    template<>
    inline std::optional<std::uint32_t>
        basic_field_reader::read_value<std::uint32_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_uint32_le(offset_in_bytes);
    }

    template<>
    inline std::optional<std::uint64_t>
        basic_field_reader::read_value<std::uint64_t>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_uint64_le(offset_in_bytes);
    }

    template<>
    inline std::optional<float>
        basic_field_reader::read_value<float>(page_view& data, const std::size_t offset_in_bytes)
    {
        return data.read_float_le(offset_in_bytes);
    }

    template<typename data_type>
    inline data_type field_reader<data_type>::read(page_view& data, const std::size_t offset_in_bytes)
    {
        std::optional<bool> read_result = read_value<data_type>(data, offset_in_bytes);
        if (!read_result.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read field value." };
        }

        return read_result.value();
    }

    class basic_field_writer
    {
    protected:
        template<typename field_type>
        static void write_value(const field_type value, const std::size_t offset_in_bytes, page_view& data);
    };

    template<typename data_type>
    class field_writer : public basic_field_writer
    {
    public:
        static void write(const data_type value, const std::size_t offset_in_bytes, page_view& data);
    };

    template<>
    inline void
        basic_field_writer::write_value<bool>(const bool value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_bool_le(value);
    }

    template<>
    inline void basic_field_writer::write_value<std::int8_t>(
        const std::int8_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_int8_le(value, offset_in_bytes);
    }

    template<>
    inline void basic_field_writer::write_value<std::int16_t>(
        const std::int16_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_int16_le(value, offset_in_bytes);
    }

    template<>
    inline void basic_field_writer::write_value<std::int32_t>(
        const std::int32_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_int32_le(value, offset_in_bytes);
    }

    template<>
    inline void basic_field_writer::write_value<std::int64_t>(
        const std::int64_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_int64_le(value, offset_in_bytes);
    }

    template<>
    inline void basic_field_writer::write_value<std::uint8_t>(
        const std::uint8_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_uint8_le(value, offset_in_bytes);
    }

    template<>
    inline void basic_field_writer::write_value<std::uint16_t>(
        const std::uint16_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_uint16_le(value, offset_in_bytes);
    }

    template<>
    inline void basic_field_writer::write_value<std::uint32_t>(
        const std::uint32_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_uint32_le(value, offset_in_bytes);
    }

    template<>
    inline void basic_field_writer::write_value<std::uint64_t>(
        const std::uint64_t value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_uint64_le(value, offset_in_bytes);
    }

    template<>
    inline void
        basic_field_writer::write_value<float>(const float value, const std::size_t offset_in_bytes, page_view& data)
    {
        return data.write_float_le(value, offset_in_bytes);
    }

    template<typename data_type>
    inline void
        field_writer<data_type>::write(const data_type value, const std::size_t offset_in_bytes, page_view& data)
    {
        write_value(value, offset_in_bytes, data);
    }
} // namespace data

#endif
