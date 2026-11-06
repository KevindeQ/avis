#ifndef AVIS_VISUALIZER_RENDERING_ENVIRONMENT_BAKER_H
#define AVIS_VISUALIZER_RENDERING_ENVIRONMENT_BAKER_H

// The baked environment (docs/design/11-renderer.md §6.4, docs/tasks/11-renderer.md §11.11): what the image-based
// lighting reads, in persistent textures the renderer owns and imports into every frame.
// - The sky cubemap (RGBA16F, environment_sky_size² per face): the analytic sky of sky_model.h.
// - The GGX-prefiltered cubemap (RGBA16F, environment_prefiltered_size², environment_prefiltered_mips mips), from the
// sky
//   cubemap, with mip m for roughness m / (mips - 1).
// - The SH9 irradiance: environment_irradiance_coefficients float4s (RGB), already convolved with the cosine lobe.
// - The split-sum BRDF LUT (RG16F, brdf_lut_size²), which doesn't depend on the environment.
//
// The bake passes are added to a frame only when the environment differs from the one last baked; the LUT's only to the
// first frame. They write imported textures, so the graph never culls them: frames whose views write nothing skip them.

#include "rendering/sky_model.h"

#include "avis/error.h"
#include "avis/graphics/buffer.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <memory>
#include <optional>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // Samples per prefiltered texel (roughness above 0), per BRDF LUT texel, and per face side for the SH9 projection
    inline constexpr std::uint32_t environment_prefilter_samples = 64;
    inline constexpr std::uint32_t brdf_lut_samples = 512;
    inline constexpr std::uint32_t environment_irradiance_samples_per_side = 32;

    struct environment_pipelines
    {
        rhi::compute_pipeline_handle sky; // environment_bake.sky_main
        rhi::compute_pipeline_handle prefilter;
        rhi::compute_pipeline_handle irradiance;
        rhi::compute_pipeline_handle brdf_lut;
    };

    // The baked resources in this frame's graph, imported outside any view: the bake's versions on a frame that bakes
    struct environment_resources
    {
        graphics::texture sky;
        graphics::texture prefiltered;
        graphics::buffer irradiance;
        graphics::texture brdf_lut;
    };

    struct environment_statistics
    {
        std::uint64_t bakes = 0; // frames that baked the environment
        std::uint64_t brdf_lut_bakes = 0;
    };

    class environment_baker
    {
    public:
        // Creates the persistent resources; fails with the RHI's error
        static result<std::unique_ptr<environment_baker>>
            create(rhi::resources resources, const environment_pipelines& pipelines);

        ~environment_baker(); // destroys the resources (deferred by the RHI until the GPU is done)
        environment_baker(const environment_baker&) = delete;
        environment_baker& operator=(const environment_baker&) = delete;

        // Imports the resources into `graph`, outside any view. When `bake` is set, first adds the bake of
        // `environment` if it differs from the one baked, and the LUT's if it was never baked.
        environment_resources
            add_passes(graphics::render_graph& graph, const environment_description& environment, bool bake);

        // The frame whose add_passes baked failed before its passes ran: the next frame bakes again
        void forget() noexcept;

        // The sky of the environment last passed to add_passes, for the sky pass
        const gpu::sky_constants& sky() const noexcept
        {
            return sky_;
        }

        environment_statistics statistics() const noexcept
        {
            return statistics_;
        }

        // For tests: the persistent resources
        rhi::texture_handle sky_texture() const noexcept
        {
            return sky_texture_;
        }
        rhi::texture_handle prefiltered_texture() const noexcept
        {
            return prefiltered_texture_;
        }
        rhi::buffer_handle irradiance_buffer() const noexcept
        {
            return irradiance_buffer_;
        }
        rhi::texture_handle brdf_lut_texture() const noexcept
        {
            return brdf_lut_texture_;
        }

    private:
        environment_baker(rhi::resources resources, const environment_pipelines& pipelines) noexcept;

        void destroy() noexcept;
        void add_environment_bake(graphics::render_graph& graph, environment_resources& resources);
        void add_brdf_lut_bake(graphics::render_graph& graph, environment_resources& resources);

        rhi::resources resources_;
        environment_pipelines pipelines_;
        rhi::texture_handle sky_texture_;
        rhi::texture_handle prefiltered_texture_;
        rhi::buffer_handle irradiance_buffer_;
        rhi::texture_handle brdf_lut_texture_;

        gpu::sky_constants sky_{};
        std::optional<environment_description> baked_; // what the resources hold, once a bake was recorded
        bool brdf_lut_baked_ = false;
        bool environment_pending_ = false; // the last add_passes recorded the environment bake
        bool brdf_lut_pending_ = false; // ... and the LUT's
        environment_statistics statistics_;
    };
} // namespace avis::visualizer::rendering

#endif
