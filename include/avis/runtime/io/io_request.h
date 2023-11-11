#ifndef RUNTIME_IO_IO_REQUEST_H
#define RUNTIME_IO_IO_REQUEST_H

#include "avis/core/common.h"
#include "avis/runtime/io/file_descriptor.h"
#include "avis/runtime/parallel/thread_pool.h"

namespace io
{
    struct basic_io_request
    {
    public:
        ENGINE_API explicit basic_io_request(file_descriptor descriptor);

        virtual void process() = 0;

    protected:
        file_descriptor descriptor_;
    };

    template<typename response_t>
    struct io_request : public basic_io_request
    {
    public:
        explicit io_request(file_descriptor descriptor);

        void propagate_response(const response_t& response);
        void propagate_exception(const std::exception_ptr& exception);

        std::future<response_t> get_future();

    private:
        std::promise<response_t> response_value_;
    };

    template<>
    struct io_request<void> : public basic_io_request
    {
    public:
        explicit io_request(file_descriptor descriptor);

        void propagate_exception(const std::exception_ptr& exception);

        std::future<void> get_future();

    private:
        std::promise<void> response_value_;
    };

    template<typename response_t>
    inline io_request<response_t>::io_request(file_descriptor descriptor) : basic_io_request(descriptor)
    {}

    template<typename response_t>
    inline void io_request<response_t>::propagate_response(const response_t& response)
    {
        response_value_.set_value(response);
    }

    template<typename response_t>
    inline void io_request<response_t>::propagate_exception(const std::exception_ptr& exception)
    {
        response_value_.set_exception(exception);
    }

    template<typename response_t>
    inline std::future<response_t> io_request<response_t>::get_future()
    {
        return response_value_.get_future();
    }

    inline io_request<void>::io_request(file_descriptor descriptor) : basic_io_request(descriptor) {}

    inline void io_request<void>::propagate_exception(const std::exception_ptr& exception)
    {
        response_value_.set_exception(exception);
    }

    inline std::future<void> io_request<void>::get_future()
    {
        return response_value_.get_future();
    }
} // namespace io

#endif
