#ifndef RUNTIME_IO_IO_CONTEXT_H
#define RUNTIME_IO_IO_CONTEXT_H

#include "avis/core/common.h"
#include "avis/runtime/io/file_descriptor.h"

namespace io
{
    class io_context
    {
    public:
        io_context() = default;
        io_context(const io_context&) = delete;
        io_context(io_context&&) = default;
        ENGINE_API ~io_context();

        io_context& operator=(const io_context&) = delete;
        io_context& operator=(io_context&&) = default;

        ENGINE_API file_descriptor create_descriptor(const std::filesystem::path& file_path);
        ENGINE_API file_descriptor get_descriptor(const std::filesystem::path& file_path);

        ENGINE_API void release_descriptor(const file_descriptor descriptor);
        ENGINE_API void release_all_descriptors();

    private:
        file_descriptor_manager descriptor_manager_;
    };
} // namespace io

#endif
