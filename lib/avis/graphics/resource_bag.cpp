#include "avis/graphics/resource_bag.h"

#include "render_graph_executor.h"

#include "avis/detail/check.h"

#include <algorithm>
#include <format>
#include <new>
#include <string_view>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";

        thread_local const detail::render_pass_node* recording_pass = nullptr;

        const detail::bag_entry& empty_entry() noexcept
        {
            static const detail::bag_entry entry{};
            return entry;
        }
    } // namespace

    namespace detail
    {
        const render_pass_node* current_pass() noexcept
        {
            return recording_pass;
        }

        current_pass_scope::current_pass_scope(const render_pass_node* const pass) noexcept :
            previous_{ recording_pass }
        {
            recording_pass = pass;
        }

        current_pass_scope::~current_pass_scope()
        {
            recording_pass = previous_;
        }

        const resource_bag& resource_bag_access::create(
            linear_arena& arena, const rhi::resources resources, std::span<const bag_entry> entries)
        {
            return *::new (arena.allocate(sizeof(resource_bag), alignof(resource_bag)))
                resource_bag{ resources, entries };
        }
    } // namespace detail

    const detail::bag_entry& resource_bag::find(const resource& handle, const detail::resource_kind kind) const
    {
        if (!AVIS_CHECK(
                logger_name,
                handle.valid() && handle.index() < entries_.size() && entries_[handle.index()].kind == kind,
                "resource_bag: the handle isn't a resource of this frame's graph"))
        {
            return empty_entry();
        }
#ifndef NDEBUG
        // A pass may only look up what its setup declared (§3.4): it catches missing dependencies early
        if (const detail::render_pass_node* const pass = recording_pass; pass != nullptr)
        {
            const bool declared = std::ranges::any_of(
                pass->declared_accesses(),
                [&](const detail::declared_access& access) { return access.resource == handle.index(); });
            if (!AVIS_CHECK(
                    logger_name,
                    declared,
                    std::format(
                        "resource_bag: pass '{}' looked up resource {} without declaring it in its setup",
                        pass->name,
                        handle.index())))
            {
                return empty_entry();
            }
        }
#endif
        return entries_[handle.index()];
    }

    rhi::texture_handle resource_bag::texture_handle(const texture resource) const
    {
        return find(resource, detail::resource_kind::texture).physical_texture;
    }

    rhi::buffer_handle resource_bag::buffer_handle(const buffer resource) const
    {
        return find(resource, detail::resource_kind::buffer).physical_buffer;
    }

    rhi::gpu_address resource_bag::gpu_address(const buffer resource) const
    {
        return find(resource, detail::resource_kind::buffer).address;
    }

    rhi::bindless_index resource_bag::srv(const texture resource) const
    {
        const detail::bag_entry& entry = find(resource, detail::resource_kind::texture);
        return entry.imported && entry.physical_texture.is_valid() ? resources_.srv(entry.physical_texture) : entry.srv;
    }

    rhi::bindless_index resource_bag::uav(const texture resource, const std::uint32_t mip) const
    {
        const detail::bag_entry& entry = find(resource, detail::resource_kind::texture);
        if (entry.imported && entry.physical_texture.is_valid())
        {
            return resources_.uav(entry.physical_texture, mip);
        }
        return mip < entry.texture_uavs.size() ? entry.texture_uavs[mip] : rhi::bindless_index{};
    }

    rhi::bindless_index resource_bag::srv(const buffer resource) const
    {
        const detail::bag_entry& entry = find(resource, detail::resource_kind::buffer);
        return entry.imported && entry.physical_buffer.is_valid() ? resources_.srv(entry.physical_buffer) : entry.srv;
    }

    rhi::bindless_index resource_bag::uav(const buffer resource) const
    {
        const detail::bag_entry& entry = find(resource, detail::resource_kind::buffer);
        return entry.imported && entry.physical_buffer.is_valid() ? resources_.uav(entry.physical_buffer) :
                                                                    entry.buffer_uav;
    }

    rhi::texture_view resource_bag::render_target_view(
        const texture resource, const std::uint32_t mip, const std::uint32_t layer) const
    {
        return { .texture = texture_handle(resource),
                 .mip = static_cast<std::uint16_t>(mip),
                 .first_layer = static_cast<std::uint16_t>(layer),
                 .layer_count = 1 };
    }

    rhi::texture_view resource_bag::depth_stencil_view(const texture resource) const
    {
        return { .texture = texture_handle(resource) };
    }

    std::uint32_t resource_bag::width(const texture resource) const
    {
        return description(resource).size.width;
    }

    std::uint32_t resource_bag::height(const texture resource) const
    {
        return description(resource).size.height;
    }

    const texture_description& resource_bag::description(const texture resource) const
    {
        return find(resource, detail::resource_kind::texture).texture_info;
    }

    const buffer_description& resource_bag::description(const buffer resource) const
    {
        return find(resource, detail::resource_kind::buffer).buffer_info;
    }
} // namespace avis::graphics
