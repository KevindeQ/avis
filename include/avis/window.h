#ifndef WINDOW_H
#define WINDOW_H

#include "avis/common.h"
#include "avis/platform/basic_window.h"
#include "avis/platform/basic_window_messages.h"

class window : public platform::basic_window, public platform::basic_window_messages
{
public:
    explicit window(const descriptor& create_params);
    window(const window&) = delete;
    window(window&&) = default;
    ~window();

    window& operator=(const window&) = delete;
    window& operator=(window&&) = default;

    void close();

private:
    std::int64_t __stdcall message_handler(
        HWND window_handle, unsigned int message, WPARAM wparam, LPARAM lparam) override;
};

#endif
