#include "gltf_import.h"

#include "wic_decoder.h"

#include "avis/log.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/error.h"
#include "avis/threading/parallel_helpers.h"

#include <Eigen/Dense>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <numbers>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <xxhash.h>

namespace avis::scene::detail
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";

        // Extensions the import understands. A required extension outside this set is gltf_unsupported; a used one is
        // ignored with a warning.
        constexpr fastgltf::Extensions supported_extensions =
            fastgltf::Extensions::KHR_texture_transform | fastgltf::Extensions::KHR_materials_emissive_strength |
            fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_materials_unlit;

        constexpr std::array<std::string_view, 4> supported_extension_names = {
            "KHR_texture_transform",
            "KHR_materials_emissive_strength",
            "KHR_mesh_quantization",
            "KHR_materials_unlit",
        };

        std::mutex decode_hook_mutex;
        std::function<void(std::size_t)> decode_hook;

        // fastgltf's strings may use a polymorphic allocator
        template<typename String>
        std::string text(const String& value)
        {
            return std::string{ std::string_view{ value } };
        }

        std::string utf8(const std::filesystem::path& path)
        {
            const std::u8string text = path.u8string();
            return std::string{ text.begin(), text.end() };
        }

        // ---- axes: glTF is Y up, Avis Z up; R = RotX(+90°) maps glTF +Y to Avis +Z (00 §4.1) ----

        Eigen::Vector3f to_avis(const float x, const float y, const float z) noexcept
        {
            return { x, -z, y };
        }

        Eigen::Vector3f to_avis(const fastgltf::math::fvec3& value) noexcept
        {
            return to_avis(value[0], value[1], value[2]);
        }

        const Eigen::Quaternionf& axis_rotation() noexcept
        {
            static const Eigen::Quaternionf rotation{ Eigen::AngleAxisf{ std::numbers::pi_v<float> / 2.0f,
                                                                         Eigen::Vector3f::UnitX() } };
            return rotation;
        }

        // R · local · R⁻¹. TRS stays exact: the translation rotates, the rotation is conjugated, and the scale's Y
        // and Z swap (R · diag(x, y, z) · R⁻¹ = diag(x, z, y)).
        transform node_transform(const fastgltf::Node& node)
        {
            if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform))
            {
                const Eigen::Quaternionf rotation{
                    trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]
                };
                transform local;
                local.position = to_avis(trs->translation);
                local.rotation = (axis_rotation() * rotation.normalized() * axis_rotation().conjugate()).normalized();
                local.scale = { trs->scale[0], trs->scale[2], trs->scale[1] };
                return local;
            }

            const auto& matrix = std::get<fastgltf::math::fmat4x4>(node.transform);
            Eigen::Affine3f affine = Eigen::Affine3f::Identity();
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 3; ++row)
                {
                    affine.matrix()(row, column) =
                        matrix[static_cast<std::size_t>(column)][static_cast<std::size_t>(row)];
                }
            }
            const Eigen::Matrix3f axes = axis_rotation().toRotationMatrix();
            Eigen::Affine3f converted = Eigen::Affine3f::Identity();
            converted.linear() = axes * affine.linear() * axes.transpose();
            converted.translation() = axes * affine.translation();
            return transform::from_affine(converted);
        }

        // ---- errors ----

        avis::error invalid(const std::filesystem::path& path, const std::string_view message)
        {
            return make_error(error_kind::gltf_invalid, std::format("glTF '{}': {}", utf8(path), message));
        }

        avis::error unsupported(const std::filesystem::path& path, const std::string_view message)
        {
            return make_error(error_kind::gltf_unsupported, std::format("glTF '{}': {}", utf8(path), message));
        }

        avis::error parse_error(const std::filesystem::path& path, const fastgltf::Error error)
        {
            if (error == fastgltf::Error::MissingExtensions || error == fastgltf::Error::UnknownRequiredExtension)
            {
                std::string supported;
                for (const std::string_view name : supported_extension_names)
                {
                    supported += supported.empty() ? "" : ", ";
                    supported += name;
                }
                return unsupported(
                    path,
                    std::format(
                        "it requires an extension outside the supported set ({}): {}",
                        supported,
                        fastgltf::getErrorMessage(error)));
            }
            return invalid(path, fastgltf::getErrorMessage(error));
        }

        // ---- accessors ----

        std::string_view attribute_type_name(const fastgltf::AccessorType type) noexcept
        {
            switch (type)
            {
            case fastgltf::AccessorType::Scalar:
                return "SCALAR";
            case fastgltf::AccessorType::Vec2:
                return "VEC2";
            case fastgltf::AccessorType::Vec3:
                return "VEC3";
            case fastgltf::AccessorType::Vec4:
                return "VEC4";
            default:
                return "a matrix";
            }
        }

        // Reads an accessor's elements, after checking its type (fastgltf only asserts it)
        template<typename Element>
        result<std::vector<Element>> read_accessor(
            const fastgltf::Asset& asset,
            const std::filesystem::path& path,
            const std::size_t index,
            const fastgltf::AccessorType type,
            const std::string_view what)
        {
            if (index >= asset.accessors.size())
            {
                return std::unexpected(invalid(path, std::format("{}: accessor {} doesn't exist", what, index)));
            }
            const fastgltf::Accessor& accessor = asset.accessors[index];
            if (accessor.type != type)
            {
                return std::unexpected(invalid(
                    path,
                    std::format(
                        "{}: accessor {} is {}, expected {}",
                        what,
                        index,
                        attribute_type_name(accessor.type),
                        attribute_type_name(type))));
            }
            std::vector<Element> elements(accessor.count);
            fastgltf::iterateAccessorWithIndex<Element>(
                asset, accessor, [&](const Element& element, const std::size_t at) { elements[at] = element; });
            return elements;
        }

        // ---- per-primitive geometry, in floats ----

        struct primitive_streams
        {
            mesh_topology topology = mesh_topology::triangles;
            std::vector<Eigen::Vector3f> positions;
            std::vector<Eigen::Vector3f> normals; // empty when absent
            std::vector<Eigen::Vector4f> tangents; // xyz, w the bitangent sign
            std::vector<Eigen::Vector2f> uvs;
            std::vector<std::array<std::uint8_t, 4>> colors;
            std::vector<std::uint32_t> indices;
            bool indexed = false;
        };

        // Linear [0, 1] to an sRGB u8, with the exact transfer function (IEC 61966-2-1)
        std::uint8_t linear_to_srgb8(const float value) noexcept
        {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            const float encoded =
                clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
            return static_cast<std::uint8_t>(std::lround(encoded * 255.0f));
        }

        std::uint8_t linear_to_u8(const float value) noexcept
        {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        Eigen::Vector3f any_perpendicular(const Eigen::Vector3f& normal) noexcept
        {
            const Eigen::Vector3f axis =
                std::abs(normal.x()) < 0.9f ? Eigen::Vector3f::UnitX() : Eigen::Vector3f::UnitY();
            return (axis - normal * normal.dot(axis)).normalized();
        }

        std::uint32_t corner(const primitive_streams& streams, const std::size_t at) noexcept
        {
            return streams.indexed ? streams.indices[at] : static_cast<std::uint32_t>(at);
        }

        std::size_t corner_count(const primitive_streams& streams) noexcept
        {
            return streams.indexed ? streams.indices.size() : streams.positions.size();
        }

        // Turns strips, fans and loops into lists (glTF 2.0 §3.7.2.1); the result is indexed
        void convert_to_list(primitive_streams& streams, const fastgltf::PrimitiveType type)
        {
            std::vector<std::uint32_t> source = std::move(streams.indices);
            if (!streams.indexed)
            {
                source.resize(streams.positions.size());
                for (std::size_t at = 0; at < source.size(); ++at)
                {
                    source[at] = static_cast<std::uint32_t>(at);
                }
            }

            std::vector<std::uint32_t> list;
            const std::size_t count = source.size();
            switch (type)
            {
            case fastgltf::PrimitiveType::TriangleStrip:
                for (std::size_t at = 0; at + 2 < count; ++at)
                {
                    // Every other triangle swaps its first two vertices, so the winding stays the same
                    const bool odd = (at % 2) != 0;
                    list.insert(list.end(), { source[odd ? at + 1 : at], source[odd ? at : at + 1], source[at + 2] });
                }
                break;
            case fastgltf::PrimitiveType::TriangleFan:
                for (std::size_t at = 1; at + 1 < count; ++at)
                {
                    list.insert(list.end(), { source[at], source[at + 1], source[0] });
                }
                break;
            case fastgltf::PrimitiveType::LineStrip:
            case fastgltf::PrimitiveType::LineLoop:
                for (std::size_t at = 0; at + 1 < count; ++at)
                {
                    list.insert(list.end(), { source[at], source[at + 1] });
                }
                if (type == fastgltf::PrimitiveType::LineLoop && count > 2)
                {
                    list.insert(list.end(), { source[count - 1], source[0] });
                }
                break;
            default:
                list = std::move(source);
                break;
            }
            streams.indices = std::move(list);
            streams.indexed = true;
        }

        // glTF 2.0 §3.7.2.1: without normals, flat normals; the tangents are then ignored. Every triangle gets its own
        // three vertices, so the primitive becomes non-indexed.
        void generate_flat_normals(primitive_streams& streams)
        {
            const std::size_t triangles = corner_count(streams) / 3;
            primitive_streams flat;
            flat.topology = streams.topology;
            flat.positions.reserve(triangles * 3);
            flat.normals.reserve(triangles * 3);
            for (std::size_t triangle = 0; triangle < triangles; ++triangle)
            {
                const std::array<std::uint32_t, 3> vertices = { corner(streams, triangle * 3),
                                                                corner(streams, triangle * 3 + 1),
                                                                corner(streams, triangle * 3 + 2) };
                const Eigen::Vector3f& a = streams.positions[vertices[0]];
                Eigen::Vector3f normal = (streams.positions[vertices[1]] - a).cross(streams.positions[vertices[2]] - a);
                normal = normal.squaredNorm() > 0.0f ? normal.normalized() : Eigen::Vector3f::UnitZ();
                for (const std::uint32_t vertex : vertices)
                {
                    flat.positions.push_back(streams.positions[vertex]);
                    flat.normals.push_back(normal);
                    if (!streams.uvs.empty())
                    {
                        flat.uvs.push_back(streams.uvs[vertex]);
                    }
                    if (!streams.colors.empty())
                    {
                        flat.colors.push_back(streams.colors[vertex]);
                    }
                }
            }
            streams = std::move(flat);
        }

        // Per-vertex tangents from the uv gradients, accumulated over the triangles and orthogonalised against the
        // normal (Lengyel's method, not MikkTSpace). The sign follows glTF: bitangent = cross(normal, tangent) · w
        // points along decreasing v, since glTF's v runs down the image. Without uvs, any tangent perpendicular to
        // the normal.
        void generate_tangents(primitive_streams& streams)
        {
            const std::size_t vertices = streams.positions.size();
            std::vector<Eigen::Vector3f> tangents(vertices, Eigen::Vector3f::Zero());
            std::vector<Eigen::Vector3f> bitangents(vertices, Eigen::Vector3f::Zero());
            if (!streams.uvs.empty())
            {
                const std::size_t triangles = corner_count(streams) / 3;
                for (std::size_t triangle = 0; triangle < triangles; ++triangle)
                {
                    const std::uint32_t i0 = corner(streams, triangle * 3);
                    const std::uint32_t i1 = corner(streams, triangle * 3 + 1);
                    const std::uint32_t i2 = corner(streams, triangle * 3 + 2);
                    const Eigen::Vector3f edge1 = streams.positions[i1] - streams.positions[i0];
                    const Eigen::Vector3f edge2 = streams.positions[i2] - streams.positions[i0];
                    // v flipped, so the bitangent is dP/d(1 - v)
                    const Eigen::Vector2f delta1{ streams.uvs[i1].x() - streams.uvs[i0].x(),
                                                  streams.uvs[i0].y() - streams.uvs[i1].y() };
                    const Eigen::Vector2f delta2{ streams.uvs[i2].x() - streams.uvs[i0].x(),
                                                  streams.uvs[i0].y() - streams.uvs[i2].y() };
                    const float determinant = delta1.x() * delta2.y() - delta2.x() * delta1.y();
                    if (std::abs(determinant) < 1e-12f)
                    {
                        continue;
                    }
                    const float inverse = 1.0f / determinant;
                    const Eigen::Vector3f tangent = (edge1 * delta2.y() - edge2 * delta1.y()) * inverse;
                    const Eigen::Vector3f bitangent = (edge2 * delta1.x() - edge1 * delta2.x()) * inverse;
                    for (const std::uint32_t vertex : { i0, i1, i2 })
                    {
                        tangents[vertex] += tangent;
                        bitangents[vertex] += bitangent;
                    }
                }
            }

            streams.tangents.resize(vertices);
            for (std::size_t vertex = 0; vertex < vertices; ++vertex)
            {
                const Eigen::Vector3f& normal = streams.normals[vertex];
                Eigen::Vector3f tangent = tangents[vertex] - normal * normal.dot(tangents[vertex]);
                tangent = tangent.squaredNorm() > 1e-20f ? tangent.normalized() : any_perpendicular(normal);
                const float sign = normal.cross(tangent).dot(bitangents[vertex]) < 0.0f ? -1.0f : 1.0f;
                streams.tangents[vertex] = { tangent.x(), tangent.y(), tangent.z(), sign };
            }
        }

        mesh_topology topology_of(const fastgltf::PrimitiveType type) noexcept
        {
            switch (type)
            {
            case fastgltf::PrimitiveType::Points:
                return mesh_topology::points;
            case fastgltf::PrimitiveType::Lines:
            case fastgltf::PrimitiveType::LineLoop:
            case fastgltf::PrimitiveType::LineStrip:
                return mesh_topology::lines;
            default:
                return mesh_topology::triangles;
            }
        }

        std::string_view topology_name(const mesh_topology topology) noexcept
        {
            switch (topology)
            {
            case mesh_topology::triangles:
                return "triangles";
            case mesh_topology::lines:
                return "lines";
            default:
                return "points";
            }
        }

        std::string_view mime_name(const fastgltf::MimeType type) noexcept
        {
            switch (type)
            {
            case fastgltf::MimeType::KTX2:
                return "KTX2";
            case fastgltf::MimeType::DDS:
                return "DDS";
            case fastgltf::MimeType::WEBP:
                return "WebP";
            default:
                return "";
            }
        }

        // The uv transform baked into a primitive's uvs (KHR_texture_transform, offset and scale only)
        struct uv_transform
        {
            Eigen::Vector2f offset = Eigen::Vector2f::Zero();
            Eigen::Vector2f scale = Eigen::Vector2f::Ones();
        };
    } // namespace

    void set_image_decode_hook(std::function<void(std::size_t)> hook)
    {
        const std::scoped_lock lock{ decode_hook_mutex };
        decode_hook = std::move(hook);
    }

    // ---- receivers ----

    void gltf_read_receiver::set_value(std::vector<std::byte> bytes) noexcept
    {
        if (resource == main_file)
        {
            core->file = std::move(bytes);
            core->hop(gltf_stage::parse);
            return;
        }
        core->external[resource] = std::move(bytes);
        core->release_read();
    }

    void gltf_read_receiver::set_error(avis::error error) noexcept
    {
        if (resource == main_file)
        {
            // An IO error passes through unchanged
            core->finish(std::unexpected(std::move(error)));
            return;
        }
        core->fail(std::move(error));
        core->release_read();
    }

    void gltf_read_receiver::set_stopped() noexcept
    {
        if (resource == main_file)
        {
            core->finish_stopped();
            return;
        }
        core->read_stopped.store(true, std::memory_order_relaxed);
        core->release_read();
    }

    void gltf_hop_receiver::set_value() noexcept
    {
        core->run(stage);
    }

    template<class Error>
    void gltf_hop_receiver::set_error(Error&& error) noexcept
    {
        if (core->stop_requested.load(std::memory_order_relaxed))
        {
            core->finish_stopped();
            return;
        }
        core->finish(std::unexpected(threading::detail::to_internal_error(std::forward<Error>(error))));
    }

    void gltf_hop_receiver::set_stopped() noexcept
    {
        // The parallel scheduler is stopping: the load can't go on
        if (core->stop_requested.load(std::memory_order_relaxed))
        {
            core->finish_stopped();
            return;
        }
        core->finish(
            std::unexpected(
                make_error(generic_error_kind::cancelled, "load_gltf: the parallel scheduler is stopping")));
    }

    void gltf_decode_item::operator()(const std::size_t index) const noexcept
    {
        core->decode(index);
    }

    void gltf_decode_receiver::set_value() noexcept
    {
        core->after_decodes();
    }

    template<class Error>
    void gltf_decode_receiver::set_error(Error&& error) noexcept
    {
        if (core->stop_requested.load(std::memory_order_relaxed))
        {
            core->finish_stopped();
            return;
        }
        core->finish(std::unexpected(threading::detail::to_internal_error(std::forward<Error>(error))));
    }

    void gltf_decode_receiver::set_stopped() noexcept
    {
        if (core->stop_requested.load(std::memory_order_relaxed))
        {
            core->finish_stopped();
            return;
        }
        core->finish(
            std::unexpected(
                make_error(generic_error_kind::cancelled, "load_gltf: the parallel scheduler is stopping")));
    }

    // ---- stage plumbing ----

    gltf_load_core::gltf_load_core(gltf_load_operation& owner, asset_cache& cache, std::filesystem::path path) :
        owner{ &owner },
        cache{ &cache },
        path{ std::move(path) }
    {}

    void gltf_load_core::begin() noexcept
    {
        if (cache->io() == nullptr)
        {
            finish(
                std::unexpected(make_error(
                    generic_error_kind::logic_error,
                    std::format("load_gltf '{}': the asset cache was created without an IO context", utf8(path)))));
            return;
        }
        try
        {
            main_read = std::unique_ptr<gltf_read_operation>{ new gltf_read_operation{ stdexec::connect(
                io::read_file(*cache->io(), path),
                gltf_read_receiver{
                    .core = this, .resource = gltf_read_receiver::main_file, .token = stop_source.get_token() }) } };
        }
        catch (...)
        {
            finish(std::unexpected(avis::make_error(std::current_exception())));
            return;
        }
        stdexec::start(*main_read);
    }

    void gltf_load_core::request_stop() noexcept
    {
        stop_requested.store(true, std::memory_order_relaxed);
        stop_source.request_stop();
    }

    void gltf_load_core::hop(const gltf_stage stage) noexcept
    {
        auto& slot = hops[static_cast<std::size_t>(stage)];
        try
        {
            slot = std::unique_ptr<gltf_hop_operation>{ new gltf_hop_operation{ stdexec::connect(
                stdexec::schedule(stdexec::get_parallel_scheduler()),
                gltf_hop_receiver{ .core = this, .stage = stage }) } };
        }
        catch (...)
        {
            finish(std::unexpected(avis::make_error(std::current_exception())));
            return;
        }
        stdexec::start(*slot);
    }

    void gltf_load_core::run(const gltf_stage stage) noexcept
    {
        if (stop_requested.load(std::memory_order_relaxed))
        {
            finish_stopped();
            return;
        }
        if (stage == gltf_stage::parse)
        {
            run_parse();
            return;
        }

        // After the external reads. Then the first failure, which stopped the other reads; then a read that stopped
        // on its own (its IO context is stopping), since its data is missing.
        if (first_error)
        {
            finish(std::unexpected(std::move(*first_error)));
            return;
        }
        if (read_stopped.load(std::memory_order_relaxed))
        {
            finish_stopped();
            return;
        }
        run_prepare();
    }

    void gltf_load_core::run_parse() noexcept
    {
        result<std::optional<gltf_asset_set>> parsed;
        try
        {
            parsed = parse();
        }
        catch (...)
        {
            parsed = std::unexpected(avis::make_error(std::current_exception()));
        }
        if (!parsed)
        {
            finish(std::unexpected(std::move(parsed.error())));
            return;
        }
        if (*parsed)
        {
            finish(std::move(**parsed)); // deduplicated
            return;
        }
        if (resources.empty())
        {
            run_prepare();
            return;
        }
        start_reads();
    }

    void gltf_load_core::start_reads() noexcept
    {
        try
        {
            external.resize(resources.size());
            reads.reserve(resources.size());
            for (std::size_t index = 0; index < resources.size(); ++index)
            {
                reads.push_back(
                    std::unique_ptr<gltf_read_operation>{ new gltf_read_operation{ stdexec::connect(
                        io::read_file(*cache->io(), resources[index].path),
                        gltf_read_receiver{ .core = this, .resource = index, .token = stop_source.get_token() }) } });
            }
        }
        catch (...)
        {
            finish(std::unexpected(avis::make_error(std::current_exception())));
            return;
        }

        // One reference per read plus this one, so a read completing inline can't move on early
        pending.store(reads.size() + 1, std::memory_order_relaxed);
        for (const auto& read : reads)
        {
            stdexec::start(*read);
        }
        release_read();
    }

    void gltf_load_core::release_read() noexcept
    {
        if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            hop(gltf_stage::prepare);
        }
    }

    void gltf_load_core::fail(avis::error error) noexcept
    {
        {
            const std::scoped_lock lock{ error_mutex };
            if (first_error)
            {
                return;
            }
            first_error = std::move(error);
        }
        stop_source.request_stop(); // the other reads aren't needed any more
    }

    void gltf_load_core::run_prepare() noexcept
    {
        result<void> prepared;
        try
        {
            prepared = prepare();
        }
        catch (...)
        {
            prepared = std::unexpected(avis::make_error(std::current_exception()));
        }
        if (!prepared)
        {
            finish(std::unexpected(std::move(prepared.error())));
            return;
        }
        if (stop_requested.load(std::memory_order_relaxed))
        {
            finish_stopped();
            return;
        }
        if (images.empty())
        {
            finish(create_assets_or_release());
            return;
        }
        start_decodes();
    }

    void gltf_load_core::start_decodes() noexcept
    {
        try
        {
            decoded.resize(images.size());
            decodes = std::unique_ptr<gltf_decode_operation>{ new gltf_decode_operation{ stdexec::connect(
                stdexec::bulk(
                    stdexec::schedule(stdexec::get_parallel_scheduler()),
                    stdexec::par,
                    images.size(),
                    gltf_decode_item{ .core = this }),
                gltf_decode_receiver{ .core = this }) } };
        }
        catch (...)
        {
            finish(std::unexpected(avis::make_error(std::current_exception())));
            return;
        }
        stdexec::start(*decodes);
    }

    void gltf_load_core::decode(const std::size_t index) noexcept
    {
        {
            std::function<void(std::size_t)> hook;
            {
                const std::scoped_lock lock{ decode_hook_mutex };
                hook = decode_hook;
            }
            if (hook)
            {
                hook(index);
            }
        }
        if (stop_requested.load(std::memory_order_relaxed))
        {
            return; // left as nullopt; after_decodes completes with set_stopped
        }
        const gltf_image_request& request = images[index];
        try
        {
            decoded[index] = decode_image(request.bytes, request.srgb, request.description);
        }
        catch (...)
        {
            decoded[index] = std::unexpected(avis::make_error(std::current_exception()));
        }
    }

    void gltf_load_core::after_decodes() noexcept
    {
        if (stop_requested.load(std::memory_order_relaxed))
        {
            finish_stopped();
            return;
        }
        finish(create_assets_or_release());
    }

    void gltf_load_core::finish(result<gltf_asset_set> outcome) noexcept
    {
        owner->complete(std::move(outcome));
    }

    void gltf_load_core::finish_stopped() noexcept
    {
        owner->complete_stopped();
    }

    void gltf_load_core::warn(const std::string& message)
    {
        if (warned.insert(message).second)
        {
            log::get(logger_name)->warn("load_gltf '{}': {}", utf8(path), message);
        }
    }

    // ---- stage 2: hash, deduplicate, parse ----

    result<std::optional<gltf_asset_set>> gltf_load_core::parse()
    {
        hash = XXH3_64bits(file.data(), file.size());
        std::error_code ignored;
        canonical_path = std::filesystem::weakly_canonical(path, ignored);
        if (canonical_path.empty())
        {
            canonical_path = std::filesystem::absolute(path, ignored);
        }
        if (std::optional<gltf_asset_set> found = cache->find_gltf(canonical_path, hash))
        {
            found->path = path;
            return found;
        }

        auto data = fastgltf::GltfDataBuffer::FromBytes(file.data(), file.size());
        if (data.error() != fastgltf::Error::None)
        {
            return std::unexpected(parse_error(path, data.error()));
        }
        fastgltf::Parser parser{ supported_extensions };
        auto loaded = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::None);
        if (loaded.error() != fastgltf::Error::None)
        {
            return std::unexpected(parse_error(path, loaded.error()));
        }
        asset.emplace(std::move(loaded.get()));
        file = {}; // the GLB chunk was copied into the asset

        // Not supported in iteration 1 (08 §5.1)
        if (!asset->skins.empty() ||
            std::ranges::any_of(asset->nodes, [](const fastgltf::Node& node) { return node.skinIndex.has_value(); }))
        {
            return std::unexpected(unsupported(path, "skinning isn't supported"));
        }
        for (const fastgltf::Mesh& mesh : asset->meshes)
        {
            for (const fastgltf::Primitive& primitive : mesh.primitives)
            {
                if (!primitive.targets.empty())
                {
                    return std::unexpected(unsupported(
                        path, std::format("mesh '{}' has morph targets, which aren't supported", mesh.name)));
                }
            }
        }
        for (const auto& used : asset->extensionsUsed)
        {
            if (std::ranges::find(supported_extension_names, std::string_view{ used }) ==
                supported_extension_names.end())
            {
                warn(std::format("the extension {} isn't supported and is ignored", std::string_view{ used }));
            }
        }
        if (!asset->animations.empty())
        {
            warn("animations aren't supported and are ignored");
        }
        if (!asset->cameras.empty())
        {
            warn("cameras aren't supported and are ignored");
        }

        // The external files: every buffer given by a URI, and the images of the materials' textures
        const std::filesystem::path directory = path.parent_path();
        const auto add_resource =
            [&](const bool buffer, const std::size_t index, const fastgltf::sources::URI& uri) -> result<void>
        {
            if (!uri.uri.isLocalPath())
            {
                return std::unexpected(unsupported(
                    path,
                    std::format(
                        "{} {}: '{}' isn't a local file", buffer ? "buffer" : "image", index, uri.uri.string())));
            }
            resources.push_back(
                { .buffer = buffer,
                  .index = index,
                  .offset = uri.fileByteOffset,
                  .path = directory / uri.uri.fspath() });
            return {};
        };
        for (std::size_t index = 0; index < asset->buffers.size(); ++index)
        {
            if (const auto* uri = std::get_if<fastgltf::sources::URI>(&asset->buffers[index].data))
            {
                if (result<void> added = add_resource(true, index, *uri); !added)
                {
                    return std::unexpected(std::move(added.error()));
                }
            }
        }

        std::vector<bool> image_used(asset->images.size(), false);
        const auto use = [&](const auto& info)
        {
            if (info.has_value() && info->textureIndex < asset->textures.size())
            {
                const auto& image = asset->textures[info->textureIndex].imageIndex;
                if (image.has_value() && *image < image_used.size())
                {
                    image_used[*image] = true;
                }
            }
        };
        for (const fastgltf::Material& material : asset->materials)
        {
            use(material.pbrData.baseColorTexture);
            use(material.pbrData.metallicRoughnessTexture);
            use(material.normalTexture);
            use(material.occlusionTexture);
            use(material.emissiveTexture);
        }
        for (std::size_t index = 0; index < asset->images.size(); ++index)
        {
            const auto* uri = std::get_if<fastgltf::sources::URI>(&asset->images[index].data);
            if (image_used[index] && uri != nullptr)
            {
                if (result<void> added = add_resource(false, index, *uri); !added)
                {
                    return std::unexpected(std::move(added.error()));
                }
            }
        }
        return std::optional<gltf_asset_set>{};
    }

    // ---- stage 3: attach, check, convert ----

    result<void> gltf_load_core::prepare()
    {
        if (result<void> attached = attach_external_bytes(); !attached)
        {
            return attached;
        }
        if (result<void> checked = check_ranges(); !checked)
        {
            return checked;
        }
        if (const fastgltf::Error validation = fastgltf::validate(*asset); validation != fastgltf::Error::None)
        {
            return std::unexpected(
                invalid(path, std::format("validation failed: {}", fastgltf::getErrorMessage(validation))));
        }
        if (result<void> converted = convert_materials(); !converted)
        {
            return converted;
        }
        if (result<void> converted = convert_meshes(); !converted)
        {
            return converted;
        }
        return convert_nodes();
    }

    result<void> gltf_load_core::attach_external_bytes()
    {
        image_bytes.assign(asset->images.size(), {});
        for (std::size_t index = 0; index < resources.size(); ++index)
        {
            const gltf_external_resource& resource = resources[index];
            const std::span<const std::byte> bytes{ external[index] };
            if (resource.offset > bytes.size())
            {
                return std::unexpected(invalid(
                    path,
                    std::format("'{}' is shorter than its byte offset {}", utf8(resource.path), resource.offset)));
            }
            const std::span<const std::byte> data = bytes.subspan(resource.offset);
            if (resource.buffer)
            {
                // The bytes stay in `external`, which outlives the asset's use
                asset->buffers[resource.index].data = fastgltf::sources::ByteView{ .bytes = data };
            }
            else
            {
                image_bytes[resource.index] = data;
            }
        }
        return {};
    }

    // fastgltf::validate checks each accessor against its buffer view, but not the buffer views against the buffers,
    // nor the buffers against the bytes that were loaded
    result<void> gltf_load_core::check_ranges() const
    {
        std::vector<std::size_t> loaded(asset->buffers.size(), 0);
        for (std::size_t index = 0; index < asset->buffers.size(); ++index)
        {
            const fastgltf::Buffer& buffer = asset->buffers[index];
            const std::size_t size = std::visit(
                [](const auto& source) -> std::size_t
                {
                    using source_type = std::remove_cvref_t<decltype(source)>;
                    if constexpr (std::is_same_v<source_type, fastgltf::sources::Array>)
                    {
                        return source.bytes.size_bytes();
                    }
                    else if constexpr (std::is_same_v<source_type, fastgltf::sources::ByteView>)
                    {
                        return source.bytes.size();
                    }
                    else if constexpr (std::is_same_v<source_type, fastgltf::sources::Vector>)
                    {
                        return source.bytes.size();
                    }
                    else
                    {
                        return 0;
                    }
                },
                buffer.data);
            if (size < buffer.byteLength)
            {
                return std::unexpected(invalid(
                    path, std::format("buffer {} has {} bytes, its byteLength is {}", index, size, buffer.byteLength)));
            }
            loaded[index] = buffer.byteLength;
        }

        for (std::size_t index = 0; index < asset->bufferViews.size(); ++index)
        {
            const fastgltf::BufferView& view = asset->bufferViews[index];
            if (view.bufferIndex >= loaded.size() || view.byteOffset > loaded[view.bufferIndex] ||
                view.byteLength > loaded[view.bufferIndex] - view.byteOffset)
            {
                return std::unexpected(invalid(path, std::format("buffer view {} lies outside its buffer", index)));
            }
        }

        for (std::size_t index = 0; index < asset->accessors.size(); ++index)
        {
            const fastgltf::Accessor& accessor = asset->accessors[index];
            if (!accessor.sparse)
            {
                continue;
            }
            const fastgltf::SparseAccessor& sparse = *accessor.sparse;
            const auto fits = [&](const std::size_t view, const std::size_t offset, const std::size_t bytes)
            {
                return view < asset->bufferViews.size() && offset <= asset->bufferViews[view].byteLength &&
                       bytes <= asset->bufferViews[view].byteLength - offset;
            };
            const std::size_t index_bytes =
                sparse.count * fastgltf::getElementByteSize(fastgltf::AccessorType::Scalar, sparse.indexComponentType);
            const std::size_t value_bytes =
                sparse.count * fastgltf::getElementByteSize(accessor.type, accessor.componentType);
            if (!fits(sparse.indicesBufferView, sparse.indicesByteOffset, index_bytes) ||
                !fits(sparse.valuesBufferView, sparse.valuesByteOffset, value_bytes))
            {
                return std::unexpected(
                    invalid(path, std::format("accessor {}: sparse data lies outside its views", index)));
            }
        }
        return {};
    }

    result<std::optional<std::size_t>> gltf_load_core::request_image(const std::size_t texture, const bool srgb)
    {
        if (texture >= asset->textures.size())
        {
            return std::unexpected(invalid(path, std::format("texture {} doesn't exist", texture)));
        }
        const auto& image_index = asset->textures[texture].imageIndex;
        if (!image_index.has_value())
        {
            warn(
                std::format(
                    "texture {} has no PNG or JPEG image (DDS and KTX2 aren't supported); it's ignored", texture));
            return std::optional<std::size_t>{};
        }
        const std::size_t image = *image_index;
        if (image >= asset->images.size())
        {
            return std::unexpected(invalid(path, std::format("texture {}: image {} doesn't exist", texture, image)));
        }

        for (std::size_t request = 0; request < images.size(); ++request)
        {
            if (images[request].image == image && images[request].srgb == srgb)
            {
                return std::optional<std::size_t>{ request };
            }
        }

        const fastgltf::Image& source = asset->images[image];
        std::span<const std::byte> bytes;
        fastgltf::MimeType mime = fastgltf::MimeType::None;
        std::string origin;
        if (const auto* array = std::get_if<fastgltf::sources::Array>(&source.data))
        {
            bytes = { array->bytes.data(), array->bytes.size_bytes() };
            mime = array->mimeType;
            origin = "a data URI";
        }
        else if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&source.data))
        {
            if (view->bufferViewIndex >= asset->bufferViews.size())
            {
                return std::unexpected(
                    invalid(path, std::format("image {}: buffer view {} doesn't exist", image, view->bufferViewIndex)));
            }
            fastgltf::DefaultBufferDataAdapter adapter; // the buffers were checked in check_ranges
            bytes = adapter(*asset, view->bufferViewIndex);
            mime = view->mimeType;
            origin = std::format("buffer view {}", view->bufferViewIndex);
        }
        else if (const auto* uri = std::get_if<fastgltf::sources::URI>(&source.data))
        {
            bytes = image_bytes[image];
            mime = uri->mimeType;
            origin = std::format("'{}'", uri->uri.string());
        }
        else
        {
            warn(std::format("image {} has no data it can read; it's ignored", image));
            return std::optional<std::size_t>{};
        }
        if (const std::string_view unsupported_format = mime_name(mime); !unsupported_format.empty())
        {
            warn(std::format("image {} is {}, which isn't supported; it's ignored", image, unsupported_format));
            return std::optional<std::size_t>{};
        }

        images.push_back(
            { .image = image,
              .srgb = srgb,
              .bytes = bytes,
              .description = std::format(
                  "image {}{} ({}) of '{}'",
                  image,
                  source.name.empty() ? std::string{} : std::format(" '{}'", text(source.name)),
                  origin,
                  utf8(path)) });
        return std::optional<std::size_t>{ images.size() - 1 };
    }

    result<void> gltf_load_core::convert_materials()
    {
        materials.reserve(asset->materials.size() + 1);
        for (std::size_t index = 0; index < asset->materials.size(); ++index)
        {
            const fastgltf::Material& source = asset->materials[index];
            gltf_pending_material converted;
            material_description& description = converted.description;
            const auto& factor = source.pbrData.baseColorFactor;
            description.base_color_factor = {
                .red = factor[0], .green = factor[1], .blue = factor[2], .alpha = factor[3]
            };
            description.metallic_factor = source.pbrData.metallicFactor;
            description.roughness_factor = source.pbrData.roughnessFactor;
            description.normal_scale = source.normalTexture ? source.normalTexture->scale : 1.0f;
            description.occlusion_strength = source.occlusionTexture ? source.occlusionTexture->strength : 1.0f;
            description.emissive_factor =
                Eigen::Vector3f{ source.emissiveFactor[0], source.emissiveFactor[1], source.emissiveFactor[2] } *
                source.emissiveStrength;
            description.alpha_cutoff = source.alphaCutoff;
            description.alpha = source.alphaMode == fastgltf::AlphaMode::Mask  ? alpha_mode::mask :
                                source.alphaMode == fastgltf::AlphaMode::Blend ? alpha_mode::blend :
                                                                                 alpha_mode::opaque;
            description.double_sided = source.doubleSided;
            description.shading = source.unlit ? shading_model::unlit : shading_model::lit;
            description.debug_name = source.name.empty() ? std::format("material {}", index) : text(source.name);

            const auto texture = [&](const auto& info, const bool srgb, const std::size_t slot) -> result<void>
            {
                if (!info.has_value())
                {
                    return {};
                }
                if (info->texCoordIndex != 0)
                {
                    warn(
                        std::format(
                            "material '{}' uses TEXCOORD_{}; only TEXCOORD_0 is supported",
                            description.debug_name,
                            info->texCoordIndex));
                }
                result<std::optional<std::size_t>> request = request_image(info->textureIndex, srgb);
                if (!request)
                {
                    return std::unexpected(std::move(request.error()));
                }
                converted.textures[slot] = *request;
                return {};
            };
            for (result<void> added : { texture(source.pbrData.baseColorTexture, true, 0),
                                        texture(source.pbrData.metallicRoughnessTexture, false, 1),
                                        texture(source.normalTexture, false, 2),
                                        texture(source.occlusionTexture, false, 3),
                                        texture(source.emissiveTexture, true, 4) })
            {
                if (!added)
                {
                    return added;
                }
            }
            materials.push_back(std::move(converted));
        }
        return {};
    }

    result<void> gltf_load_core::convert_meshes()
    {
        using fastgltf::AccessorType;
        using fastgltf::math::fvec2;
        using fastgltf::math::fvec3;
        using fastgltf::math::fvec4;

        // The default material (glTF 2.0 §3.9.6), created once when a primitive has none
        const auto default_material_index = [&]() -> std::size_t
        {
            if (!default_material)
            {
                gltf_pending_material fallback;
                fallback.description.debug_name = "default";
                materials.push_back(std::move(fallback));
                default_material = materials.size() - 1;
            }
            return *default_material;
        };

        // The uv transform to bake for a material, or none. Baking changes the uvs of every texture of the
        // primitive, so it's only correct when they all use the same transform.
        const auto bake_for = [&](const std::size_t material_index) -> std::optional<uv_transform>
        {
            if (material_index >= asset->materials.size())
            {
                return std::nullopt;
            }
            const fastgltf::Material& material = asset->materials[material_index];
            const fastgltf::TextureInfo* const textures[] = {
                material.pbrData.baseColorTexture ? &*material.pbrData.baseColorTexture : nullptr,
                material.pbrData.metallicRoughnessTexture ? &*material.pbrData.metallicRoughnessTexture : nullptr,
                material.normalTexture ? &*material.normalTexture : nullptr,
                material.occlusionTexture ? &*material.occlusionTexture : nullptr,
                material.emissiveTexture ? &*material.emissiveTexture : nullptr,
            };
            const fastgltf::TextureInfo* const base = textures[0];
            const bool others_transformed = std::any_of(
                std::begin(textures) + 1,
                std::end(textures),
                [](const fastgltf::TextureInfo* info) { return info != nullptr && info->transform != nullptr; });
            const std::string name =
                material.name.empty() ? std::format("material {}", material_index) : text(material.name);
            if (base == nullptr || base->transform == nullptr)
            {
                if (others_transformed)
                {
                    warn(
                        std::format(
                            "material '{}': KHR_texture_transform is only supported on the base colour texture; "
                            "ignored",
                            name));
                }
                return std::nullopt;
            }

            const fastgltf::TextureTransform& wanted = *base->transform;
            if (wanted.rotation != 0.0f)
            {
                warn(std::format("material '{}': KHR_texture_transform rotations aren't supported; ignored", name));
                return std::nullopt;
            }
            for (const fastgltf::TextureInfo* info : textures)
            {
                const bool same = info == nullptr || (info->transform != nullptr && info->transform->rotation == 0.0f &&
                                                      info->transform->uvOffset == wanted.uvOffset &&
                                                      info->transform->uvScale == wanted.uvScale);
                if (!same)
                {
                    warn(
                        std::format(
                            "material '{}': its textures use different KHR_texture_transforms, which can't be baked "
                            "into "
                            "the uvs; ignored",
                            name));
                    return std::nullopt;
                }
            }
            return uv_transform{ .offset = { wanted.uvOffset[0], wanted.uvOffset[1] },
                                 .scale = { wanted.uvScale[0], wanted.uvScale[1] } };
        };

        const auto has_normal_texture = [&](const std::size_t material_index)
        {
            return material_index < asset->materials.size() &&
                   asset->materials[material_index].normalTexture.has_value();
        };

        meshes.reserve(asset->meshes.size());
        for (std::size_t mesh_index = 0; mesh_index < asset->meshes.size(); ++mesh_index)
        {
            const fastgltf::Mesh& mesh = asset->meshes[mesh_index];
            gltf_pending_mesh pending;
            pending.name = mesh.name.empty() ? std::format("mesh {}", mesh_index) : text(mesh.name);
            const std::string_view name = pending.name;
            if (mesh.primitives.empty())
            {
                return std::unexpected(invalid(path, std::format("mesh '{}' has no primitives", name)));
            }

            std::vector<primitive_streams> primitives;
            primitives.reserve(mesh.primitives.size());
            for (std::size_t primitive_index = 0; primitive_index < mesh.primitives.size(); ++primitive_index)
            {
                const fastgltf::Primitive& primitive = mesh.primitives[primitive_index];
                const std::string what = std::format("mesh '{}', primitive {}", name, primitive_index);
                primitive_streams streams;
                streams.topology = topology_of(primitive.type);
                if (!primitives.empty() && primitives.front().topology != streams.topology)
                {
                    return std::unexpected(unsupported(
                        path,
                        std::format(
                            "mesh '{}' mixes {} and {}; a mesh has one topology",
                            name,
                            topology_name(primitives.front().topology),
                            topology_name(streams.topology))));
                }

                const auto* position = primitive.findAttribute("POSITION");
                if (position == primitive.attributes.end())
                {
                    return std::unexpected(invalid(path, std::format("{} has no POSITION", what)));
                }
                auto positions = read_accessor<fvec3>(*asset, path, position->accessorIndex, AccessorType::Vec3, what);
                if (!positions)
                {
                    return std::unexpected(std::move(positions.error()));
                }
                const std::size_t vertex_count = positions->size();
                if (vertex_count > std::numeric_limits<std::uint32_t>::max())
                {
                    return std::unexpected(unsupported(path, std::format("{} has more than 2³² vertices", what)));
                }
                streams.positions.reserve(vertex_count);
                for (const fvec3& value : *positions)
                {
                    streams.positions.push_back(to_avis(value));
                }

                const auto check_count = [&](const std::size_t count, const std::string_view attribute) -> result<void>
                {
                    if (count != vertex_count)
                    {
                        return std::unexpected(invalid(
                            path,
                            std::format(
                                "{}: {} has {} elements, POSITION has {}", what, attribute, count, vertex_count)));
                    }
                    return {};
                };

                if (const auto* normal = primitive.findAttribute("NORMAL"); normal != primitive.attributes.end())
                {
                    auto normals = read_accessor<fvec3>(*asset, path, normal->accessorIndex, AccessorType::Vec3, what);
                    if (!normals)
                    {
                        return std::unexpected(std::move(normals.error()));
                    }
                    if (result<void> counted = check_count(normals->size(), "NORMAL"); !counted)
                    {
                        return std::unexpected(std::move(counted.error()));
                    }
                    streams.normals.reserve(vertex_count);
                    for (const fvec3& value : *normals)
                    {
                        streams.normals.push_back(to_avis(value));
                    }
                }
                if (const auto* tangent = primitive.findAttribute("TANGENT"); tangent != primitive.attributes.end())
                {
                    auto tangents =
                        read_accessor<fvec4>(*asset, path, tangent->accessorIndex, AccessorType::Vec4, what);
                    if (!tangents)
                    {
                        return std::unexpected(std::move(tangents.error()));
                    }
                    if (result<void> counted = check_count(tangents->size(), "TANGENT"); !counted)
                    {
                        return std::unexpected(std::move(counted.error()));
                    }
                    streams.tangents.reserve(vertex_count);
                    for (const fvec4& value : *tangents)
                    {
                        const Eigen::Vector3f direction = to_avis(value[0], value[1], value[2]);
                        streams.tangents.emplace_back(direction.x(), direction.y(), direction.z(), value[3]);
                    }
                }
                if (const auto* uv = primitive.findAttribute("TEXCOORD_0"); uv != primitive.attributes.end())
                {
                    auto uvs = read_accessor<fvec2>(*asset, path, uv->accessorIndex, AccessorType::Vec2, what);
                    if (!uvs)
                    {
                        return std::unexpected(std::move(uvs.error()));
                    }
                    if (result<void> counted = check_count(uvs->size(), "TEXCOORD_0"); !counted)
                    {
                        return std::unexpected(std::move(counted.error()));
                    }
                    streams.uvs.reserve(vertex_count);
                    for (const fvec2& value : *uvs)
                    {
                        streams.uvs.emplace_back(value[0], value[1]);
                    }
                }
                if (const auto* color = primitive.findAttribute("COLOR_0"); color != primitive.attributes.end())
                {
                    // Linear in glTF, sRGB u8x4 in Avis; alpha stays linear
                    const std::size_t accessor = color->accessorIndex;
                    const bool rgba =
                        accessor < asset->accessors.size() && asset->accessors[accessor].type == AccessorType::Vec4;
                    std::vector<fvec4> colors;
                    if (rgba)
                    {
                        auto read = read_accessor<fvec4>(*asset, path, accessor, AccessorType::Vec4, what);
                        if (!read)
                        {
                            return std::unexpected(std::move(read.error()));
                        }
                        colors = std::move(*read);
                    }
                    else
                    {
                        auto read = read_accessor<fvec3>(*asset, path, accessor, AccessorType::Vec3, what);
                        if (!read)
                        {
                            return std::unexpected(std::move(read.error()));
                        }
                        colors.reserve(read->size());
                        for (const fvec3& value : *read)
                        {
                            colors.emplace_back(value[0], value[1], value[2], 1.0f);
                        }
                    }
                    if (result<void> counted = check_count(colors.size(), "COLOR_0"); !counted)
                    {
                        return std::unexpected(std::move(counted.error()));
                    }
                    streams.colors.reserve(vertex_count);
                    for (const fvec4& value : colors)
                    {
                        streams.colors.push_back(
                            { linear_to_srgb8(value[0]),
                              linear_to_srgb8(value[1]),
                              linear_to_srgb8(value[2]),
                              linear_to_u8(value[3]) });
                    }
                }

                if (primitive.indicesAccessor.has_value())
                {
                    const std::size_t accessor = *primitive.indicesAccessor;
                    if (accessor < asset->accessors.size() &&
                        asset->accessors[accessor].componentType != fastgltf::ComponentType::UnsignedByte &&
                        asset->accessors[accessor].componentType != fastgltf::ComponentType::UnsignedShort &&
                        asset->accessors[accessor].componentType != fastgltf::ComponentType::UnsignedInt)
                    {
                        return std::unexpected(
                            invalid(path, std::format("{}: the indices aren't unsigned integers", what)));
                    }
                    auto indices = read_accessor<std::uint32_t>(*asset, path, accessor, AccessorType::Scalar, what);
                    if (!indices)
                    {
                        return std::unexpected(std::move(indices.error()));
                    }
                    for (const std::uint32_t index : *indices)
                    {
                        if (index >= vertex_count)
                        {
                            return std::unexpected(invalid(
                                path,
                                std::format("{}: index {} is out of range ({} vertices)", what, index, vertex_count)));
                        }
                    }
                    streams.indices = std::move(*indices);
                    streams.indexed = true;
                }

                switch (primitive.type)
                {
                case fastgltf::PrimitiveType::TriangleStrip:
                case fastgltf::PrimitiveType::TriangleFan:
                case fastgltf::PrimitiveType::LineStrip:
                case fastgltf::PrimitiveType::LineLoop:
                    convert_to_list(streams, primitive.type);
                    break;
                case fastgltf::PrimitiveType::Triangles:
                case fastgltf::PrimitiveType::Lines:
                    {
                        // A trailing incomplete triangle or line is dropped
                        const std::size_t per = primitive.type == fastgltf::PrimitiveType::Triangles ? 3 : 2;
                        if (streams.indexed)
                        {
                            streams.indices.resize(streams.indices.size() - streams.indices.size() % per);
                        }
                        else if (vertex_count % per != 0)
                        {
                            convert_to_list(streams, fastgltf::PrimitiveType::Points); // index them, then drop the rest
                            streams.indices.resize(streams.indices.size() - streams.indices.size() % per);
                        }
                        break;
                    }
                default:
                    break;
                }

                if (streams.topology == mesh_topology::triangles && streams.normals.empty())
                {
                    generate_flat_normals(streams);
                }

                const std::size_t material_index =
                    primitive.materialIndex.has_value() ? *primitive.materialIndex : default_material_index();
                if (primitive.materialIndex.has_value() && material_index >= asset->materials.size())
                {
                    return std::unexpected(
                        invalid(path, std::format("{}: material {} doesn't exist", what, material_index)));
                }
                if (const std::optional<uv_transform> bake = bake_for(material_index); bake && !streams.uvs.empty())
                {
                    for (Eigen::Vector2f& uv : streams.uvs)
                    {
                        uv = bake->offset + bake->scale.cwiseProduct(uv);
                    }
                }
                pending.materials.push_back(material_index);
                primitives.push_back(std::move(streams));
            }

            // One attribute set for the mesh. Triangles always have normals (generated when missing), and tangents
            // when a primitive has them or its material has a normal texture; uvs and colours when any primitive
            // has them, with zeros and white elsewhere. Lines and points keep normals and tangents only when every
            // primitive has them.
            const mesh_topology topology = primitives.front().topology;
            const bool triangles = topology == mesh_topology::triangles;
            const auto any = [&](const auto member)
            {
                return std::ranges::any_of(
                    primitives, [&](const primitive_streams& p) { return !(p.*member).empty(); });
            };
            const auto all = [&](const auto member)
            {
                return std::ranges::all_of(
                    primitives, [&](const primitive_streams& p) { return !(p.*member).empty(); });
            };

            bool wants_tangents = false;
            for (std::size_t at = 0; at < primitives.size(); ++at)
            {
                wants_tangents =
                    wants_tangents || !primitives[at].tangents.empty() || has_normal_texture(pending.materials[at]);
            }
            const bool with_normals = triangles || all(&primitive_streams::normals);
            const bool with_tangents = triangles ? wants_tangents : all(&primitive_streams::tangents);
            const bool with_uvs = any(&primitive_streams::uvs);
            const bool with_colors = any(&primitive_streams::colors);

            mesh_data& data = pending.data;
            data.topology = topology;
            if (with_normals)
            {
                data.attributes |= vertex_attribute::normal;
            }
            if (with_tangents)
            {
                data.attributes |= vertex_attribute::tangent;
            }
            if (with_uvs)
            {
                data.attributes |= vertex_attribute::uv0;
            }
            if (with_colors)
            {
                data.attributes |= vertex_attribute::color0;
            }

            for (primitive_streams& streams : primitives)
            {
                if (with_tangents && streams.tangents.empty())
                {
                    generate_tangents(streams);
                }

                mesh_primitive range;
                range.vertex_first = static_cast<std::uint32_t>(data.positions.size());
                range.vertex_count = static_cast<std::uint32_t>(streams.positions.size());
                range.index_first = static_cast<std::uint32_t>(data.indices.size());
                range.index_count = streams.indexed ? static_cast<std::uint32_t>(streams.indices.size()) : 0;
                data.primitives.push_back(range);

                data.positions.insert(data.positions.end(), streams.positions.begin(), streams.positions.end());
                const std::size_t count = streams.positions.size();
                for (std::size_t vertex = 0; vertex < count; ++vertex)
                {
                    if (with_normals)
                    {
                        data.normals.push_back(encode_octahedral(streams.normals[vertex]));
                    }
                    if (with_tangents)
                    {
                        const Eigen::Vector4f& tangent = streams.tangents[vertex];
                        data.tangents.push_back(encode_octahedral(tangent.head<3>()));
                        data.tangent_signs.push_back(tangent.w() < 0.0f ? std::int8_t{ -1 } : std::int8_t{ 1 });
                    }
                    if (with_uvs)
                    {
                        const Eigen::Vector2f uv = streams.uvs.empty() ? Eigen::Vector2f::Zero() : streams.uvs[vertex];
                        data.uv0.push_back({ encode_f16(uv.x()), encode_f16(uv.y()) });
                    }
                    if (with_colors)
                    {
                        data.color0.push_back(
                            streams.colors.empty() ? std::array<std::uint8_t, 4>{ 255, 255, 255, 255 } :
                                                     streams.colors[vertex]);
                    }
                }
                if (streams.indexed)
                {
                    data.indices.insert(data.indices.end(), streams.indices.begin(), streams.indices.end());
                }
            }
            meshes.push_back(std::move(pending));
        }
        return {};
    }

    result<void> gltf_load_core::convert_nodes()
    {
        const std::size_t count = asset->nodes.size();
        std::vector<std::size_t> roots;
        if (!asset->scenes.empty())
        {
            const std::size_t scene = asset->defaultScene.has_value() ? *asset->defaultScene : 0;
            if (scene >= asset->scenes.size())
            {
                return std::unexpected(invalid(path, std::format("the default scene {} doesn't exist", scene)));
            }
            roots.assign(asset->scenes[scene].nodeIndices.begin(), asset->scenes[scene].nodeIndices.end());
        }
        else
        {
            // No scene: every node without a parent
            std::vector<bool> child(count, false);
            for (const fastgltf::Node& node : asset->nodes)
            {
                for (const std::size_t at : node.children)
                {
                    if (at < count)
                    {
                        child[at] = true;
                    }
                }
            }
            for (std::size_t at = 0; at < count; ++at)
            {
                if (!child[at])
                {
                    roots.push_back(at);
                }
            }
        }

        // Depth first, parents before children, children in their glTF order
        std::vector<bool> visited(count, false);
        std::vector<std::pair<std::size_t, std::int32_t>> stack; // node, parent's position in `nodes`
        for (auto root = roots.rbegin(); root != roots.rend(); ++root)
        {
            stack.emplace_back(*root, -1);
        }
        while (!stack.empty())
        {
            const auto [index, parent] = stack.back();
            stack.pop_back();
            if (index >= count)
            {
                return std::unexpected(invalid(path, std::format("node {} doesn't exist", index)));
            }
            if (visited[index])
            {
                return std::unexpected(invalid(path, std::format("node {} has two parents or is in a cycle", index)));
            }
            visited[index] = true;

            const fastgltf::Node& node = asset->nodes[index];
            gltf_pending_node converted;
            converted.name = node.name.empty() ? std::format("node {}", index) : text(node.name);
            converted.local = node_transform(node);
            converted.parent = parent;
            if (node.meshIndex.has_value())
            {
                if (*node.meshIndex >= meshes.size())
                {
                    return std::unexpected(
                        invalid(path, std::format("node {}: mesh {} doesn't exist", index, *node.meshIndex)));
                }
                converted.mesh = *node.meshIndex;
            }
            nodes.push_back(std::move(converted));

            const auto position = static_cast<std::int32_t>(nodes.size() - 1);
            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
            {
                stack.emplace_back(*child, position);
            }
        }
        return {};
    }

    // ---- stage 4: create ----

    result<void> gltf_load_core::create_assets(gltf_asset_set& created)
    {
        created.path = path;

        std::vector<texture_handle> textures(images.size());
        for (std::size_t index = 0; index < images.size(); ++index)
        {
            std::optional<result<texture_data>>& image = decoded[index];
            if (!image.has_value())
            {
                return std::unexpected(make_error(generic_error_kind::cancelled, "load_gltf: an image wasn't decoded"));
            }
            if (!*image)
            {
                return std::unexpected(std::move(image->error()));
            }
        }
        for (std::size_t index = 0; index < images.size(); ++index)
        {
            result<texture_handle> texture = cache->create_texture(std::move(**decoded[index]));
            if (!texture)
            {
                return std::unexpected(std::move(texture.error()));
            }
            created.textures.push_back(*texture);
            textures[index] = *texture;
        }
        decoded.clear();

        std::vector<material_handle> material_handles(materials.size());
        for (std::size_t index = 0; index < materials.size(); ++index)
        {
            gltf_pending_material& pending = materials[index];
            material_description& description = pending.description;
            texture_handle* const slots[] = { &description.base_color_texture,
                                              &description.metallic_roughness_texture,
                                              &description.normal_texture,
                                              &description.occlusion_texture,
                                              &description.emissive_texture };
            for (std::size_t slot = 0; slot < pending.textures.size(); ++slot)
            {
                if (pending.textures[slot])
                {
                    *slots[slot] = textures[*pending.textures[slot]];
                }
            }
            result<material_handle> material = cache->create_material(std::move(description));
            if (!material)
            {
                return std::unexpected(std::move(material.error()));
            }
            created.materials.push_back(*material);
            material_handles[index] = *material;
        }

        std::vector<mesh_handle> mesh_handles(meshes.size());
        for (std::size_t index = 0; index < meshes.size(); ++index)
        {
            result<mesh_handle> mesh = cache->create_mesh(std::move(meshes[index].data));
            if (!mesh)
            {
                return std::unexpected(
                    invalid(path, std::format("mesh '{}': {}", meshes[index].name, mesh.error().message)));
            }
            created.meshes.push_back(*mesh);
            mesh_handles[index] = *mesh;
        }

        created.nodes.reserve(nodes.size());
        for (gltf_pending_node& pending : nodes)
        {
            gltf_node node;
            node.name = std::move(pending.name);
            node.local = pending.local;
            node.parent = pending.parent;
            if (pending.mesh)
            {
                node.mesh = mesh_handles[*pending.mesh];
                for (const std::size_t material : meshes[*pending.mesh].materials)
                {
                    node.materials.push_back(material_handles[material]);
                }
            }
            created.nodes.push_back(std::move(node));
        }
        return {};
    }

    result<gltf_asset_set> gltf_load_core::create_assets_or_release() noexcept
    {
        gltf_asset_set created;
        result<void> outcome;
        try
        {
            outcome = create_assets(created);
        }
        catch (...)
        {
            outcome = std::unexpected(avis::make_error(std::current_exception()));
        }
        if (!outcome)
        {
            cache->release_all(created);
            return std::unexpected(std::move(outcome.error()));
        }

        try
        {
            cache->remember_gltf(canonical_path, hash, created);
        }
        catch (...)
        {
            // Not remembered: the next load parses again
        }
        return created;
    }

    // ---- gltf_load_operation ----

    gltf_load_operation::gltf_load_operation(asset_cache& cache, std::filesystem::path path) :
        core_{ std::make_unique<gltf_load_core>(*this, cache, std::move(path)) }
    {}

    gltf_load_operation::~gltf_load_operation() = default;

    void gltf_load_operation::begin() noexcept
    {
        core_->begin();
    }

    void gltf_load_operation::request_stop() noexcept
    {
        core_->request_stop();
    }
} // namespace avis::scene::detail
