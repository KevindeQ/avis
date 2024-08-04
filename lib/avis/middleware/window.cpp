#include "avis/middleware/window.h"

#include "avis/core/common.h"

window::window(const descriptor& create_params) : basic_window(), basic_window_messages()
{
    auto window_class = generate_window_class();
    auto is_registered = register_window_class(window_class);
    if (!is_registered)
    {
        throw std::system_error{std::error_code{static_cast<int>(::GetLastError()), std::generic_category()}};
    }

    auto is_created = create(create_params);
    if (!is_created)
    {
        throw std::system_error{std::error_code{static_cast<int>(::GetLastError()), std::generic_category()}};
    }

    if (create_params.fullscreen)
    {
        toggle_fullscreen(true);
    }

    on_message(
        WM_SIZE,
        [this](WPARAM wparam, LPARAM lparam)
        {
            update_size();
            return 0;
        });

    on_message(
        WM_CLOSE,
        [this](WPARAM wparam, LPARAM lparam)
        {
            close();
            return 0;
        });
}

window::~window()
{
    destroy();
}

void window::close()
{
    ::PostQuitMessage(EXIT_SUCCESS);
}

std::int64_t __stdcall window::message_handler(HWND window_handle, unsigned int message, WPARAM wparam, LPARAM lparam)
{
    auto result = process_message(message, wparam, lparam);
    if (!result)
    {
        return basic_window::message_handler(window_handle, message, wparam, lparam);
    }

    return result.value();
}
