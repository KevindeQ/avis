#include "representation/shared_assets.h"

#include "avis/scene/assets/mesh_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace avis::visualizer::representation
{
    namespace
    {
        using Eigen::Vector3f;
        using srgb8 = std::array<std::uint8_t, 4>;

        // Adds vertices and indices to the builder's open primitive, keeping the index base
        class primitive_writer
        {
        public:
            explicit primitive_writer(scene::mesh_builder& builder) : builder_{ builder } {}

            // A flat quad a, b, c, d (in order around it), facing away from `inside`
            void quad(Vector3f a, Vector3f b, Vector3f c, Vector3f d, const Vector3f& inside)
            {
                Vector3f normal = (b - a).cross(c - a);
                if (normal.dot((a + b + c + d) / 4.0f - inside) < 0.0f)
                {
                    std::swap(b, d); // counter-clockwise seen from outside (glTF's front face)
                    normal = -normal;
                }
                normal.normalize();
                const std::uint32_t first = next_;
                for (const Vector3f& corner : { a, b, c, d })
                {
                    builder_.add_vertex(corner).normal(normal);
                }
                next_ += 4;
                for (const std::uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
                {
                    builder_.add_index(first + index);
                }
            }

            // Six faces between a bottom and a top rectangle (corners counter-clockwise from above, same order)
            void prism(const std::array<Vector3f, 4>& bottom, const std::array<Vector3f, 4>& top)
            {
                Vector3f centre = Vector3f::Zero();
                for (std::size_t corner = 0; corner < 4; ++corner)
                {
                    centre += bottom[corner] + top[corner];
                }
                centre /= 8.0f;
                quad(bottom[0], bottom[1], bottom[2], bottom[3], centre);
                quad(top[0], top[1], top[2], top[3], centre);
                for (std::size_t side = 0; side < 4; ++side)
                {
                    const std::size_t next = (side + 1) % 4;
                    quad(bottom[side], bottom[next], top[next], top[side], centre);
                }
            }

            void box(const Vector3f& low, const Vector3f& high)
            {
                prism(
                    rectangle(low.x(), high.x(), low.y(), high.y(), low.z()),
                    rectangle(low.x(), high.x(), low.y(), high.y(), high.z()));
            }

            static std::array<Vector3f, 4>
                rectangle(const float x0, const float x1, const float y0, const float y1, const float z)
            {
                return { Vector3f{ x0, y0, z }, Vector3f{ x1, y0, z }, Vector3f{ x1, y1, z }, Vector3f{ x0, y1, z } };
            }

            // A line segment with a colour per end
            void line(const Vector3f& a, const srgb8 colour_a, const Vector3f& b, const srgb8 colour_b)
            {
                builder_.add_vertex(a).color(colour_a);
                builder_.add_vertex(b).color(colour_b);
                builder_.add_index(next_).add_index(next_ + 1);
                next_ += 2;
            }

            void end_primitive()
            {
                builder_.end_primitive();
                next_ = 0;
            }

        private:
            scene::mesh_builder& builder_;
            std::uint32_t next_ = 0;
        };

        // Fades a grid line's alpha from `alpha` at the origin to 0 at `radius` (smoothstep)
        std::uint8_t fade(const float alpha, const Vector3f& point, const float radius) noexcept
        {
            const float t = std::clamp(point.norm() / radius, 0.0f, 1.0f);
            const float weight = 1.0f - t * t * (3.0f - 2.0f * t);
            return static_cast<std::uint8_t>(std::lround(255.0f * alpha * weight));
        }

        // Lattice lines every `spacing` out to `radius`, each split at every crossing so the alpha fades along it
        void add_grid_lines(
            primitive_writer& writer,
            const float spacing,
            const float radius,
            const std::uint8_t grey,
            const float alpha,
            const float skip_every)
        {
            const int count = static_cast<int>(std::lround(radius / spacing));
            const auto skipped = [&](const int line)
            {
                if (skip_every <= 0.0f)
                {
                    return false;
                }
                const float position = static_cast<float>(line) * spacing;
                return std::fmod(std::abs(position), skip_every) < 1e-3f;
            };
            for (int line = -count; line <= count; ++line)
            {
                if (skipped(line))
                {
                    continue; // a major line is drawn there
                }
                for (int segment = -count; segment < count; ++segment)
                {
                    const float fixed = static_cast<float>(line) * spacing;
                    const float from = static_cast<float>(segment) * spacing;
                    const float to = from + spacing;
                    for (const bool along_x : { true, false })
                    {
                        const Vector3f a = along_x ? Vector3f{ from, fixed, 0.0f } : Vector3f{ fixed, from, 0.0f };
                        const Vector3f b = along_x ? Vector3f{ to, fixed, 0.0f } : Vector3f{ fixed, to, 0.0f };
                        const std::uint8_t alpha_a = fade(alpha, a, radius);
                        const std::uint8_t alpha_b = fade(alpha, b, radius);
                        if (alpha_a == 0 && alpha_b == 0)
                        {
                            continue;
                        }
                        writer.line(a, { grey, grey, grey, alpha_a }, b, { grey, grey, grey, alpha_b });
                    }
                }
            }
        }

        template<typename Handle>
        void release_valid(scene::asset_cache& assets, const Handle handle) noexcept
        {
            if (handle.valid())
            {
                assets.release(handle);
            }
        }
    } // namespace

    namespace meshes
    {
        result<scene::mesh_data> unit_cube()
        {
            scene::mesh_builder builder{ scene::mesh_topology::triangles };
            builder.reserve(24, 36);
            primitive_writer writer{ builder };
            writer.box(Vector3f::Constant(-0.5f), Vector3f::Constant(0.5f));
            writer.end_primitive();
            return std::move(builder).build();
        }

        result<scene::mesh_data> cube_edges()
        {
            scene::mesh_builder builder{ scene::mesh_topology::lines };
            for (int corner = 0; corner < 8; ++corner)
            {
                builder.add_vertex(
                    { (corner & 1) != 0 ? 0.5f : -0.5f,
                      (corner & 2) != 0 ? 0.5f : -0.5f,
                      (corner & 4) != 0 ? 0.5f : -0.5f });
            }
            // Corners that differ in one bit share an edge
            for (std::uint32_t corner = 0; corner < 8; ++corner)
            {
                for (const std::uint32_t bit : { 1u, 2u, 4u })
                {
                    if ((corner & bit) == 0)
                    {
                        builder.add_index(corner).add_index(corner | bit);
                    }
                }
            }
            builder.end_primitive();
            return std::move(builder).build();
        }

        result<scene::mesh_data> axes()
        {
            scene::mesh_builder builder{ scene::mesh_topology::lines };
            primitive_writer writer{ builder };
            const Vector3f origin = Vector3f::Zero();
            writer.line(origin, { 230, 60, 60, 255 }, Vector3f::UnitX(), { 230, 60, 60, 255 });
            writer.line(origin, { 70, 200, 70, 255 }, Vector3f::UnitY(), { 70, 200, 70, 255 });
            writer.line(origin, { 70, 110, 240, 255 }, Vector3f::UnitZ(), { 70, 110, 240, 255 });
            writer.end_primitive();
            return std::move(builder).build();
        }

        result<scene::mesh_data> grid()
        {
            scene::mesh_builder builder{ scene::mesh_topology::lines };
            primitive_writer writer{ builder };
            add_grid_lines(writer, 1.0f, minor_grid_radius, 150, 0.35f, 10.0f);
            add_grid_lines(writer, 10.0f, major_grid_radius, 190, 0.6f, 0.0f);
            writer.end_primitive();
            return std::move(builder).build();
        }

        result<scene::mesh_data> vehicle_proxy()
        {
            scene::mesh_builder builder{ scene::mesh_topology::triangles };
            primitive_writer writer{ builder };

            // Primitive 0: the body and a cabin that narrows towards the roof
            writer.box({ -0.5f, -0.5f, 0.15f }, { 0.5f, 0.5f, 0.55f });
            writer.prism(
                primitive_writer::rectangle(-0.32f, 0.2f, -0.47f, 0.47f, 0.55f),
                primitive_writer::rectangle(-0.26f, 0.04f, -0.4f, 0.4f, 1.0f));
            writer.end_primitive();

            // Primitive 1: four wheels, slightly outside the body
            for (const float x : { -0.32f, 0.32f })
            {
                for (const float y : { -0.44f, 0.44f })
                {
                    writer.box({ x - 0.08f, y - 0.07f, 0.0f }, { x + 0.08f, y + 0.07f, 0.3f });
                }
            }
            writer.end_primitive();
            return std::move(builder).build();
        }

        result<scene::mesh_data> frustum(const float horizontal_fov, const float vertical_fov, const float range)
        {
            constexpr float largest = 179.0f * std::numbers::pi_v<float> / 180.0f;
            const float depth = range > 0.0f ? range : 1.0f;
            const float half_width = depth * std::tan(std::clamp(horizontal_fov, 0.0f, largest) / 2.0f);
            const float half_height = depth * std::tan(std::clamp(vertical_fov, 0.0f, largest) / 2.0f);

            scene::mesh_builder builder{ scene::mesh_topology::lines };
            builder.add_vertex(Vector3f::Zero());
            builder.add_vertex({ depth, half_width, half_height });
            builder.add_vertex({ depth, -half_width, half_height });
            builder.add_vertex({ depth, -half_width, -half_height });
            builder.add_vertex({ depth, half_width, -half_height });
            for (std::uint32_t corner = 1; corner <= 4; ++corner)
            {
                builder.add_index(0).add_index(corner); // apex to corner
                builder.add_index(corner).add_index(corner % 4 + 1); // far rectangle
            }
            builder.end_primitive();
            return std::move(builder).build();
        }
    } // namespace meshes

    result<std::unique_ptr<shared_assets>> shared_assets::create(scene::asset_cache& assets)
    {
        std::unique_ptr<shared_assets> made{ new shared_assets{ assets } };

        const auto make_mesh = [&](result<scene::mesh_data> data, scene::mesh_handle& target) -> result<void>
        {
            if (!data)
            {
                return std::unexpected(std::move(data.error()));
            }
            result<scene::mesh_handle> created = assets.create_mesh(std::move(*data));
            if (!created)
            {
                return std::unexpected(std::move(created.error()));
            }
            target = *created;
            return {};
        };
        const auto make_material = [&](scene::material_description description,
                                       scene::material_handle& target) -> result<void>
        {
            result<scene::material_handle> created = assets.create_material(std::move(description));
            if (!created)
            {
                return std::unexpected(std::move(created.error()));
            }
            target = *created;
            return {};
        };

        scene::material_description axes_description;
        axes_description.shading = scene::shading_model::unlit;
        axes_description.use_vertex_color = true;
        axes_description.line_width = 2.0f;
        axes_description.debug_name = "axes";

        scene::material_description grid_description;
        grid_description.shading = scene::shading_model::unlit;
        grid_description.alpha = scene::alpha_mode::blend;
        grid_description.use_vertex_color = true;
        grid_description.line_width = 1.0f;
        grid_description.debug_name = "grid";

        scene::material_description wheel_description;
        wheel_description.base_color_factor = { .red = 0.02f, .green = 0.02f, .blue = 0.02f, .alpha = 1.0f };
        wheel_description.metallic_factor = 0.0f;
        wheel_description.roughness_factor = 0.9f;
        wheel_description.debug_name = "vehicle wheels";

        for (result<void> step : {
                 make_mesh(meshes::unit_cube(), made->unit_cube_),
                 make_mesh(meshes::cube_edges(), made->cube_edges_),
                 make_mesh(meshes::axes(), made->axes_),
                 make_mesh(meshes::grid(), made->grid_),
                 make_mesh(meshes::vehicle_proxy(), made->vehicle_proxy_),
                 make_material(std::move(axes_description), made->axes_material_),
                 make_material(std::move(grid_description), made->grid_material_),
                 make_material(std::move(wheel_description), made->wheel_material_),
             })
        {
            if (!step)
            {
                return std::unexpected(std::move(step.error())); // the destructor releases what was made
            }
        }
        return made;
    }

    shared_assets::~shared_assets()
    {
        for (const scene::mesh_handle mesh : { unit_cube_, cube_edges_, axes_, grid_, vehicle_proxy_ })
        {
            release_valid(assets_, mesh);
        }
        for (const scene::material_handle material : { axes_material_, grid_material_, wheel_material_ })
        {
            release_valid(assets_, material);
        }
    }
} // namespace avis::visualizer::representation
