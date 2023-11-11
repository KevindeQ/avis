#ifndef MIDDLEWARE_PLATFORM_BASIC_WINDOW_MESSAGES_H
#define MIDDLEWARE_PLATFORM_BASIC_WINDOW_MESSAGES_H

#include "avis/core/common.h"

namespace platform
{
    class basic_window_messages
    {
    private:
        using message_id = std::uint32_t;
        using message_handler = std::function<std::int64_t(WPARAM, LPARAM)>;

    public:
        virtual ~basic_window_messages() = default;

        ENGINE_API void on_message(message_id id, message_handler&& handler);

        std::optional<std::int64_t> process_message(message_id id, WPARAM wparam, LPARAM lparam);

    private:
        std::map<message_id, message_handler> message_handlers_;
    };
} // namespace platform

#endif
