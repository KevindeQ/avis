#ifndef MIDDLEWARE_WINDOW_H
#define MIDDLEWARE_WINDOW_H

#include "avis/core/common.h"
#include "avis/middleware/platform/basic_window.h"
#include "avis/middleware/platform/basic_window_messages.h"

class window : public platform::basic_window, public platform::basic_window_messages
{
public:
    ENGINE_API explicit window(const descriptor& create_params);
    window(const window&) = delete;
    ENGINE_API window(window&&) = default;
    ENGINE_API ~window();

    window& operator=(const window&) = delete;
    ENGINE_API window& operator=(window&&) = default;

    void close();

private:
    ENGINE_API std::int64_t __stdcall message_handler(
        HWND window_handle, unsigned int message, WPARAM wparam, LPARAM lparam) override;
};

#endif
