#ifndef CORE_UNIQUE_HANDLE_H
#define CORE_UNIQUE_HANDLE_H

#include "avis/core/common.h"

class unique_handle
{
private:
    using pointer = void*;
    using element_type = void;

public:
    ENGINE_API constexpr unique_handle() noexcept;
    ENGINE_API constexpr unique_handle(nullptr_t) noexcept;
    ENGINE_API explicit constexpr unique_handle(pointer handle) noexcept;
    ENGINE_API unique_handle(const unique_handle&) = delete;
    ENGINE_API unique_handle(unique_handle&& other) noexcept;
    ENGINE_API ~unique_handle();

    ENGINE_API unique_handle& operator=(unique_handle&& other) noexcept;
    ENGINE_API unique_handle& operator=(nullptr_t) noexcept;

    ENGINE_API pointer get() const noexcept
    {
        return handle_;
    }

    ENGINE_API pointer release() noexcept;
    ENGINE_API void reset(pointer new_handle = pointer()) noexcept;

    ENGINE_API explicit operator bool() const noexcept;

    friend bool operator==(const unique_handle& lhs, const unique_handle& rhs);
    friend bool operator==(const unique_handle& lhs, nullptr_t);
    friend bool operator==(nullptr_t, const unique_handle& rhs);
    friend bool operator!=(const unique_handle& lhs, const unique_handle& rhs);
    friend bool operator!=(const unique_handle& lhs, nullptr_t);
    friend bool operator!=(nullptr_t, const unique_handle& rhs);

    ENGINE_API void swap(unique_handle& other) noexcept;
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
