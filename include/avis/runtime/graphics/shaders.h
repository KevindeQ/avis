#ifndef RUNTIME_GRAPHICS_SHADERS_H
#define RUNTIME_GRAPHICS_SHADERS_H

#include "avis/core/common.h"

namespace graphics
{
    class shader_ref;

    class shader_manager
    {
    public:
        struct shader_object
        {
        public:
            shader_object(const std::span<unsigned char> compiled_byte_code);

            const std::span<const unsigned char> data() const;

        private:
            std::vector<unsigned char> byte_code;
        };

        shader_ref create_new(const std::span<unsigned char> compiled_byte_code);

    private:
        std::uint64_t hash_byte_code(const std::span<unsigned char> compiled_byte_code);

        using key = std::uint64_t;
        using value = std::shared_ptr<shader_object>;
        using data_store = std::unordered_map<key, value>;

        data_store shaders;
    };

    class shader_ref
    {
    public:
        shader_ref(std::shared_ptr<shader_manager::shader_object> referenced_shader);

        std::span<const unsigned char> byte_code() const;

    private:
        std::shared_ptr<shader_manager::shader_object> shader;
    };
} // namespace graphics

#endif
