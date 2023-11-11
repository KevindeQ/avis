#include "avis/runtime/io/io_request.h"

#include "avis/core/common.h"

namespace io
{
    basic_io_request::basic_io_request(file_descriptor descriptor) : descriptor_{descriptor} {}
} // namespace io
