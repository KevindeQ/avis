#include "avis/runtime/io/io_context.h"

#include "avis/core/common.h"
#include "avis/runtime/io/file_descriptor.h"

namespace io
{
    io_context::~io_context() {}

    file_descriptor io_context::create_descriptor(const std::filesystem::path& file_path)
    {
        return descriptor_manager_.create_descriptor(file_path);
    }

    file_descriptor io_context::get_descriptor(const std::filesystem::path& file_path)
    {
        return descriptor_manager_.find_descriptor(file_path);
    }

    void io_context::release_descriptor(const file_descriptor descriptor)
    {
        descriptor_manager_.release_descriptor(descriptor);
    }

    void io_context::release_all_descriptors()
    {
        descriptor_manager_.release_all_descriptors();
    }
} // namespace io
