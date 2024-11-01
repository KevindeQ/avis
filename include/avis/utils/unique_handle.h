#ifndef UTILS_UNIQUE_HANDLE_H
#define UTILS_UNIQUE_HANDLE_H

#include "avis/common.h"

class unique_handle
{
private:
    using pointer = void*;
    using element_type = void;

public:
    constexpr unique_handle() noexcept;
    constexpr unique_handle(nullptr_t) noexcept;
    explicit constexpr unique_handle(pointer handle) noexcept;
    unique_handle(const unique_handle&) = delete;
    unique_handle(unique_handle&& other) noexcept;
    ~unique_handle();

    unique_handle& operator=(unique_handle&& other) noexcept;
    unique_handle& operator=(nullptr_t) noexcept;

    pointer get() const noexcept
    {
        return handle_;
    }

    pointer release() noexcept;
    void reset(pointer new_handle = pointer()) noexcept;

    explicit operator bool() const noexcept;

    friend bool operator==(const unique_handle& lhs, const unique_handle& rhs);
    friend bool operator==(const unique_handle& lhs, nullptr_t);
    friend bool operator==(nullptr_t, const unique_handle& rhs);
    friend bool operator!=(const unique_handle& lhs, const unique_handle& rhs);
    friend bool operator!=(const unique_handle& lhs, nullptr_t);
    friend bool operator!=(nullptr_t, const unique_handle& rhs);

    void swap(unique_handle& other) noexcept;
    friend void swap(unique_handle& lhs, unique_handle& rhs) noexcept;

private:
    inline void release_internal_handle() noexcept;
    inline void release_handle(pointer handle) noexcept;

    pointer handle_;
};

constexpr unique_handle::unique_handle() noexcept : handle_{INVALID_HANDLE_VALUE} {}

constexpr unique_handle::unique_handle(nullptr_t) noexcept : handle_{INVALID_HANDLE_VALUE} {}

constexpr unique_handle::unique_handle(pointer handle) noexcept : handle_{handle} {}

void unique_handle::release_internal_handle() noexcept
{
    release_handle(handle_);
}

void unique_handle::release_handle(pointer handle) noexcept
{
    if (handle != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(handle);
    }
}

#endif
