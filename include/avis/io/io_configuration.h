#ifndef AVIS_IO_IO_CONFIGURATION_H
#define AVIS_IO_IO_CONFIGURATION_H

#include <chrono>
#include <cstddef>

namespace avis::io
{
    // Size and shutdown behaviour of an IO context. Free of asio, so the runtime configuration can include it.
    struct io_configuration
    {
        std::size_t threads = 1; // long-running io_context bodies on the default pool

        // How long request_stop() waits for cancelled operations to complete before it stops the context anyway;
        // 0 cancels and stops straight away (docs/design/07-io-layer.md §3.3, D-71)
        std::chrono::milliseconds shutdown_timeout{ 1000 };
    };
} // namespace avis::io

#endif
