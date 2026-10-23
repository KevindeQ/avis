#ifndef AVIS_THREADING_THREAD_OPTIONS_H
#define AVIS_THREADING_THREAD_OPTIONS_H

#include "avis/error.h"
#include "avis/threading/thread_priority.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>

namespace avis::threading
{
    struct thread_options
    {
        thread_priority priority = thread_priority::normal;
        std::optional<std::uint64_t> affinity_mask; // none = OS decides
        std::move_only_function<void(const std::optional<error>&)> on_exit; // optional, runs on the exiting thread
    };

    // Runs until stop is requested (or until it decides to finish); must return promptly once the token is signalled
    using thread_body = std::move_only_function<void(std::stop_token)>;
} // namespace avis::threading

#endif
