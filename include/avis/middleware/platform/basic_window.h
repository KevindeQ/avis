#ifndef MIDDLEWARE_PLATFORM_BASIC_WINDOW_H
#define MIDDLEWARE_PLATFORM_BASIC_WINDOW_H

#include "avis/core/common.h"

namespace platform
{
    class basic_window
    {
    public:
        struct descriptor
        {
        public:
            std::wstring title = L"Application";

            std::uint32_t width = 800;
            std::uint32_t height = 800;
            bool fullscreen = false;

            bool resizeable = true;
        };

    public:
        basic_window();
        virtual ~basic_window() = default;

        void resize(std::uint32_t width, std::uint32_t height);

        void toggle_fullscreen(bool fullscreen);

        std::uint32_t width() const;
        std::uint32_t height() const;

        HWND native_handle() const;

    protected:
        bool create(const descriptor& create_params);
        void destroy();

        void update_size();

        WNDCLASSEX generate_window_class();
        bool register_window_class(WNDCLASSEX& window_class);

        virtual std::int64_t __stdcall message_handler(
            HWND window_handle, unsigned int message, WPARAM wparam, LPARAM lparam);

    private:
        static std::int64_t __stdcall message_router(
            HWND window_handle, unsigned int message, WPARAM wparam, LPARAM lparam);

    private:
        HWND native_handle_;

        // The following variables track window information when toggling between normal and
        // fullscreen windows.
        bool fullscreen_;
        std::uint32_t style_;
        std::uint32_t extended_style_;
        RECT window_rect_;
    };
} // namespace platform

#endif
