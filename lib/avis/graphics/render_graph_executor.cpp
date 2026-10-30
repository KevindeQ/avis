#include "render_graph_executor.h"

#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/transient_resource_pool.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

namespace avis::graphics::detail
{
    namespace
    {
        // The views a transient resource's accesses need
        struct view_needs
        {
            bool srv = false;
            bool uav = false; // buffers
            std::uint64_t uav_mips = 0; // textures: a bit per mip (the first 64)
        };

        void add_needs(view_needs& needs, const declared_access& access, const std::uint32_t mip_count) noexcept
        {
            if (access.kind == resource_kind::texture)
            {
                if (access.direction == access_direction::read)
                {
                    const auto usage = static_cast<texture_read>(access.usage);
                    needs.srv = needs.srv || usage == texture_read::srv_pixel || usage == texture_read::srv_compute ||
                                usage == texture_read::depth_read;
                }
                else if (static_cast<texture_write>(access.usage) == texture_write::uav)
                {
                    const std::uint32_t first = access.range.first_mip;
                    const std::uint32_t end =
                        access.range.mip_count == subresource_range::all ? mip_count : first + access.range.mip_count;
                    for (std::uint32_t mip = first; mip < end && mip < 64; ++mip)
                    {
                        needs.uav_mips |= std::uint64_t{ 1 } << mip;
                    }
                }
                return;
            }
            if (access.direction == access_direction::read)
            {
                const auto usage = static_cast<buffer_read>(access.usage);
                needs.srv = needs.srv || usage == buffer_read::srv_pixel || usage == buffer_read::srv_compute;
            }
            else
            {
                needs.uav = needs.uav || static_cast<buffer_write>(access.usage) == buffer_write::uav;
            }
        }
    } // namespace

    result<const resource_bag*> materialise(
        const render_graph_tables& tables,
        const compiled_graph& compiled,
        transient_resource_pool& pool,
        frame_scheduler& frames,
        const frame_token& frame,
        const rhi::resources resources,
        linear_arena& arena)
    {
        const std::uint32_t context_index = frame.context_index();
        if (auto prepared = pool.prepare(context_index, compiled.statistics.heap_size); !prepared)
        {
            return std::unexpected(std::move(prepared.error()));
        }

        const std::size_t resource_count = tables.resources.size();
        const std::span<view_needs> needs = arena.allocate_array<view_needs>(resource_count);
        std::ranges::fill(needs, view_needs{});
        for (const std::uint32_t pass : compiled.order)
        {
            for (const declared_access& access : tables.passes[pass]->declared_accesses())
            {
                add_needs(needs[access.resource], access, compiled.resources[access.resource].mip_count);
            }
        }

        const std::span<bag_entry> entries = make_array<bag_entry>(arena, resource_count);
        for (std::uint32_t index = 0; index < resource_count; ++index)
        {
            const resource_entry& source = tables.resources[index];
            const compiled_resource& resource = compiled.resources[index];
            bag_entry& entry = entries[index];
            entry.kind = source.kind;
            entry.imported = source.imported;
            entry.texture_info = source.texture;
            entry.texture_info.size = absolute(source.extent.width, source.extent.height);
            entry.buffer_info = source.buffer;

            if (source.imported)
            {
                entry.physical_texture = source.texture_handle;
                entry.physical_buffer = source.buffer_handle;
                if (source.kind == resource_kind::buffer && source.buffer_handle.is_valid())
                {
                    entry.address = resources.buffer_address(source.buffer_handle);
                }
                continue;
            }
            if (!resource.lifetime.live())
            {
                continue; // culled with its passes
            }

            const view_needs& wanted = needs[index];
            if (source.kind == resource_kind::texture)
            {
                auto texture = pool.acquire_texture(context_index, resource.texture, resource.offset);
                if (!texture)
                {
                    return std::unexpected(std::move(texture.error()));
                }
                entry.physical_texture = *texture;
                if (wanted.srv)
                {
                    auto view = frames.create_frame_srv(frame, *texture);
                    if (!view)
                    {
                        return std::unexpected(std::move(view.error()));
                    }
                    entry.srv = *view;
                }
                if (wanted.uav_mips != 0)
                {
                    const std::uint32_t mips = std::min<std::uint32_t>(resource.mip_count, 64);
                    const std::span<rhi::bindless_index> uavs = arena.allocate_array<rhi::bindless_index>(mips);
                    for (std::uint32_t mip = 0; mip < mips; ++mip)
                    {
                        uavs[mip] = {};
                        if ((wanted.uav_mips & (std::uint64_t{ 1 } << mip)) == 0)
                        {
                            continue;
                        }
                        auto view = frames.create_frame_uav(frame, *texture, mip);
                        if (!view)
                        {
                            return std::unexpected(std::move(view.error()));
                        }
                        uavs[mip] = *view;
                    }
                    entry.texture_uavs = uavs;
                }
            }
            else
            {
                auto buffer = pool.acquire_buffer(context_index, resource.buffer, resource.offset);
                if (!buffer)
                {
                    return std::unexpected(std::move(buffer.error()));
                }
                entry.physical_buffer = *buffer;
                entry.address = resources.buffer_address(*buffer);
                if (wanted.srv)
                {
                    auto view = frames.create_frame_srv(frame, *buffer);
                    if (!view)
                    {
                        return std::unexpected(std::move(view.error()));
                    }
                    entry.srv = *view;
                }
                if (wanted.uav)
                {
                    auto view = frames.create_frame_uav(frame, *buffer);
                    if (!view)
                    {
                        return std::unexpected(std::move(view.error()));
                    }
                    entry.buffer_uav = *view;
                }
            }
        }
        return &resource_bag_access::create(arena, resources, entries);
    }
} // namespace avis::graphics::detail
