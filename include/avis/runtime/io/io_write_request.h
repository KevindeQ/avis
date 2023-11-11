#ifndef RUNTIME_IO_IO_WRITE_REQUEST_H
#define RUNTIME_IO_IO_WRITE_REQUEST_H

#include "avis/core/common.h"
#include "avis/runtime/io/io_request.h"

namespace io
{
    template<typename response_t = void>
    struct io_write_request : public io_request<response_t>
    {
    public:
        explicit io_write_request(file_descriptor descriptor);

        void process() override;
    };

    template<typename response_t>
    inline io_write_request<response_t>::io_write_request(file_descriptor descriptor) :
        io_request<response_t>(descriptor)
    {}

    template<typename response_t>
    inline void io_write_request<response_t>::process()
    {
        basic_file_descriptor* internal_descriptor = basic_file_descriptor::from_file_descriptor(this->descriptor_);

        try
        {
            // internal_process_write_request(internal_descriptor);
        }
        catch (...)
        {
            std::exception_ptr exception = std::current_exception();
            this->propagate_exception(exception);
        }
    }
} // namespace io

#endif
