#ifndef AVIS_IO_ASYNC_FILE_H
#define AVIS_IO_ASYNC_FILE_H

#include "avis/enum_flags.h"
#include "avis/error.h"
#include "avis/io/detail/file_sender.h"
#include "avis/io/error.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

// Random-access asynchronous file (docs/design/07-io-layer.md §4.2–4.4). No asio and no <windows.h> here: the asio
// file and its strand live in lib/avis/io/detail/async_file_state.h, the read/write loop in
// lib/avis/io/detail/file_sender.cpp and the sender templates in avis/io/detail/file_sender.h.

namespace avis::io
{
    enum class open_mode : std::uint8_t
    {
        read = 1,
        write = 2,
        create = 4, // create if missing; requires write
        truncate = 8, // requires write
        exclusive = 16 // requires create; fail with error_kind::already_exists if the file exists
    };
} // namespace avis::io

ALLOW_FLAGS_FOR_ENUM(avis::io::open_mode)

namespace avis::io
{
    using open_flags = avis::flags<open_mode>;

    class io_context;

    // set_value(std::size_t bytes) | set_error(avis::error) | set_stopped(). Completes on an IO thread.
    using read_at_sender = detail::file_sender<detail::file_operation_kind::read>;
    using write_at_sender = detail::file_sender<detail::file_operation_kind::write>;

    // Random-access async file handle. Move-only. Operations co-own the internal state, so destroying or moving the
    // file while operations are in flight is safe: they complete with set_stopped. Buffers must still outlive them.
    class async_file
    {
    public:
        async_file() noexcept = default; // closed
        ~async_file(); // close()
        async_file(async_file&& other) noexcept;
        async_file& operator=(async_file&& other) noexcept; // closes the current file first
        async_file(const async_file&) = delete;
        async_file& operator=(const async_file&) = delete;

        // Synchronous: opening a local file is fast and its errors are needed right away. The path-only overload uses
        // avis::runtime::io_context().
        static result<async_file> open(const std::filesystem::path& path, open_flags mode);
        static result<async_file> open(io_context& context, const std::filesystem::path& path, open_flags mode);

        // Loops until buffer is full or end of file. Reading at or past the end completes with 0.
        read_at_sender read_at(std::uint64_t offset, std::span<std::byte> buffer) const;

        // Loops until all of data is written; on success bytes == data.size(). Extends the file when needed.
        write_at_sender write_at(std::uint64_t offset, std::span<const std::byte> data) const;

        result<std::uint64_t> size() const;
        result<void> resize(std::uint64_t size) const;

        // FlushFileBuffers. Blocking: call it from a parallel worker or a caller thread, never from an IO thread.
        result<void> flush() const;

        // Posts a cancel of outstanding operations to the file's strand (they complete with set_stopped) and releases
        // this handle's reference; the OS handle is closed once those operations finished (07 §3.1). Callable from any
        // thread, but not concurrently with other calls on this same object; idempotent.
        void close() noexcept;

        bool is_open() const noexcept;
        const std::filesystem::path& path() const noexcept; // empty when closed; used in error messages

    private:
        // Lets the whole-file operations (lib/avis/io/file_operations.cpp) take over the state of a file they opened,
        // so they can close its handle before they complete
        friend struct detail::async_file_access;

        explicit async_file(std::shared_ptr<detail::async_file_state> state) noexcept;

        // True when mode is set in flags; used by open() to validate and translate the open mode
        static bool has(open_flags flags, open_mode mode) noexcept;

        std::shared_ptr<detail::async_file_state> state_;
    };
} // namespace avis::io

#endif
