#include "rendering/shader_manifest_parser.h"

#include "rendering/yaml_reading.h"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        using yaml_reading::child_path;
        using yaml_reading::element_path;

        constexpr std::string_view shaders_path = "shaders";
        constexpr std::size_t hash_digits = 16;

        class manifest_parser
        {
        public:
            explicit manifest_parser(const std::string_view source_name) : reader_{ source_name } {}

            result<graphics::shader_manifest> parse(const std::string_view text) const
            {
                YAML::Node root;
                try
                {
                    root = YAML::Load(std::string{ text });
                }
                catch (const YAML::Exception& exception)
                {
                    return std::unexpected{ reader_.error_at(exception.mark, {}, exception.msg) };
                }

                if (auto checked = reader_.expect_map(root, {}); !checked)
                {
                    return std::unexpected{ std::move(checked).error() };
                }
                if (auto checked = reader_.check_keys(root, {}, { "avis_shader_manifest", "shaders" }); !checked)
                {
                    return std::unexpected{ std::move(checked).error() };
                }

                auto format = read_format(root);
                if (!format)
                {
                    return std::unexpected{ std::move(format).error() };
                }

                auto shaders = reader_.required(root, shaders_path, {});
                if (!shaders)
                {
                    return std::unexpected{ std::move(shaders).error() };
                }
                if (auto checked = reader_.expect_sequence(*shaders, shaders_path); !checked)
                {
                    return std::unexpected{ std::move(checked).error() };
                }

                graphics::shader_manifest manifest;
                manifest.entries.reserve(shaders->size());
                std::unordered_map<std::string, int> id_lines; // id -> 1-based line of its first declaration
                for (std::size_t index = 0; index < shaders->size(); ++index)
                {
                    const YAML::Node shader = (*shaders)[index];
                    const std::string path = element_path(shaders_path, index);
                    auto entry = read_entry(shader, path, *format);
                    if (!entry)
                    {
                        return std::unexpected{ std::move(entry).error() };
                    }

                    const YAML::Node id = shader["id"];
                    const int line = id.Mark().line + 1;
                    if (const auto [first, inserted] = id_lines.try_emplace(entry->id, line); !inserted)
                    {
                        return std::unexpected{ reader_.error_at(
                            id,
                            child_path(path, "id"),
                            std::format("duplicate id '{}', first declared on line {}", entry->id, first->second)) };
                    }
                    manifest.entries.push_back(std::move(*entry));
                }
                return manifest;
            }

        private:
            result<std::uint32_t> read_format(const YAML::Node& root) const
            {
                constexpr std::string_view key = "avis_shader_manifest";
                auto node = reader_.required(root, key, {});
                if (!node)
                {
                    return std::unexpected{ std::move(node).error() };
                }
                auto format = reader_.unsigned_integer_32(*node, key);
                if (!format)
                {
                    return std::unexpected{ std::move(format).error() };
                }
                if (*format < oldest_shader_manifest_format || *format > newest_shader_manifest_format)
                {
                    return std::unexpected{ reader_.error_at(
                        *node,
                        key,
                        std::format(
                            "unsupported format {} (this Visualizer reads {} to {})",
                            *format,
                            oldest_shader_manifest_format,
                            newest_shader_manifest_format)) };
                }
                return format;
            }

            result<graphics::shader_manifest_entry>
                read_entry(const YAML::Node& shader, const std::string& path, const std::uint32_t format) const
            {
                if (auto checked = reader_.expect_map(shader, path); !checked)
                {
                    return std::unexpected{ std::move(checked).error() };
                }
                const auto keys =
                    format >= 2 ?
                        reader_.check_keys(
                            shader, path, { "id", "file", "stage", "entry", "defines", "xxh3", "bindings" }) :
                        reader_.check_keys(shader, path, { "id", "file", "stage", "entry", "defines", "xxh3" });
                if (!keys)
                {
                    return std::unexpected{ keys.error() };
                }

                graphics::shader_manifest_entry entry;

                // id: a non-empty string
                auto id_node = reader_.required(shader, "id", path);
                if (!id_node)
                {
                    return std::unexpected{ std::move(id_node).error() };
                }
                auto id = reader_.text(*id_node, child_path(path, "id"));
                if (!id)
                {
                    return std::unexpected{ std::move(id).error() };
                }
                if (id->empty())
                {
                    return std::unexpected{ reader_.error_at(*id_node, child_path(path, "id"), "empty id") };
                }
                entry.id = std::move(*id);

                // file: relative, inside the shader directory
                auto blob = read_blob(shader, path);
                if (!blob)
                {
                    return std::unexpected{ std::move(blob).error() };
                }
                entry.blob = std::move(*blob);

                auto stage = read_stage(shader, path);
                if (!stage)
                {
                    return std::unexpected{ std::move(stage).error() };
                }
                entry.stage = *stage;

                // entry and defines are informational and optional
                if (const YAML::Node node = shader["entry"]; node.IsDefined())
                {
                    auto value = reader_.text(node, child_path(path, "entry"));
                    if (!value)
                    {
                        return std::unexpected{ std::move(value).error() };
                    }
                    entry.entry = std::move(*value);
                }
                if (const YAML::Node node = shader["defines"]; node.IsDefined())
                {
                    auto values = reader_.text_sequence(node, child_path(path, "defines"));
                    if (!values)
                    {
                        return std::unexpected{ std::move(values).error() };
                    }
                    entry.defines = std::move(*values);
                }

                auto hash = read_hash(shader, path);
                if (!hash)
                {
                    return std::unexpected{ std::move(hash).error() };
                }
                entry.content_hash = *hash;

                if (format >= 2)
                {
                    auto bindings = read_bindings(shader, path);
                    if (!bindings)
                    {
                        return std::unexpected{ std::move(bindings).error() };
                    }
                    entry.bindings = std::move(*bindings);
                }
                return entry;
            }

            result<std::filesystem::path> read_blob(const YAML::Node& shader, const std::string& path) const
            {
                const std::string file_path = child_path(path, "file");
                auto node = reader_.required(shader, "file", path);
                if (!node)
                {
                    return std::unexpected{ std::move(node).error() };
                }
                auto text = reader_.text(*node, file_path);
                if (!text)
                {
                    return std::unexpected{ std::move(text).error() };
                }
                // UTF-8, so non-ASCII file names survive on Windows
                std::filesystem::path blob{ std::u8string_view{ reinterpret_cast<const char8_t*>(text->data()),
                                                                text->size() } };
                if (blob.empty())
                {
                    return std::unexpected{ reader_.error_at(*node, file_path, "empty path") };
                }
                if (blob.has_root_path())
                {
                    return std::unexpected{ reader_.error_at(
                        *node, file_path, std::format("'{}' isn't relative to the shader directory", *text)) };
                }
                for (const std::filesystem::path& element : blob)
                {
                    if (element == "..")
                    {
                        return std::unexpected{ reader_.error_at(
                            *node, file_path, std::format("'{}' reaches outside the shader directory", *text)) };
                    }
                }
                return blob;
            }

            result<graphics::shader_stage> read_stage(const YAML::Node& shader, const std::string& path) const
            {
                const std::string stage_path = child_path(path, "stage");
                auto node = reader_.required(shader, "stage", path);
                if (!node)
                {
                    return std::unexpected{ std::move(node).error() };
                }
                auto text = reader_.text(*node, stage_path);
                if (!text)
                {
                    return std::unexpected{ std::move(text).error() };
                }
                if (*text == "vs")
                {
                    return graphics::shader_stage::vertex;
                }
                if (*text == "ps")
                {
                    return graphics::shader_stage::pixel;
                }
                if (*text == "cs")
                {
                    return graphics::shader_stage::compute;
                }
                return std::unexpected{
                    reader_.error_at(*node, stage_path, std::format("expected vs, ps or cs, got '{}'", *text)),
                };
            }

            // Exactly 16 hexadecimal digits, either case, without a "0x" prefix
            result<std::uint64_t> read_hash(const YAML::Node& shader, const std::string& path) const
            {
                const std::string hash_path = child_path(path, "xxh3");
                auto node = reader_.required(shader, "xxh3", path);
                if (!node)
                {
                    return std::unexpected{ std::move(node).error() };
                }
                auto text = reader_.text(*node, hash_path);
                if (!text)
                {
                    return std::unexpected{ std::move(text).error() };
                }
                std::uint64_t hash = 0;
                const char* const end = text->data() + text->size();
                const auto [last, status] = std::from_chars(text->data(), end, hash, 16);
                if (text->size() != hash_digits || status != std::errc{} || last != end)
                {
                    return std::unexpected{ reader_.error_at(
                        *node,
                        hash_path,
                        std::format("expected {} hexadecimal digits, got '{}'", hash_digits, *text)) };
                }
                return hash;
            }

            result<graphics::shader_bindings> read_bindings(const YAML::Node& shader, const std::string& path) const
            {
                const std::string bindings_path = child_path(path, "bindings");
                auto node = reader_.required(shader, "bindings", path);
                if (!node)
                {
                    return std::unexpected{ std::move(node).error() };
                }
                if (auto checked = reader_.expect_map(*node, bindings_path); !checked)
                {
                    return std::unexpected{ std::move(checked).error() };
                }
                if (auto checked =
                        reader_.check_keys(*node, bindings_path, { "constant_buffers", "buffers", "samplers" });
                    !checked)
                {
                    return std::unexpected{ std::move(checked).error() };
                }

                graphics::shader_bindings bindings;
                auto constant_buffers = read_list(
                    *node,
                    bindings_path,
                    "constant_buffers",
                    { "register", "space", "size" },
                    [&](const YAML::Node& item, const std::string& item_path) -> result<void>
                    {
                        graphics::constant_buffer_binding binding;
                        if (auto read = read_register(item, item_path, binding.shader_register, binding.register_space);
                            !read)
                        {
                            return read;
                        }
                        auto size = read_field(item, item_path, "size");
                        if (!size)
                        {
                            return std::unexpected{ std::move(size).error() };
                        }
                        binding.size = *size;
                        bindings.constant_buffers.push_back(binding);
                        return {};
                    });
                if (!constant_buffers)
                {
                    return std::unexpected{ std::move(constant_buffers).error() };
                }

                auto buffers = read_list(
                    *node,
                    bindings_path,
                    "buffers",
                    { "kind", "register", "space" },
                    [&](const YAML::Node& item, const std::string& item_path) -> result<void>
                    {
                        graphics::buffer_binding binding;
                        const std::string kind_path = child_path(item_path, "kind");
                        auto kind_node = reader_.required(item, "kind", item_path);
                        if (!kind_node)
                        {
                            return std::unexpected{ std::move(kind_node).error() };
                        }
                        auto kind = reader_.text(*kind_node, kind_path);
                        if (!kind)
                        {
                            return std::unexpected{ std::move(kind).error() };
                        }
                        if (*kind == "shader_resource")
                        {
                            binding.kind = graphics::buffer_binding_kind::shader_resource;
                        }
                        else if (*kind == "unordered_access")
                        {
                            binding.kind = graphics::buffer_binding_kind::unordered_access;
                        }
                        else
                        {
                            return std::unexpected{ reader_.error_at(
                                *kind_node,
                                kind_path,
                                std::format("expected shader_resource or unordered_access, got '{}'", *kind)) };
                        }
                        if (auto read = read_register(item, item_path, binding.shader_register, binding.register_space);
                            !read)
                        {
                            return read;
                        }
                        bindings.buffers.push_back(binding);
                        return {};
                    });
                if (!buffers)
                {
                    return std::unexpected{ std::move(buffers).error() };
                }

                auto samplers = read_list(
                    *node,
                    bindings_path,
                    "samplers",
                    { "register", "space" },
                    [&](const YAML::Node& item, const std::string& item_path) -> result<void>
                    {
                        graphics::sampler_binding binding;
                        if (auto read = read_register(item, item_path, binding.shader_register, binding.register_space);
                            !read)
                        {
                            return read;
                        }
                        bindings.samplers.push_back(binding);
                        return {};
                    });
                if (!samplers)
                {
                    return std::unexpected{ std::move(samplers).error() };
                }
                return bindings;
            }

            // A required sequence of mappings with the given keys, each passed to `read_item`
            template<class ReadItem>
            result<void> read_list(
                const YAML::Node& bindings,
                const std::string& bindings_path,
                const std::string_view key,
                const std::initializer_list<std::string_view> item_keys,
                ReadItem&& read_item) const
            {
                const std::string list_path = child_path(bindings_path, key);
                auto list = reader_.required(bindings, key, bindings_path);
                if (!list)
                {
                    return std::unexpected{ std::move(list).error() };
                }
                if (auto checked = reader_.expect_sequence(*list, list_path); !checked)
                {
                    return checked;
                }
                for (std::size_t index = 0; index < list->size(); ++index)
                {
                    const YAML::Node item = (*list)[index];
                    const std::string item_path = element_path(list_path, index);
                    if (auto checked = reader_.expect_map(item, item_path); !checked)
                    {
                        return checked;
                    }
                    if (auto checked = reader_.check_keys(item, item_path, item_keys); !checked)
                    {
                        return checked;
                    }
                    if (auto read = read_item(item, item_path); !read)
                    {
                        return read;
                    }
                }
                return {};
            }

            result<void> read_register(
                const YAML::Node& item,
                const std::string& item_path,
                std::uint32_t& shader_register,
                std::uint32_t& register_space) const
            {
                auto read_register = read_field(item, item_path, "register");
                if (!read_register)
                {
                    return std::unexpected{ std::move(read_register).error() };
                }
                auto read_space = read_field(item, item_path, "space");
                if (!read_space)
                {
                    return std::unexpected{ std::move(read_space).error() };
                }
                shader_register = *read_register;
                register_space = *read_space;
                return {};
            }

            result<std::uint32_t>
                read_field(const YAML::Node& item, const std::string& item_path, const std::string_view key) const
            {
                auto node = reader_.required(item, key, item_path);
                if (!node)
                {
                    return std::unexpected{ std::move(node).error() };
                }
                return reader_.unsigned_integer_32(*node, child_path(item_path, key));
            }

            yaml_reading::reader reader_;
        };
    } // namespace

    result<graphics::shader_manifest>
        parse_shader_manifest(const std::string_view yaml_text, const std::string_view source_name)
    {
        return manifest_parser{ source_name }.parse(yaml_text);
    }
} // namespace avis::visualizer::rendering
