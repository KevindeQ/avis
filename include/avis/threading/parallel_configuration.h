#ifndef AVIS_THREADING_PARALLEL_CONFIGURATION_H
#define AVIS_THREADING_PARALLEL_CONFIGURATION_H

#include <cstddef>

namespace avis::threading
{
    // Size of the parallel pool behind stdexec::get_parallel_scheduler(). Free of stdexec, so the runtime configuration
    // can include it.
    struct parallel_configuration
    {
        std::size_t workers = 0; // 0 = max(1, hardware_concurrency - 2), set by the runtime
    };
} // namespace avis::threading

#endif
