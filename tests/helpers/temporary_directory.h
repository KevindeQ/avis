#ifndef AVIS_TESTS_HELPERS_TEMPORARY_DIRECTORY_H
#define AVIS_TESTS_HELPERS_TEMPORARY_DIRECTORY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace avis::tests
{
    // A unique directory under the system temporary directory, removed with its contents when destroyed
    class temporary_directory
    {
    public:
        temporary_directory()
        {
            static std::atomic<std::uint32_t> counter = 0;
            const std::uint64_t seed = std::random_device{}();
            path_ = std::filesystem::temp_directory_path() /
                    std::format("avis-test-{:016x}-{}", seed, counter.fetch_add(1));
            std::filesystem::create_directories(path_);
        }

        ~temporary_directory()
        {
            // Read-only files (permission tests) would stop remove_all
            std::error_code ignored;
            for (const auto& entry : std::filesystem::recursive_directory_iterator{ path_, ignored })
            {
                std::filesystem::permissions(
                    entry.path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ignored);
            }
            std::filesystem::remove_all(path_, ignored);
        }

        temporary_directory(const temporary_directory&) = delete;
        temporary_directory& operator=(const temporary_directory&) = delete;

        const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

        std::filesystem::path file(const std::string_view name) const
        {
            return path_ / name;
        }

    private:
        std::filesystem::path path_;
    };

    // Deterministic test contents: byte i of a file made with a given seed
    inline std::byte pattern_byte(const std::uint64_t index, const std::uint32_t seed = 0) noexcept
    {
        return static_cast<std::byte>((index * 131 + (index >> 8) * 7 + seed) & 0xFF);
    }

    inline std::vector<std::byte> pattern(const std::size_t size, const std::uint32_t seed = 0)
    {
        std::vector<std::byte> bytes(size);
        for (std::size_t index = 0; index < size; ++index)
        {
            bytes[index] = pattern_byte(index, seed);
        }
        return bytes;
    }

    // Writes bytes synchronously with the standard library, independent of the code under test
    inline void write_file(const std::filesystem::path& path, const std::span<const std::byte> bytes)
    {
        std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    inline std::vector<std::byte> read_file(const std::filesystem::path& path)
    {
        std::ifstream stream{ path, std::ios::binary };
        std::vector<char> characters{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
        std::vector<std::byte> bytes(characters.size());
        for (std::size_t index = 0; index < characters.size(); ++index)
        {
            bytes[index] = static_cast<std::byte>(characters[index]);
        }
        return bytes;
    }
} // namespace avis::tests

#endif
