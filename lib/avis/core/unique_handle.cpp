#include "avis/core/unique_handle.h"

unique_handle::unique_handle(unique_handle&& other) noexcept : handle_{other.handle_}
{
    other.handle_ = INVALID_HANDLE_VALUE;
}

unique_handle::~unique_handle()
{
    release_internal_handle();
}

unique_handle& unique_handle::operator=(unique_handle&& other) noexcept
{
    release_internal_handle();

    handle_ = other.handle_;
    other.handle_ = INVALID_HANDLE_VALUE;

    return *this;
}

unique_handle& unique_handle::operator=(nullptr_t) noexcept
{
    release_internal_handle();
    handle_ = INVALID_HANDLE_VALUE;

    return *this;
}

unique_handle::pointer unique_handle::release() noexcept
{
    pointer current_ptr = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return current_ptr;
}

void unique_handle::reset(pointer new_handle) noexcept
{
    pointer current_handle = handle_;
    handle_ = new_handle;
    release_handle(current_handle);
}

unique_handle::operator bool() const noexcept
{
    return handle_ != INVALID_HANDLE_VALUE;
}

void unique_handle::swap(unique_handle& other) noexcept
{
    std::swap(handle_, other.handle_);
}

bool operator==(const unique_handle& lhs, const unique_handle& rhs)
{
    return lhs.get() == rhs.get();
}

bool operator==(const unique_handle& lhs, nullptr_t)
{
    return lhs.get() == INVALID_HANDLE_VALUE;
}

bool operator==(nullptr_t, const unique_handle& rhs)
{
    return rhs.get() == INVALID_HANDLE_VALUE;
}

bool operator!=(const unique_handle& lhs, const unique_handle& rhs)
{
    return lhs.get() != rhs.get();
}

bool operator!=(const unique_handle& lhs, nullptr_t)
{
    return lhs.get() != INVALID_HANDLE_VALUE;
}

bool operator!=(nullptr_t, const unique_handle& rhs)
{
    return rhs.get() != INVALID_HANDLE_VALUE;
}

void swap(unique_handle& lhs, unique_handle& rhs) noexcept
{
    lhs.swap(rhs);
}
