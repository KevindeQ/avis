#include "avis/io/async_file.h"

#include "detail/async_file_state.h"

#include "avis/io/io_context.h"
#include "avis/log.h"
#include "avis/runtime.h"

#include <asio/post.hpp>
#include <asio/random_access_file.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <cassert>
#include <exception>
#include <system_error>
#include <utility>
#include <windows.h>

namespace avis::io
{
    async_file::async_file(std::shared_ptr<detail::async_file_state> state) noexcept : state_{ std::move(state) } {}

    async_file::~async_file()
    {
        close();
    }

    async_file::async_file(async_file&& other) noexcept : state_{ std::move(other.state_) } {}

    async_file& async_file::operator=(async_file&& other) noexcept
    {
        if (this != &other)
        {
            close();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    result<async_file> async_file::open(const std::filesystem::path& path, const open_flags mode)
    {
        return open(runtime::io_context(), path, mode);
    }

    result<async_file> async_file::open(io_context& context, const std::filesystem::path& path, const open_flags mode)
    {
        const bool reading = has(mode, open_mode::read);
        const bool writing = has(mode, open_mode::write);
        const bool creating = has(mode, open_mode::create);
        const bool truncating = has(mode, open_mode::truncate);
        const bool exclusive = has(mode, open_mode::exclusive);

        if (!reading && !writing)
        {
            return std::unexpected(make_error(error_kind::invalid_argument, "open: needs open_mode::read or write"));
        }
        if ((creating || truncating) && !writing)
        {
            return std::unexpected(
                make_error(error_kind::invalid_argument, "open: create and truncate need open_mode::write"));
        }
        if (exclusive && !creating)
        {
            return std::unexpected(make_error(error_kind::invalid_argument, "open: exclusive needs open_mode::create"));
        }

        DWORD access = 0;
        if (reading)
        {
            access |= GENERIC_READ;
        }
        if (writing)
        {
            access |= GENERIC_WRITE;
        }

        // Read-only opens share delete, so an atomic replace can rename over a file that is being read (07 §4.2)
        const DWORD share = writing ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_DELETE;

        DWORD disposition = OPEN_EXISTING;
        if (exclusive)
        {
            disposition = CREATE_NEW;
        }
        else if (creating && truncating)
        {
            disposition = CREATE_ALWAYS;
        }
        else if (creating)
        {
            disposition = OPEN_ALWAYS;
        }
        else if (truncating)
        {
            disposition = TRUNCATE_EXISTING;
        }

        // Not asio's own open: it calls CreateFileA (no non-ASCII paths) without FILE_SHARE_DELETE
        const HANDLE handle = ::CreateFileW(
            path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(make_error(
                std::error_code{ static_cast<int>(::GetLastError()), std::system_category() }, "open", path));
        }

        bool assigned = false;
        try
        {
            asio::random_access_file file{ context.native() };
            std::error_code code;
            file.assign(handle, code);
            if (code)
            {
                ::CloseHandle(handle);
                return std::unexpected(make_error(code, "open", path));
            }
            assigned = true; // the asio file owns the handle from here on

            return async_file{ std::make_shared<detail::async_file_state>(
                context, std::move(file), path, detail::find_operation_registry(context.native())) };
        }
        catch (...)
        {
            if (!assigned)
            {
                ::CloseHandle(handle);
            }
            return std::unexpected(avis::make_error(std::current_exception()));
        }
    }

    read_at_sender async_file::read_at(const std::uint64_t offset, const std::span<std::byte> buffer) const
    {
        return read_at_sender{ state_, offset, buffer.data(), buffer.size() };
    }

    write_at_sender async_file::write_at(const std::uint64_t offset, const std::span<const std::byte> data) const
    {
        // The write path only reads through the pointer
        return write_at_sender{ state_, offset, const_cast<std::byte*>(data.data()), data.size() };
    }

    result<std::uint64_t> async_file::size() const
    {
        if (!state_)
        {
            return std::unexpected(make_error(error_kind::not_open, "size of a closed file"));
        }
        std::error_code code;
        const std::uint64_t size = state_->file.size(code);
        if (code)
        {
            return std::unexpected(make_error(code, "size", state_->path));
        }
        return size;
    }

    result<void> async_file::resize(const std::uint64_t size) const
    {
        if (!state_)
        {
            return std::unexpected(make_error(error_kind::not_open, "resize of a closed file"));
        }
        std::error_code code;
        state_->file.resize(size, code);
        if (code)
        {
            return std::unexpected(make_error(code, "resize", state_->path));
        }
        return {};
    }

    result<void> async_file::flush() const
    {
        assert(io_context::current() == nullptr && "async_file::flush blocks; don't call it on an IO thread");
        if (!state_)
        {
            return std::unexpected(make_error(error_kind::not_open, "flush of a closed file"));
        }
        std::error_code code;
        state_->file.sync_all(code);
        if (code)
        {
            return std::unexpected(make_error(code, "flush", state_->path));
        }
        return {};
    }

    void async_file::close() noexcept
    {
        if (!state_)
        {
            return;
        }

        std::shared_ptr<detail::async_file_state> state = std::move(state_);
        try
        {
            asio::post(
                state->strand,
                [state]()
                {
                    state->closed = true;
                    std::error_code ignored;
                    state->file.cancel(ignored); // outstanding operations complete with operation_aborted
                });
        }
        catch (...)
        {
            try
            {
                log::get("io")->error("async_file: couldn't post the close of '{}'", state->path.string());
            }
            catch (...)
            {
                // Logging is best effort
            }
        }
    }

    bool async_file::is_open() const noexcept
    {
        return state_ != nullptr;
    }

    const std::filesystem::path& async_file::path() const noexcept
    {
        static const std::filesystem::path empty;
        return state_ ? state_->path : empty;
    }

    bool async_file::has(const open_flags flags, const open_mode mode) noexcept
    {
        return static_cast<bool>(flags & mode);
    }
} // namespace avis::io
