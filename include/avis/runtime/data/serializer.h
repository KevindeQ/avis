#ifndef RUNTIME_DATA_SERIALIZER_H
#define RUNTIME_DATA_SERIALIZER_H

#include "avis/core/common.h"
#include "avis/runtime/data/field.h"
#include "avis/runtime/data/field_extensions.h"
#include "avis/runtime/data/page_view.h"

namespace data
{
    template<typename record_t>
    class serializer_lookup
    {
    public:
        using record_type = record_t;

        static void invoke_read(record_type& instance, const std::size_t offset_in_bytes, page_view& data_view);
        static void invoke_write(record_type& instance, const std::size_t offset_in_bytes, page_view& data_view);

        static std::size_t record_size(record_type& instance);

    private:
        class basic_context
        {
        public:
            basic_context(record_type& instance, const std::size_t offset_in_bytes, page_view& data_view);

        protected:
            record_type& object;
            std::size_t offset;
            page_view& data;
        };

        class reader_context : public basic_context
        {
        public:
            reader_context(record_type& instance, const std::size_t offset_in_bytes, page_view& data_view);

            template<auto member_ptr>
            void serialize();
        };

        class writer_context : public basic_context
        {
        public:
            writer_context(record_type& instance, const std::size_t offset_in_bytes, page_view& data_view);

            template<auto member_ptr>
            void serialize();
        };

        class sizing_context
        {
        public:
            sizing_context(record_type& instance);

            template<auto member_ptr>
            void serialize();

            std::size_t total_record_size() const;

        private:
            record_type& object;
            std::size_t record_size;
        };
    };

    // Provide a forward declaration so it can be specialized for user-defined types
    template<typename record_t>
    class serializer;

    template<typename record_t>
    void serializer_lookup<record_t>::invoke_read(
        record_type& instance, const std::size_t offset_in_bytes, page_view& data_view)
    {
        // TODO: Remove overhead of creating a new context for each read
        reader_context context{ instance, offset_in_bytes, data_view };

        // Rely on ADL to find the correct type of serializer
        serializer<record_t> s{};
        s.serialize(context);
    }

    template<typename record_t>
    void serializer_lookup<record_t>::invoke_write(
        record_type& instance, const std::size_t offset_in_bytes, page_view& data_view)
    {
        // TODO: Remove overhead of creating a new context for each write
        writer_context context{ instance, offset_in_bytes, data_view };

        // Rely on ADL to find the correct type of serializer
        serializer<record_t> s{};
        s.serialize(context);
    }

    template<typename record_t>
    std::size_t serializer_lookup<record_t>::record_size(record_type& instance)
    {
        sizing_context context{ instance };

        // Rely on ADL to find the correct type of serializer
        serializer<record_t> s{};
        s.serialize(context);

        return context.total_record_size();
    }

    template<typename record_t>
    serializer_lookup<record_t>::basic_context::basic_context(
        record_type& instance, const std::size_t offset_in_bytes, page_view& data_view) :
        object{ instance },
        offset{ offset_in_bytes },
        data{ data_view }
    {}

    template<typename record_t>
    serializer_lookup<record_t>::reader_context::reader_context(
        record_type& instance, const std::size_t offset_in_bytes, page_view& data_view) :
        basic_context(instance, offset_in_bytes, data_view)
    {}

    template<typename record_t>
    template<auto member_ptr>
    void serializer_lookup<record_t>::reader_context::serialize()
    {
        using field_definition = field<member_ptr>;
        typename field_definition::field_type value =
            field_reader<typename field_definition::field_type>::read(this->data, this->offset);

        field_definition::set_value(this->object, std::move(value));
    }

    template<typename record_t>
    serializer_lookup<record_t>::writer_context::writer_context(
        record_type& instance, const std::size_t offset_in_bytes, page_view& data_view) :
        basic_context(instance, offset_in_bytes, data_view)
    {}

    template<typename record_t>
    template<auto member_ptr>
    void serializer_lookup<record_t>::writer_context::serialize()
    {
        using field_definition = field<member_ptr>;
        typename field_definition::field_type value = field_writer<typename field_definition::field_type>::write(
            field_definition::get_value(this->object), this->offset, this->data);
    }

    template<typename record_t>
    serializer_lookup<record_t>::sizing_context::sizing_context(record_type& instance) :
        object{ instance },
        record_size{ 0 }
    {}

    template<typename record_t>
    template<auto member_ptr>
    void serializer_lookup<record_t>::sizing_context::serialize()
    {
        using field_definition = field<member_ptr>;
        record_size += field_definition::size();
    }

    template<typename record_t>
    std::size_t serializer_lookup<record_t>::sizing_context::total_record_size() const
    {
        return record_size;
    }
} // namespace data

#endif
