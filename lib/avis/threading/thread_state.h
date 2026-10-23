#ifndef AVIS_THREADING_THREAD_STATE_H
#define AVIS_THREADING_THREAD_STATE_H

#include <cstdint>
#include <string_view>

namespace avis::threading::detail
{
    // Life cycle of a pool thread. request_stop() moves starting/running to stop_requested, which the join
    // warning reports. Private to the library.
    enum class thread_state : std::uint8_t
    {
        starting,
        running,
        stop_requested,
        exited
    };

    std::string_view to_string(thread_state state) noexcept;
} // namespace avis::threading::detail

#endif
