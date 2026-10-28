#ifndef AVIS_RHI_D3D12_UNIQUE_HANDLE_H
#define AVIS_RHI_D3D12_UNIQUE_HANDLE_H

// Move-only owner of a Win32 HANDLE (docs/design/09-rhi.md §20.2.1)

#include "avis/rhi/d3d12/d3d12.h"

#include <utility>

namespace avis::rhi::d3d12
{
    // Calls CloseHandle when destroyed or reset (events, shared texture and fence handles)
    class unique_handle
    {
    public:
        unique_handle() noexcept = default;
        explicit unique_handle(const HANDLE handle) noexcept : handle_{ handle } {}

        unique_handle(unique_handle&& other) noexcept : handle_{ std::exchange(other.handle_, nullptr) } {}

        unique_handle& operator=(unique_handle&& other) noexcept
        {
            if (this != &other)
            {
                reset(std::exchange(other.handle_, nullptr));
            }
            return *this;
        }

        unique_handle(const unique_handle&) = delete;
        unique_handle& operator=(const unique_handle&) = delete;

        ~unique_handle()
        {
            reset();
        }

        HANDLE get() const noexcept
        {
            return handle_;
        }

        explicit operator bool() const noexcept
        {
            return is_valid();
        }

        // Closes the held handle, if any, and takes ownership of `handle`
        void reset(const HANDLE handle = nullptr) noexcept
        {
            if (is_valid())
            {
                CloseHandle(handle_);
            }
            handle_ = handle;
        }

        // Gives up ownership without closing
        HANDLE release() noexcept
        {
            return std::exchange(handle_, nullptr);
        }

    private:
        // Win32 APIs return either nullptr or INVALID_HANDLE_VALUE on failure, depending on the function
        bool is_valid() const noexcept
        {
            return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
        }

        HANDLE handle_ = nullptr;
    };
} // namespace avis::rhi::d3d12

#endif
