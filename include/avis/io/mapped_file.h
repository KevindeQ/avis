#ifndef AVIS_IO_MAPPED_FILE_H
#define AVIS_IO_MAPPED_FILE_H

#include "avis/error.h"
#include "avis/io/error.h"

#include <cstddef>
#include <filesystem>
#include <span>

// Read-only memory mapping of a whole file, for small indices and headers (docs/design/07-io-layer.md §6). Synchronous;
// large payloads go through async_file::read_at instead, so page faults don't stall the thread that reads them. No
// <windows.h> here: handles are kept as void*.

namespace avis::io
{
    // Read-only memory mapping of a whole file. Move-only; unmaps and closes on destruction.
    class mapped_file
    {
    public:
        mapped_file() noexcept = default; // closed
        ~mapped_file();
        mapped_file(mapped_file&& other) noexcept;
        mapped_file& operator=(mapped_file&& other) noexcept; // closes the current mapping first
        mapped_file(const mapped_file&) = delete;
        mapped_file& operator=(const mapped_file&) = delete;

        // Opened with FILE_SHARE_READ | FILE_SHARE_DELETE, so an atomic_replace of the path succeeds while it's mapped;
        // the mapping keeps the old contents. An empty file opens with an empty span and no mapping.
        static result<mapped_file> open(const std::filesystem::path& path);

        std::span<const std::byte> bytes() const noexcept;
        std::size_t size() const noexcept;
        bool is_open() const noexcept;
        const std::filesystem::path& path() const noexcept; // empty when closed

    private:
        void close() noexcept;

        void* file_handle_ = nullptr; // HANDLE, kept as void* so <windows.h> stays out of the header
        void* mapping_handle_ = nullptr; // HANDLE; nullptr for an empty file
        const std::byte* view_ = nullptr;
        std::size_t size_ = 0;
        std::filesystem::path path_;
    };
} // namespace avis::io

#endif
