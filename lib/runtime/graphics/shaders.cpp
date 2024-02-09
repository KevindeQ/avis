#include "avis/runtime/graphics/shaders.h"

#include "xxhash.h"

namespace graphics
{
    shader_manager::shader_object::shader_object(const std::span<unsigned char> compiled_byte_code) :
        byte_code{ compiled_byte_code.begin(), compiled_byte_code.end() }
    {}

    const std::span<const unsigned char> shader_manager::shader_object::data() const
    {
        return std::span{ byte_code.begin(), byte_code.end() };
    }

    shader_ref shader_manager::create_new(const std::span<unsigned char> compiled_byte_code)
    {
        std::uint64_t key = hash_byte_code(compiled_byte_code);
        data_store::const_iterator it = shaders.find(key);
        if (it != shaders.cend())
        {
            return it->second;
        }

        value new_value = std::make_shared<shader_object>(compiled_byte_code);
        shaders.insert({ key, new_value });

        return shader_ref{ new_value };
    }

    std::uint64_t shader_manager::hash_byte_code(const std::span<unsigned char> compiled_byte_code)
    {
        return XXH3_64bits(compiled_byte_code.data(), compiled_byte_code.size());
    }

    shader_ref::shader_ref(std::shared_ptr<shader_manager::shader_object> referenced_shader) :
        shader{ referenced_shader }
    {}

    std::span<const unsigned char> shader_ref::byte_code() const
    {
        return shader->data();
    }
}