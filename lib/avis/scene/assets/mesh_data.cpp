#include "avis/scene/assets/mesh_data.h"

#include "avis/scene/error.h"
#include "mesh_validation.h"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

namespace avis::scene
{
    std::uint64_t mesh_data::byte_size() const noexcept
    {
        const auto bytes = [](const auto& stream) -> std::uint64_t {
            return stream.size() * sizeof(typename std::remove_cvref_t<decltype(stream)>::value_type);
        };
        return bytes(positions) + bytes(normals) + bytes(tangents) + bytes(tangent_signs) + bytes(uv0) +
               bytes(color0) + bytes(indices);
    }
} // namespace avis::scene

namespace avis::scene::detail
{
    namespace
    {
        bool has(const mesh_data& data, const vertex_attribute attribute) noexcept
        {
            return static_cast<bool>(data.attributes & attribute);
        }

        // Empty string when the stream matches the attribute set
        template<typename Stream>
        std::string check_stream(
            const Stream& stream, const std::string_view name, const bool present, const std::size_t vertex_count)
        {
            if (present && stream.size() != vertex_count)
            {
                return std::format("{} has {} entries for {} positions", name, stream.size(), vertex_count);
            }
            if (!present && !stream.empty())
            {
                return std::format("{} has {} entries but the attributes don't include it", name, stream.size());
            }
            return {};
        }

        std::string first_problem(const mesh_data& data)
        {
            const std::size_t vertex_count = data.positions.size();
            for (const std::string& problem : {
                     check_stream(data.normals, "normals", has(data, vertex_attribute::normal), vertex_count),
                     check_stream(data.tangents, "tangents", has(data, vertex_attribute::tangent), vertex_count),
                     check_stream(
                         data.tangent_signs, "tangent_signs", has(data, vertex_attribute::tangent), vertex_count),
                     check_stream(data.uv0, "uv0", has(data, vertex_attribute::uv0), vertex_count),
                     check_stream(data.color0, "color0", has(data, vertex_attribute::color0), vertex_count),
                 })
            {
                if (!problem.empty())
                {
                    return problem;
                }
            }

            if (data.primitives.empty())
            {
                return "no primitives";
            }

            bool indexed = false;
            for (std::size_t number = 0; number < data.primitives.size(); ++number)
            {
                const mesh_primitive& primitive = data.primitives[number];
                if (std::uint64_t{ primitive.vertex_first } + primitive.vertex_count > vertex_count)
                {
                    return std::format(
                        "primitive {} has vertices [{}, {}) past the {} positions",
                        number,
                        primitive.vertex_first,
                        std::uint64_t{ primitive.vertex_first } + primitive.vertex_count,
                        vertex_count);
                }
                if (std::uint64_t{ primitive.index_first } + primitive.index_count > data.indices.size())
                {
                    return std::format(
                        "primitive {} has indices [{}, {}) past the {} indices",
                        number,
                        primitive.index_first,
                        std::uint64_t{ primitive.index_first } + primitive.index_count,
                        data.indices.size());
                }
                for (std::uint32_t offset = 0; offset < primitive.index_count; ++offset)
                {
                    const std::uint32_t index = data.indices[primitive.index_first + offset];
                    if (index >= primitive.vertex_count)
                    {
                        return std::format(
                            "primitive {}: index {} at position {} is out of range for its {} vertices",
                            number,
                            index,
                            primitive.index_first + offset,
                            primitive.vertex_count);
                    }
                }
                indexed = indexed || primitive.index_count != 0;
            }

            if (!indexed && !data.indices.empty())
            {
                return std::format("{} indices but no indexed primitive", data.indices.size());
            }
            return {};
        }
    } // namespace

    result<void> validate_and_bound(mesh_data& data)
    {
        if (const std::string problem = first_problem(data); !problem.empty())
        {
            return std::unexpected{ make_error(error_kind::invalid_mesh, std::format("invalid mesh: {}", problem)) };
        }

        data.bounds = aabb{};
        for (mesh_primitive& primitive : data.primitives)
        {
            primitive.bounds = aabb{};
            for (std::uint32_t vertex = 0; vertex < primitive.vertex_count; ++vertex)
            {
                primitive.bounds.extend(data.positions[primitive.vertex_first + vertex]);
            }
            data.bounds.extend(primitive.bounds);
        }
        return {};
    }
} // namespace avis::scene::detail
