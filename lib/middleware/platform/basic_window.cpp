#include "avis/middleware/platform/basic_window.h"

#include "avis/core/common.h"

namespace platform
{
    basic_window::basic_window() : native_handle_{}, fullscreen_{false}, style_{}, extended_style_{}, window_rect_{} {}

    bool basic_window::create(const descriptor& create_params)
    {
        // Adjust window size based on selected styles
        style_ = WS_OVERLAPPEDWINDOW;
        extended_style_ = WS_EX_OVERLAPPEDWINDOW;
        if (!create_params.resizeable)
        {
            style_ &= ~(WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        }

        RECT dimensions = {0, 0, static_cast<LONG>(create_params.width), static_cast<LONG>(create_params.height)};
        ::AdjustWindowRectEx(&dimensions, style_, FALSE, extended_style_);

        // Create the actual window
        native_handle_ = ::CreateWindowExW(
            extended_style_,
            L"MainWindow",
            create_params.title.c_str(),
            style_,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            dimensions.right - dimensions.left,
            dimensions.bottom - dimensions.top,
            nullptr,
            nullptr,
            ::GetModuleHandleW(nullptr),
            static_cast<void*>(this));
        if (native_handle_ == nullptr)
        {
            return false;
        }

        // Show the window to the user
        ::ShowWindow(native_handle_, SW_SHOW);
        ::UpdateWindow(native_handle_);

        // Store window dimensions
        window_rect_ = dimensions;

        return true;
    }

    void basic_window::destroy()
    {
        ::DestroyWindow(native_handle_);
        ::UnregisterClassW(L"MainWindow", ::GetModuleHandleW(nullptr));
    }

    void basic_window::resize(std::uint32_t width, std::uint32_t height)
    {
        RECT dimensions = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        ::AdjustWindowRectEx(&dimensions, style_, FALSE, extended_style_);

        ::SetWindowPos(
            native_handle_,
            nullptr,
            0,
            0,
            dimensions.right - dimensions.left,
            dimensions.bottom - dimensions.top,
            SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOZORDER);

        window_rect_ = dimensions;
    }

    void basic_window::toggle_fullscreen(bool fullscreen)
    {
        if (!fullscreen)
        {
            style_ = ::GetWindowLongW(native_handle_, GWL_STYLE);
            extended_style_ = ::GetWindowLongW(native_handle_, GWL_EXSTYLE);

            ::GetWindowRect(native_handle_, &window_rect_);
        }

        fullscreen_ = fullscreen;

        if (fullscreen)
        {
            SetWindowLongW(native_handle_, GWL_STYLE, style_ & ~(WS_CAPTION | WS_THICKFRAME));
            SetWindowLongW(
                native_handle_,
                GWL_EXSTYLE,
                extended_style_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

            MONITORINFO monitor_info = {};
            monitor_info.cbSize = sizeof(monitor_info);
            ::GetMonitorInfoW(::MonitorFromWindow(native_handle_, MONITOR_DEFAULTTONEAREST), &monitor_info);
            SetWindowPos(
                native_handle_,
                nullptr,
                monitor_info.rcMonitor.left,
                monitor_info.rcMonitor.top,
                monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
                monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        else
        {
            SetWindowLongW(native_handle_, GWL_STYLE, style_);
            SetWindowLongW(native_handle_, GWL_EXSTYLE, extended_style_);

            SetWindowPos(
                native_handle_,
                nullptr,
                window_rect_.left,
                window_rect_.top,
                window_rect_.right - window_rect_.left,
                window_rect_.bottom - window_rect_.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    std::uint32_t basic_window::width() const
    {
        return window_rect_.right - window_rect_.left;
    }

    std::uint32_t basic_window::height() const
    {
        return window_rect_.bottom - window_rect_.top;
    }

    HWND basic_window::native_handle() const
    {
        return native_handle_;
    }

    void basic_window::update_size()
    {
        ::GetWindowRect(native_handle_, &window_rect_);
    }

    WNDCLASSEX basic_window::generate_window_class()
    {
        // Register window class
        WNDCLASSEX window_class = {};
        window_class.cbSize = sizeof(WNDCLASSEX);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &basic_window::message_router;
        window_class.cbClsExtra = 0;
        window_class.cbWndExtra = sizeof(basic_window*);
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
        window_class.lpszMenuName = nullptr;
        window_class.lpszClassName = L"MainWindow";

        HANDLE app_cursor = ::LoadImageW(
            nullptr,
            static_cast<LPCWSTR>(MAKEINTRESOURCEW(IDC_ARROW)),
            IMAGE_CURSOR,
            ::GetSystemMetrics(SM_CXCURSOR),
            ::GetSystemMetrics(SM_CYCURSOR),
            LR_DEFAULTCOLOR | LR_SHARED);
        window_class.hCursor = static_cast<HCURSOR>(app_cursor);

        HANDLE app_icon = ::LoadImageW(
            nullptr,
            static_cast<LPCWSTR>(MAKEINTRESOURCEW(IDI_APPLICATION)),
            IMAGE_ICON,
            ::GetSystemMetrics(SM_CXICON),
            ::GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR | LR_SHARED);
        window_class.hIcon = static_cast<HICON>(app_icon);

        HANDLE app_icon_small = ::LoadImageW(
            nullptr,
            static_cast<LPCWSTR>(MAKEINTRESOURCEW(IDI_APPLICATION)),
            IMAGE_ICON,
            ::GetSystemMetrics(SM_CXSMICON),
            ::GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED);
        window_class.hIconSm = static_cast<HICON>(app_icon_small);

        return window_class;
    }

    bool basic_window::register_window_class(WNDCLASSEX& window_class)
    {
        if (::RegisterClassExW(&window_class) == 0)
        {
            return false;
        }

        return true;
    }

    LRESULT __stdcall basic_window::message_handler(
        HWND window_handle, unsigned int message, WPARAM wparam, LPARAM lparam)
    {
        return ::DefWindowProcW(window_handle, message, wparam, lparam);
    }

    std::int64_t __stdcall basic_window::message_router(
        HWND window_handle, unsigned int message, WPARAM wparam, LPARAM lparam)
    {
        basic_window* current_window = nullptr;
        if (message == WM_NCCREATE)
        {
            current_window = reinterpret_cast<basic_window*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
            ::SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<std::int64_t>(current_window));
        }

        current_window = reinterpret_cast<basic_window*>(::GetWindowLongPtrW(window_handle, GWLP_USERDATA));
        if (current_window != nullptr)
        {
            return current_window->message_handler(window_handle, message, wparam, lparam);
        }
        {
            return ::DefWindowProcW(window_handle, message, wparam, lparam);
        }
    }
} // namespace platform
