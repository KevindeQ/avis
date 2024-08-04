#include "avis/middleware/platform/basic_window_messages.h"

#include "avis/core/common.h"

namespace platform
{
    void basic_window_messages::on_message(message_id id, message_handler&& handler)
    {
        message_handlers_.insert_or_assign(id, std::forward<message_handler>(handler));
    }

    std::optional<std::int64_t> basic_window_messages::process_message(message_id id, WPARAM wparam, LPARAM lparam)
    {
        auto message_handler_loc = message_handlers_.find(id);
        if (message_handler_loc == message_handlers_.end())
        {
            return std::nullopt;
        }

        auto handler = message_handler_loc->second;
        return handler(wparam, lparam);
    }
} // namespace platform
