#ifndef RUNTIME_IO_IO_READ_REQUEST_H
#define RUNTIME_IO_IO_READ_REQUEST_H

#include "avis/core/common.h"
#include "avis/core/streams/memory_stream.h"
#include "avis/runtime/io/io_request.h"

namespace io
{
    template<typename response_t>
    struct io_read_request : public io_request<response_t>
    {
    public:
        explicit io_read_request(
            file_descriptor descriptor, std::function<response_t(const streams::memory_stream& stream)> converter);

        void process() override;

    private:
        void internal_process(basic_file_descriptor* const descriptor);

    private:
        std::function<response_t(const streams::memory_stream& stream)> converter_;
    };

    template<>
    struct io_read_request<streams::memory_stream> : public io_request<streams::memory_stream>
    {
    public:
        explicit io_read_request(file_descriptor descriptor);

        void process() override;

    private:
        void internal_process(basic_file_descriptor* const descriptor);
    };

    template<typename response_t>
    inline io_read_request<response_t>::io_read_request(
        file_descriptor descriptor, std::function<response_t(const streams::memory_stream& stream)> converter) :
        io_request<response_t>(descriptor),

        converter_{converter}
    {}

    template<typename response_t>
    inline void io_read_request<response_t>::process()
    {
        basic_file_descriptor* internal_descriptor = basic_file_descriptor::from_file_descriptor(this->descriptor_);

        try
        {
            internal_process(internal_descriptor);
        }
        catch (...)
        {
            std::exception_ptr exception = std::current_exception();
            this->propagate_exception(exception);
        }
    }

    template<typename response_t>
    inline void io_read_request<response_t>::internal_process(basic_file_descriptor* const descriptor)
    {
        std::span<unsigned char> view = descriptor->acquire_view(0, 0);
        if (view.empty())
        {
            throw make_win32_error_code();
        }

        streams::memory_stream stream{view};
        response_t result = converter_(stream);

        this->propagate_response(std::move(result));
    }

    inline io_read_request<streams::memory_stream>::io_read_request(file_descriptor descriptor) :
        io_request<streams::memory_stream>(descriptor)
    {}

    inline void io_read_request<streams::memory_stream>::process()
    {
        basic_file_descriptor* internal_descriptor = basic_file_descriptor::from_file_descriptor(descriptor_);

        try
        {
            internal_process(internal_descriptor);
        }
        catch (...)
        {
            std::exception_ptr exception = std::current_exception();
            propagate_exception(exception);
        }
    }

    inline void io_read_request<streams::memory_stream>::internal_process(basic_file_descriptor* const descriptor)
    {
        std::span<unsigned char> view = descriptor->acquire_view(0, 0);
        if (view.empty())
        {
            throw make_win32_error_code();
        }

        streams::memory_stream stream{view};

        propagate_response(stream);
    }
} // namespace io

#endif
