#ifndef AVIS_GRAPHICS_SHADER_LIBRARY_H
#define AVIS_GRAPHICS_SHADER_LIBRARY_H

// Shader ids and the immutable shader library (docs/design/18-graphics-shaders-and-pipelines.md §2). Depends on
// neither the RHI nor the IO layer: load_shader_library (shader_loading.h) fills it from files, a test or tool with
// bytecode in memory calls shader_library::create directly.

#include "avis/error.h"
#include "avis/graphics/shader_bindings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace avis::graphics
{
    // 64-bit FNV-1a hash of the shader id string ("lighting.main.cs"). Unrelated to the manifest's xxh3 blob checksum.
    struct shader_id
    {
        std::uint64_t hash = 0;

        friend constexpr bool operator==(shader_id, shader_id) noexcept = default;
    };

    // The hash both the literal and the library use
    constexpr shader_id make_shader_id(const std::string_view text) noexcept
    {
        std::uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
        for (const char character : text)
        {
            hash ^= static_cast<unsigned char>(character);
            hash *= 0x100000001b3ULL; // FNV-1a 64-bit prime
        }
        return shader_id{ hash };
    }

    namespace literals
    {
        // "lighting.main.cs"_shader_id, a compile-time constant
        consteval shader_id operator""_shader_id(const char* text, const std::size_t length) noexcept
        {
            return make_shader_id(std::string_view{ text, length });
        }
    } // namespace literals

    enum class shader_stage : std::uint8_t
    {
        vertex,
        pixel,
        compute
    };

    struct shader_blob
    {
        std::string id; // "lighting.main.cs"
        shader_stage stage = shader_stage::compute;
        std::vector<std::byte> bytecode; // DXIL; moved in, the library keeps the only copy
        std::optional<shader_bindings> bindings; // registers used, for root signature matching (18 §5)
    };

    class shader_library;

    namespace detail
    {
        // create() with the id hashes given instead of computed (one per blob), so a test can force a collision. Not
        // for application code; declared here because shader_library befriends it.
        result<shader_library> create_shader_library(
            std::vector<shader_blob> blobs, std::string name, std::span<const std::uint64_t> hashes);
    } // namespace detail

    // Immutable after creation, so many workers can read it at once
    class shader_library
    {
    public:
        // Fails with invalid_argument on an empty id or bytecode, and with duplicate_shader_id on a duplicate id or two
        // ids with the same hash (the message names both). `name` is for messages only. In debug builds, a blob that
        // is a DXIL container whose shader kind differs from its stage fails with invalid_argument.
        static result<shader_library> create(std::vector<shader_blob> blobs, std::string name = {});

        // A moved-from library is empty: no shaders, an empty name and serial 0
        shader_library(shader_library&& other) noexcept;
        shader_library& operator=(shader_library&& other) noexcept;
        shader_library(const shader_library&) = delete;
        shader_library& operator=(const shader_library&) = delete;
        ~shader_library();

        struct entry
        {
            std::string_view id;
            shader_stage stage;
            std::span<const std::byte> bytecode;
            const shader_bindings* bindings; // null when the blob had none
        };

        // The views stay valid while the library (or the library it was moved into) is alive
        std::optional<entry> find(shader_id id) const noexcept;
        std::size_t size() const noexcept;
        const std::string& name() const noexcept;
        std::uint64_t serial() const noexcept; // unique per created library, never 0; part of pipeline cache keys

    private:
        friend result<shader_library> detail::create_shader_library(
            std::vector<shader_blob> blobs, std::string name, std::span<const std::uint64_t> hashes);

        struct index_entry
        {
            std::uint64_t hash;
            std::uint32_t blob;
        };

        shader_library(
            std::vector<shader_blob> blobs, std::vector<index_entry> index, std::string name, std::uint64_t serial);

        std::vector<shader_blob> blobs_;
        std::vector<index_entry> index_; // sorted by hash, for find by binary search
        std::string name_;
        std::uint64_t serial_ = 0;
    };
} // namespace avis::graphics

#endif
