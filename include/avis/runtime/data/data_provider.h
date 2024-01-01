#ifndef RUNTIME_DATA_DATA_PROVIDER_H
#define RUNTIME_DATA_DATA_PROVIDER_H

#include "avis/core/common.h"
#include "avis/core/dynamic_bitset.h"
#include "avis/runtime/data/page_controller.h"
#include "avis/runtime/data/page_view.h"
#include "avis/runtime/data/serializer.h"
#include "avis/runtime/io/file_descriptor.h"

namespace data
{
    template<typename record_t>
    class record_iterator;

    template<typename record_t>
    bool operator==(const record_iterator<record_t>& lhs, const record_iterator<record_t>& rhs);
    template<typename record_t>
    bool operator!=(const record_iterator<record_t>& lhs, const record_iterator<record_t>& rhs);

    template<typename record_t>
    struct record_iterator
    {
    public:
        using value_type = record_t;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::input_iterator_tag;

        record_iterator(
            page_view& data_page,
            const std::size_t record_size_in_bytes,
            const std::uint16_t record_count_in_page,
            const std::uint16_t record_index_in_page,
            dynamic_bitset<unsigned char>& record_usage_in_page);

        reference operator*();
        pointer operator->();

        record_iterator& operator++();
        record_iterator operator++(int);

        friend bool operator==<>(const record_iterator& lhs, const record_iterator& rhs);
        friend bool operator!=<>(const record_iterator& lhs, const record_iterator& rhs);

    private:
        std::uint16_t load_next_record(const std::uint16_t start_index, const std::uint16_t end_index);
        void unload_current_record();

        inline void invalidate_iterator();
        inline bool valid_index_range() const;

        inline pointer storage_as_record();

        page_view& data;
        std::size_t record_size;
        std::uint16_t record_count;
        std::uint16_t record_index;
        dynamic_bitset<unsigned char>& record_usage;

        alignas(value_type) std::array<std::byte, sizeof(value_type)> record_storage;
    };

    template<typename record_t, typename data_index_t>
    class data_provider
    {
    public:
        using record_type = record_t;
        using index_type = data_index_t;

        data_provider(io::basic_file_descriptor& data, io::basic_file_descriptor& index);

        std::vector<record_type> query();

    private:
        io::basic_file_descriptor file_data;
        page_controller data_page_controller;
        io::basic_file_descriptor file_index;
        page_controller index_page_controller;
    };

    template<typename record_t>
    record_iterator<record_t>::record_iterator(
        page_view& data_page,
        const std::size_t record_size_in_bytes,
        const std::uint16_t record_count_in_page,
        const std::uint16_t record_index_in_page,
        dynamic_bitset<unsigned char>& record_usage_in_page) :
        data{ data_page },
        record_size{ record_size_in_bytes },
        record_count{ record_count_in_page },
        record_index{ record_index_in_page },
        record_usage{ record_usage_in_page }
    {
        // Adjust the record index in case a record is not valid
        record_index = load_next_record(record_index, record_count);
        if (!valid_index_range())
        {
            // Invalidate iterator if there is no valid record in the current data page
            invalidate_iterator();
        }
    }

    template<typename record_t>
    record_iterator<record_t>::reference record_iterator<record_t>::operator*()
    {
        if (!valid_index_range())
        {
            throw std::out_of_range{ "Record index out-of-range" };
        }

        return *storage_as_record();
    }

    template<typename record_t>
    record_iterator<record_t>::pointer record_iterator<record_t>::operator->()
    {
        if (!valid_index_range())
        {
            throw std::out_of_range{ "Record index out-of-range" };
        }

        return storage_as_record();
    }

    template<typename record_t>
    record_iterator<record_t>& record_iterator<record_t>::operator++()
    {
        record_index += 1;
        if (valid_index_range())
        {
            // Adjust the record index in case a record is not valid
            record_index = load_next_record(record_index, record_count);
        }
        else
        {
            // Invalidate iterator if it goes past the end of the record range
            invalidate_iterator();
        }

        return *this;
    }

    template<typename record_t>
    record_iterator<record_t> record_iterator<record_t>::operator++(int)
    {
        record_iterator temp{ data, record_size, record_count, record_index, record_usage };

        record_index += 1;
        if (valid_index_range())
        {
            // Adjust the record index in case a record is not valid
            record_index = load_next_record(record_index, record_count);
        }
        else
        {
            // Invalidate iterator if it goes past the end of the record range
            invalidate_iterator();
        }

        return temp;
    }

    template<typename record_t>
    std::uint16_t record_iterator<record_t>::load_next_record(const std::uint16_t start_index, const std::uint16_t end_index)
    {
        for (std::uint16_t index = start_index; index < end_index; ++index)
        {
            bool used = record_usage.test(index);
            if (!used)
            {
                continue;
            }

            std::size_t base_offset = index * record_size;

            pointer new_record = new (record_storage.data()) value_type{};
            serializer_lookup<value_type>::invoke_read(*new_record, base_offset, data);

            return index;
        }

        // The variable end_index is expected to be one past the end of the valid range. Return to signify no valid record could be loaded.
        return end_index;
    }

    template<typename record_t>
    void record_iterator<record_t>::unload_current_record()
    {
        pointer current_record = storage_as_record();

        // Manually call the destructor since it was placed using placement-new
        current_record->~record_t();
    }

    template<typename record_t>
    inline void record_iterator<record_t>::invalidate_iterator()
    {
        record_count = 0;
        record_index = 0;
    }

    template<typename record_t>
    inline bool record_iterator<record_t>::valid_index_range() const
    {
        return record_index < record_count;
    }

    template<typename record_t>
    inline record_iterator<record_t>::pointer record_iterator<record_t>::storage_as_record()
    {
        return reinterpret_cast<pointer>(record_storage.data());
    }

    template<typename record_t>
    bool operator==(const record_iterator<record_t>& lhs, const record_iterator<record_t>& rhs)
    {
        return lhs.data == rhs.data && lhs.record_size == rhs.record_size && lhs.record_count == rhs.record_count &&
               lhs.record_index == rhs.record_index && lhs.record_usage == rhs.record_usage;
    }

    template<typename record_t>
    bool operator!=(const record_iterator<record_t>& lhs, const record_iterator<record_t>& rhs)
    {
        return lhs.data != rhs.data || lhs.record_size != rhs.record_size || lhs.record_count != rhs.record_count ||
               lhs.record_index != rhs.record_index || lhs.record_usage != rhs.record_usage;
    }

    template<typename record_t, typename data_index_t>
    data_provider<record_t, data_index_t>::data_provider(
        io::basic_file_descriptor& data, io::basic_file_descriptor& index) :
        file_data{ data },
        data_page_controller{ data, 8192 },
        file_index{ index },
        index_page_controller{ index, 8192 }
    {}

    template<typename record_t, typename data_index_t>
    std::vector<typename data_provider<record_t, data_index_t>::record_type>
        data_provider<record_t, data_index_t>::query()
    {
        std::vector<typename data_provider<record_t, data_index_t>::record_type> result{};

        // TODO: Think of a way to determine the record size without creating a dummy record
        record_type dummy_record{};
        std::size_t record_size = serializer_lookup<record_type>::record_size(dummy_record);

        std::uint64_t page_count = data_page_controller.page_count();
        for (std::uint64_t page_index = 0; page_index < page_count; ++page_index)
        {
            page_view data_page = data_page_controller.fetch_page(page_index);

            std::optional<std::uint16_t> record_capacity = data_page.read_uint16_le();
            if (!record_capacity.has_value())
            {
                throw std::runtime_error{ "Invalid file format. Unable to read read page record capacity." };
            }

            std::optional<std::uint16_t> record_count = data_page.read_uint16_le();
            if (!record_count.has_value())
            {
                throw std::runtime_error{ "Invalid file format. Unable to read read page record count." };
            }

            // Make sure record_usage_byte_count is a multiple of dynamic_bitset<unsigned char>::bits_per_block
            constexpr auto bits_per_block = dynamic_bitset<unsigned char>::bits_per_block;
            std::size_t record_usage_byte_count =
                round_up(static_cast<std::size_t>(record_capacity.value()) / bits_per_block, bits_per_block);
            std::optional<std::vector<unsigned char>> record_usage_data = data_page.read_bytes(record_usage_byte_count);
            if (!record_usage_data.has_value())
            {
                throw std::runtime_error{ "Invalid file format. Unable to read page record usage map." };
            }

            dynamic_bitset<unsigned char> record_usage{ record_usage_data->cbegin(), record_usage_data->cend() };
            record_usage.resize(record_count.value());

            record_iterator<record_t> first{ data_page, record_size, record_count.value(), 0, record_usage };
            record_iterator<record_t> last{
                data_page, record_size, record_count.value(), record_count.value(), record_usage
            };
            for (; first != last; ++first)
            {
                // TODO: Add filtering based on predicates
                result.push_back(*first);
            }
        }

        return result;
    }
} // namespace data

#endif
