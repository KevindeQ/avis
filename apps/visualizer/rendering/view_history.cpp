#include "rendering/view_history.h"

#include "avis/graphics/error.h"
#include "avis/rhi/formats.h"

#include <format>
#include <utility>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    view_history_store::view_history_store(const rhi::resources resources) noexcept : resources_{ resources } {}

    view_history_store::~view_history_store()
    {
        for (auto& [name, history] : histories_)
        {
            destroy_resources(history);
        }
    }

    result<view_history*> view_history_store::acquire(
        const std::string_view name, const graphics::texture_extent size, const std::uint64_t frame_index)
    {
        auto found = histories_.find(name);
        if (found != histories_.end())
        {
            view_history& history = found->second;
            if (history.last_used_frame == frame_index)
            {
                return std::unexpected{ graphics::make_error(
                    graphics::error_kind::invalid_argument,
                    std::format("view history '{}' acquired twice in frame {}", name, frame_index)) };
            }
            if (history.size == size)
            {
                history.current = history.previous();
                history.has_previous = true;
                history.last_used_frame = frame_index;
                return &history;
            }
            // Resized: the old content doesn't match the new size, except the exposure's
            view_history kept;
            kept.exposure = std::exchange(history.exposure, {});
            kept.exposure_initialized = history.exposure_initialized;
            kept.last_adaptation = history.last_adaptation;
            destroy_resources(history);
            histories_.erase(found);
            return insert(name, size, frame_index, std::move(kept));
        }
        return insert(name, size, frame_index, {});
    }

    result<view_history*> view_history_store::insert(
        const std::string_view name,
        const graphics::texture_extent size,
        const std::uint64_t frame_index,
        view_history created)
    {
        if (auto made = create_resources(name, size, created); !made)
        {
            destroy_resources(created);
            return std::unexpected{ std::move(made).error() };
        }
        created.size = size;
        created.last_used_frame = frame_index;
        auto [inserted, ignored] = histories_.emplace(std::string{ name }, created);
        return &inserted->second;
    }

    void view_history_store::release_unused(const std::uint64_t frame_index) noexcept
    {
        for (auto entry = histories_.begin(); entry != histories_.end();)
        {
            if (frame_index - entry->second.last_used_frame >= view_history_release_frames)
            {
                destroy_resources(entry->second);
                entry = histories_.erase(entry);
            }
            else
            {
                ++entry;
            }
        }
    }

    const view_history* view_history_store::find(const std::string_view name) const noexcept
    {
        const auto found = histories_.find(name);
        return found == histories_.end() ? nullptr : &found->second;
    }

    view_history* view_history_store::find(const std::string_view name) noexcept
    {
        const auto found = histories_.find(name);
        return found == histories_.end() ? nullptr : &found->second;
    }

    result<void> view_history_store::create_resources(
        const std::string_view name, const graphics::texture_extent size, view_history& history)
    {
        for (std::uint32_t slot = 0; slot < 2; ++slot)
        {
            const std::string taa_name = std::format("{}/taa_history[{}]", name, slot);
            auto taa = resources_.create_texture(
                { .width = size.width,
                  .height = size.height,
                  .format = rhi::format::rgba16_float,
                  .usage = rhi::texture_usage::shader_resource | rhi::texture_usage::unordered_access,
                  .debug_name = taa_name });
            if (!taa)
            {
                return std::unexpected{ std::move(taa).error() };
            }
            history.taa_history[slot] = *taa;

            const std::string depth_name = std::format("{}/depth_history[{}]", name, slot);
            auto depth = resources_.create_texture(
                { .width = size.width,
                  .height = size.height,
                  .format = rhi::format::r32_float,
                  .usage = rhi::texture_usage::shader_resource | rhi::texture_usage::unordered_access |
                           rhi::texture_usage::copy_destination,
                  .debug_name = depth_name });
            if (!depth)
            {
                return std::unexpected{ std::move(depth).error() };
            }
            history.depth[slot] = *depth;
        }

        if (!history.exposure.is_valid())
        {
            const std::string exposure_name = std::format("{}/exposure", name);
            auto exposure = resources_.create_buffer(
                { .size = gpu::exposure_buffer_bytes,
                  .usage = rhi::buffer_usage::shader_resource | rhi::buffer_usage::unordered_access,
                  .memory = rhi::memory_type::gpu_only,
                  .debug_name = exposure_name });
            if (!exposure)
            {
                return std::unexpected{ std::move(exposure).error() };
            }
            history.exposure = *exposure;
            history.exposure_initialized = false;
            history.last_adaptation.reset();
        }

        const std::string clusters_name = std::format("{}/cluster_bounds", name);
        auto clusters = resources_.create_buffer(
            { .size = std::uint64_t{ gpu::cluster_count } * sizeof(gpu::cluster_bounds),
              .structure_stride = sizeof(gpu::cluster_bounds),
              .usage = rhi::buffer_usage::shader_resource | rhi::buffer_usage::unordered_access,
              .memory = rhi::memory_type::gpu_only,
              .debug_name = clusters_name });
        if (!clusters)
        {
            return std::unexpected{ std::move(clusters).error() };
        }
        history.cluster_bounds = *clusters;
        return {};
    }

    void view_history_store::destroy_resources(view_history& history) noexcept
    {
        for (rhi::texture_handle& texture : history.taa_history)
        {
            if (texture.is_valid())
            {
                resources_.destroy(texture);
                texture = {};
            }
        }
        for (rhi::texture_handle& texture : history.depth)
        {
            if (texture.is_valid())
            {
                resources_.destroy(texture);
                texture = {};
            }
        }
        if (history.exposure.is_valid())
        {
            resources_.destroy(history.exposure);
            history.exposure = {};
        }
        if (history.cluster_bounds.is_valid())
        {
            resources_.destroy(history.cluster_bounds);
            history.cluster_bounds = {};
        }
        history.cluster_projection.reset();
    }
} // namespace avis::visualizer::rendering
