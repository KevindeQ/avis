#ifndef AVIS_IO_IO_CONFIGURATION_H
#define AVIS_IO_IO_CONFIGURATION_H

#include <cstddef>

namespace avis::io
{
    // Size of the default IO context. Free of asio, so the runtime configuration can include it.
    struct io_configuration
    {
        std::size_t threads = 1; // long-running io_context bodies on the default pool
    };
} // namespace avis::io

#endif
