#include "avis/graphics/shader_library.h"

#include "shader_library_internal.h"

#include "avis/graphics/error.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace avis::graphics
{
    namespace
    {
        std::string_view stage_name(const shader_stage stage) noexcept
        {
            switch (stage)
            {
            case shader_stage::vertex:
                return "vertex";
            case shader_stage::pixel:
                return "pixel";
            case shader_stage::compute:
                return "compute";
            }
            return "unknown";
        }

        // "shader library 'name': " for messages, or nothing for an unnamed library
        std::string prefix(const std::string& name)
        {
            return name.empty() ? std::string{} : std::format("shader library '{}': ", name);
        }

        std::uint64_t next_serial() noexcept
        {
            static std::atomic<std::uint64_t> counter{ 1 };
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t offset) noexcept
        {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes.data() + offset, sizeof(value)); // DXIL containers are little-endian, like x64
            return value;
        }
    } // namespace

    namespace detail
    {
        std::optional<shader_stage> dxil_shader_stage(const std::span<const std::byte> bytecode) noexcept
        {
            // Container header: "DXBC", 16-byte digest, version (2 + 2), size (4), part count (4), then one offset per
            // part. Each part: four-character code (4), size (4), data.
            constexpr std::size_t header_size = 32;
            if (bytecode.size() < header_size || std::memcmp(bytecode.data(), "DXBC", 4) != 0)
            {
                return std::nullopt;
            }
            const std::uint32_t part_count = read_u32(bytecode, 28);
            if (part_count > (bytecode.size() - header_size) / 4)
            {
                return std::nullopt;
            }
            for (std::uint32_t part = 0; part < part_count; ++part)
            {
                const std::size_t offset = read_u32(bytecode, header_size + std::size_t{ part } * 4);
                if (offset > bytecode.size() || bytecode.size() - offset < 12)
                {
                    return std::nullopt;
                }
                if (std::memcmp(bytecode.data() + offset, "DXIL", 4) != 0)
                {
                    continue;
                }
                // The DXIL program header starts with the program version: the shader kind is in the upper 16 bits
                switch (read_u32(bytecode, offset + 8) >> 16)
                {
                case 0:
                    return shader_stage::pixel;
                case 1:
                    return shader_stage::vertex;
                case 5:
                    return shader_stage::compute;
                default:
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        result<shader_library> create_shader_library(
            std::vector<shader_blob> blobs, std::string name, const std::span<const std::uint64_t> hashes)
        {
            if (hashes.size() != blobs.size())
            {
                return std::unexpected(make_error(
                    error_kind::invalid_argument,
                    std::format("{}{} hashes for {} shader blobs", prefix(name), hashes.size(), blobs.size())));
            }
            if (blobs.size() > std::numeric_limits<std::uint32_t>::max())
            {
                return std::unexpected(make_error(
                    error_kind::invalid_argument,
                    std::format("{}too many shader blobs ({})", prefix(name), blobs.size())));
            }

            for (std::size_t index = 0; index < blobs.size(); ++index)
            {
                const shader_blob& blob = blobs[index];
                if (blob.id.empty())
                {
                    return std::unexpected(make_error(
                        error_kind::invalid_argument,
                        std::format("{}shader blob {} has an empty id", prefix(name), index)));
                }
                if (blob.bytecode.empty())
                {
                    return std::unexpected(make_error(
                        error_kind::invalid_argument,
                        std::format("{}shader '{}' has empty bytecode", prefix(name), blob.id)));
                }
#ifndef NDEBUG
                // Catches a mislabelled blob early; bytes that aren't a DXIL container aren't checked
                if (const auto kind = dxil_shader_stage(blob.bytecode); kind && *kind != blob.stage)
                {
                    return std::unexpected(make_error(
                        error_kind::invalid_argument,
                        std::format(
                            "{}shader '{}' is declared as a {} shader, but its DXIL is a {} shader",
                            prefix(name),
                            blob.id,
                            stage_name(blob.stage),
                            stage_name(*kind))));
                }
#endif
            }

            std::vector<shader_library::index_entry> index;
            index.reserve(blobs.size());
            for (std::size_t blob = 0; blob < blobs.size(); ++blob)
            {
                index.push_back({ .hash = hashes[blob], .blob = static_cast<std::uint32_t>(blob) });
            }
            // Stable, so of two equal hashes the earlier blob comes first and is named first
            std::ranges::stable_sort(index, {}, &shader_library::index_entry::hash);

            const auto duplicate = std::ranges::adjacent_find(
                index, [](const auto& left, const auto& right) { return left.hash == right.hash; });
            if (duplicate != index.end())
            {
                const std::string& first = blobs[duplicate->blob].id;
                const std::string& second = blobs[std::next(duplicate)->blob].id;
                std::string message = first == second ? std::format(
                                                            "{}shader id '{}' appears twice (blobs {} and {})",
                                                            prefix(name),
                                                            first,
                                                            duplicate->blob,
                                                            std::next(duplicate)->blob) :
                                                        std::format(
                                                            "{}shader ids '{}' and '{}' have the same hash {:016x}",
                                                            prefix(name),
                                                            first,
                                                            second,
                                                            duplicate->hash);
                return std::unexpected(make_error(error_kind::duplicate_shader_id, std::move(message)));
            }

            return shader_library{ std::move(blobs), std::move(index), std::move(name), next_serial() };
        }
    } // namespace detail

    result<shader_library> shader_library::create(std::vector<shader_blob> blobs, std::string name)
    {
        std::vector<std::uint64_t> hashes;
        hashes.reserve(blobs.size());
        for (const shader_blob& blob : blobs)
        {
            hashes.push_back(make_shader_id(blob.id).hash);
        }
        return detail::create_shader_library(std::move(blobs), std::move(name), hashes);
    }

    shader_library::shader_library(
        std::vector<shader_blob> blobs, std::vector<index_entry> index, std::string name, const std::uint64_t serial) :
        blobs_{ std::move(blobs) },
        index_{ std::move(index) },
        name_{ std::move(name) },
        serial_{ serial }
    {}

    shader_library::shader_library(shader_library&& other) noexcept :
        blobs_{ std::exchange(other.blobs_, {}) },
        index_{ std::exchange(other.index_, {}) },
        name_{ std::exchange(other.name_, {}) },
        serial_{ std::exchange(other.serial_, 0) }
    {}

    shader_library& shader_library::operator=(shader_library&& other) noexcept
    {
        if (this != &other)
        {
            blobs_ = std::exchange(other.blobs_, {});
            index_ = std::exchange(other.index_, {});
            name_ = std::exchange(other.name_, {});
            serial_ = std::exchange(other.serial_, 0);
        }
        return *this;
    }

    shader_library::~shader_library() = default;

    std::optional<shader_library::entry> shader_library::find(const shader_id id) const noexcept
    {
        const auto found = std::ranges::lower_bound(index_, id.hash, {}, &index_entry::hash);
        if (found == index_.end() || found->hash != id.hash)
        {
            return std::nullopt;
        }
        const shader_blob& blob = blobs_[found->blob];
        return entry{
            .id = blob.id,
            .stage = blob.stage,
            .bytecode = blob.bytecode,
            .bindings = blob.bindings ? &*blob.bindings : nullptr,
        };
    }

    std::size_t shader_library::size() const noexcept
    {
        return blobs_.size();
    }

    const std::string& shader_library::name() const noexcept
    {
        return name_;
    }

    std::uint64_t shader_library::serial() const noexcept
    {
        return serial_;
    }
} // namespace avis::graphics
