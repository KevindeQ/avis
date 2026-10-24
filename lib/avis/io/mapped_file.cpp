#include "avis/io/mapped_file.h"

#include "avis/io/error.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <windows.h>

namespace avis::io
{
    namespace
    {
        std::error_code last_error() noexcept
        {
            return std::error_code{ static_cast<int>(::GetLastError()), std::system_category() };
        }

        // A failed CreateFileMappingW or MapViewOfFile: error_kind::mapping_failed, with the usual message (operation,
        // path, native code) from the classifying make_error
        avis::error mapping_error(
            const std::error_code& code, const std::string_view operation, const std::filesystem::path& path)
        {
            avis::error error = make_error(code, operation, path);
            error.code = static_cast<std::int32_t>(error_kind::mapping_failed);
            return error;
        }
    } // namespace

    mapped_file::~mapped_file()
    {
        close();
    }

    mapped_file::mapped_file(mapped_file&& other) noexcept :
        file_handle_{ std::exchange(other.file_handle_, nullptr) },
        mapping_handle_{ std::exchange(other.mapping_handle_, nullptr) },
        view_{ std::exchange(other.view_, nullptr) },
        size_{ std::exchange(other.size_, 0) },
        path_{ std::move(other.path_) }
    {
        other.path_.clear();
    }

    mapped_file& mapped_file::operator=(mapped_file&& other) noexcept
    {
        if (this != &other)
        {
            close();
            file_handle_ = std::exchange(other.file_handle_, nullptr);
            mapping_handle_ = std::exchange(other.mapping_handle_, nullptr);
            view_ = std::exchange(other.view_, nullptr);
            size_ = std::exchange(other.size_, 0);
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    result<mapped_file> mapped_file::open(const std::filesystem::path& path)
    {
        // Filled step by step; on a failure its destructor releases what was created so far
        mapped_file mapped;
        mapped.path_ = path;

        const HANDLE file = ::CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, // lets an atomic_replace rename over the path (07 §6)
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(make_error(last_error(), "open", path));
        }
        mapped.file_handle_ = file;

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size))
        {
            return std::unexpected(make_error(last_error(), "size", path));
        }
        const auto file_size = static_cast<std::uint64_t>(size.QuadPart);
        if (file_size > std::numeric_limits<std::size_t>::max())
        {
            return std::unexpected(
                make_error(error_kind::file_too_large, "mapped_file: file larger than the address space"));
        }
        if (file_size == 0)
        {
            return mapped; // CreateFileMappingW rejects empty files: keep the handle, no mapping
        }

        const HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr)
        {
            return std::unexpected(mapping_error(last_error(), "map", path));
        }
        mapped.mapping_handle_ = mapping;

        const void* const view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr)
        {
            return std::unexpected(mapping_error(last_error(), "map view", path));
        }
        mapped.view_ = static_cast<const std::byte*>(view);
        mapped.size_ = static_cast<std::size_t>(file_size);
        return mapped;
    }

    std::span<const std::byte> mapped_file::bytes() const noexcept
    {
        return { view_, size_ };
    }

    std::size_t mapped_file::size() const noexcept
    {
        return size_;
    }

    bool mapped_file::is_open() const noexcept
    {
        return file_handle_ != nullptr;
    }

    const std::filesystem::path& mapped_file::path() const noexcept
    {
        return path_;
    }

    void mapped_file::close() noexcept
    {
        if (view_ != nullptr)
        {
            ::UnmapViewOfFile(view_);
        }
        if (mapping_handle_ != nullptr)
        {
            ::CloseHandle(mapping_handle_);
        }
        if (file_handle_ != nullptr)
        {
            ::CloseHandle(file_handle_);
        }
        file_handle_ = nullptr;
        mapping_handle_ = nullptr;
        view_ = nullptr;
        size_ = 0;
        path_.clear();
    }
} // namespace avis::io
