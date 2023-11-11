#ifndef CORE_EXCEPTIONS_H
#define CORE_EXCEPTIONS_H

namespace detail
{
    class win32_error_category : public std::error_category
    {
    public:
        char const* name() const noexcept override final
        {
            return "Win32 Error";
        }

        std::string message(int c) const override final
        {
            char error_message[256];
            auto len = FormatMessageA(
                FORMAT_MESSAGE_FROM_SYSTEM, NULL, static_cast<DWORD>(c), 0, error_message, sizeof error_message, NULL);
            if (len == 0)
            {
                return "N/A";
            }

            // Trim trailing newline
            while (len && (error_message[len - 1] == '\r' || error_message[len - 1] == '\n'))
            {
                --len;
            }

            return std::string{error_message, len};
        }
    };
} // namespace detail

inline detail::win32_error_category const& win32_error_category()
{
    static detail::win32_error_category c;
    return c;
}

struct win32_error_code
{
    explicit win32_error_code(DWORD e) noexcept : error_code{e} {}

    DWORD error_code;
};

// Overload the function make_error_code in std with our custom error. It will be found via ADL by the compiler if
// needed.
inline std::error_code make_error_code(win32_error_code const& error)
{
    return std::error_code(static_cast<int>(error.error_code), win32_error_category());
}

inline std::error_code make_win32_error_code(DWORD error_number)
{
    return make_error_code(win32_error_code{error_number});
}

inline std::error_code make_win32_error_code()
{
    return make_win32_error_code(GetLastError());
}

namespace std
{
    // Register win32_error_code with the with the standard error code system in the C++ STL
    template<>
    struct is_error_code_enum<win32_error_code> : std::true_type
    {};
} // namespace std

#endif
