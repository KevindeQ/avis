#include "avis/io/io_request.h"

#include "avis/common.h"

namespace io
{
    basic_io_request::basic_io_request(file_descriptor descriptor) : descriptor_{descriptor} {}
} // namespace io
