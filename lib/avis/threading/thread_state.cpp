#include "thread_state.h"

namespace avis::threading::detail
{
    std::string_view to_string(const thread_state state) noexcept
    {
        switch (state)
        {
        case thread_state::starting:
            return "starting";
        case thread_state::running:
            return "running";
        case thread_state::stop_requested:
            return "stop_requested";
        case thread_state::exited:
            return "exited";
        }
        return "unknown";
    }
} // namespace avis::threading::detail
