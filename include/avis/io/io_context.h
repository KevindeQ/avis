#ifndef IO_IO_CONTEXT_H
#define IO_IO_CONTEXT_H

#include "avis/common.h"
#include "avis/io/file_descriptor.h"

namespace io
{
    class io_context
    {
    public:
        io_context() = default;
        io_context(const io_context&) = delete;
        io_context(io_context&&) = default;
        ~io_context();

        io_context& operator=(const io_context&) = delete;
        io_context& operator=(io_context&&) = default;

        file_descriptor create_descriptor(const std::filesystem::path& file_path);
        file_descriptor get_descriptor(const std::filesystem::path& file_path);

        void release_descriptor(const file_descriptor descriptor);
        void release_all_descriptors();

    private:
        file_descriptor_manager descriptor_manager_;
    };
} // namespace io

#endif
