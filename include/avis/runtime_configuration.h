#ifndef AVIS_RUNTIME_CONFIGURATION_H
#define AVIS_RUNTIME_CONFIGURATION_H

#include "avis/error.h"
#include "avis/io/io_configuration.h"
#include "avis/log.h"
#include "avis/threading/parallel_configuration.h"
#include "avis/threading/thread_pool.h"

#include <cstddef>
#include <string_view>

namespace avis
{
    // Configuration of avis::runtime
    struct runtime_configuration
    {
    public:
        // Reads the `runtime:` and `logging:` sections of an application's YAML configuration and ignores the rest.
        // Missing sections and keys keep their defaults. Errors are error_domain::parse, with "line:column" and the key
        // path (for example runtime.parallel_workers) in the message.
        static result<runtime_configuration> parse(std::string_view yaml_text);

        // Fills in the computed values and rejects inconsistent ones with
        // generic_error_kind::invalid_configuration: parallel.workers 0 becomes max(1, hardware_concurrency - 2),
        // thread_pool.capacity 0 becomes io.threads + application_threads + 2. avis::runtime resolves the
        // configuration it's given; runtime::configuration() returns the result.
        static result<runtime_configuration> resolve(runtime_configuration configuration);

        logging_configuration logging;
        threading::thread_pool_configuration thread_pool{ .capacity = 0 }; // 0 = computed
        io::io_configuration io; // threads of the default IO context
        threading::parallel_configuration parallel; // workers of the parallel pool
        std::size_t application_threads = 0; // long-running threads the application will start itself

    private:
        static std::size_t resolve_worker_count(std::size_t workers) noexcept;
    };
} // namespace avis

#endif
