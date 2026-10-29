#include "avis/rhi/d3d12/resource_manager.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/formats.h"
#include "avis/rhi/d3d12/conversions/resources.h"
#include "avis/rhi/d3d12/conversions/samplers.h"
#include "avis/rhi/d3d12/deferred_release.h"
#include "avis/rhi/d3d12/descriptor_heaps.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/d3d12/shared_objects.h"
#include "avis/rhi/d3d12/view_descriptions.h"
#include "avis/rhi/error.h"

#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        std::string label(const std::string_view kind, const std::string_view name)
        {
            return name.empty() ? std::string{ kind } : std::format("{} '{}'", kind, name);
        }

        std::string_view usage_name(const texture_usage usage) noexcept
        {
            switch (usage)
            {
            case texture_usage::shader_resource:
                return "shader_resource";
            case texture_usage::unordered_access:
                return "unordered_access";
            case texture_usage::render_target:
                return "render_target";
            case texture_usage::depth_stencil:
                return "depth_stencil";
            case texture_usage::shared:
                return "shared";
            case texture_usage::copy_source:
                return "copy_source";
            case texture_usage::copy_destination:
                return "copy_destination";
            }
            return "unknown";
        }

        avis::error invalid(std::string message)
        {
            return make_error(error_kind::invalid_argument, std::move(message));
        }

        bool has(const avis::flags<texture_usage> usage, const texture_usage value) noexcept
        {
            return usage.count(value) != 0;
        }

        bool has(const avis::flags<buffer_usage> usage, const buffer_usage value) noexcept
        {
            return usage.count(value) != 0;
        }

        D3D12_FORMAT_SUPPORT1 dimension_support(const texture_dimension dimension) noexcept
        {
            switch (dimension)
            {
            case texture_dimension::texture_1d:
                return D3D12_FORMAT_SUPPORT1_TEXTURE1D;
            case texture_dimension::texture_2d:
                return D3D12_FORMAT_SUPPORT1_TEXTURE2D;
            case texture_dimension::texture_3d:
                return D3D12_FORMAT_SUPPORT1_TEXTURE3D;
            case texture_dimension::cube:
                return D3D12_FORMAT_SUPPORT1_TEXTURECUBE;
            }
            return D3D12_FORMAT_SUPPORT1_TEXTURE2D;
        }

        template<class Handle, class Record>
        void destroy_record(
            slot_map<Record>& map,
            lifetime_manager& lifetime,
            const Handle handle,
            const std::string_view kind) noexcept
        {
            std::optional<Record> record = map.extract(to_bits(handle));
            if (!record)
            {
                AVIS_RHI_CHECK(false, std::format("destroy: stale or invalid {} handle", kind));
                return;
            }
            if constexpr (requires { record->placed; })
            {
                if (record->placed)
                {
                    record->heap->live_resources.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            lifetime.destroy(release_entry{ std::move(*record) }); // out of memory terminates (§23.8)
        }
    } // namespace

    resource_manager::resource_manager(
        device& device, descriptor_heaps& descriptors, lifetime_manager& lifetime) noexcept :
        device_{ device },
        descriptors_{ descriptors },
        lifetime_{ lifetime }
    {}

    result<std::unique_ptr<resource_manager>>
        resource_manager::create(device& device, descriptor_heaps& descriptors, lifetime_manager& lifetime)
    {
        std::unique_ptr<resource_manager> manager{ new resource_manager{ device, descriptors, lifetime } };
        auto allocator = memory_allocator::create(device);
        if (!allocator)
        {
            return std::unexpected(std::move(allocator.error()));
        }
        manager->allocator_ = std::move(*allocator);
        return manager;
    }

    result<void> resource_manager::validate(const texture_description& description, const bool shared) const
    {
        const std::string name = label("texture", description.debug_name);
        if (description.format == format::unknown)
        {
            return std::unexpected(invalid(std::format("{}: format is unknown", name)));
        }
        if (description.width == 0 || description.height == 0 || description.depth_or_layers == 0)
        {
            return std::unexpected(invalid(
                std::format(
                    "{}: width, height and depth_or_layers must be at least 1 ({}×{}×{})",
                    name,
                    description.width,
                    description.height,
                    description.depth_or_layers)));
        }
        if (has(description.usage, texture_usage::shared) && !shared)
        {
            return std::unexpected(
                invalid(std::format("{}: shared textures are created with sharing::create_shared_texture", name)));
        }
        if (shared && !has(description.usage, texture_usage::shared))
        {
            return std::unexpected(
                invalid(std::format("{}: create_shared_texture requires texture_usage::shared", name)));
        }
        if (has(description.usage, texture_usage::render_target) &&
            has(description.usage, texture_usage::depth_stencil))
        {
            return std::unexpected(
                invalid(std::format("{}: render_target and depth_stencil exclude each other", name)));
        }

        // Each usage against FORMAT_SUPPORT1 (§8.1). A sampled depth texture is read through its depth view format.
        const auto supports = [&](const DXGI_FORMAT native, const D3D12_FORMAT_SUPPORT1 required)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ .Format = native };
            return SUCCEEDED(
                       device_.native().CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                   (support.Support1 & required) == required;
        };
        const DXGI_FORMAT native = to_native(description.format);
        const auto unsupported = [&](const std::string_view what)
        {
            return std::unexpected(invalid(
                std::format(
                    "{}: format {} doesn't support {} on this device", name, to_string(description.format), what)));
        };
        if (!supports(native, dimension_support(description.dimension)))
        {
            return unsupported("this texture dimension");
        }

        struct usage_requirement
        {
            texture_usage usage;
            D3D12_FORMAT_SUPPORT1 support;
        };
        const usage_requirement requirements[] = {
            { texture_usage::shader_resource, D3D12_FORMAT_SUPPORT1_SHADER_LOAD },
            { texture_usage::unordered_access, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW },
            { texture_usage::render_target, D3D12_FORMAT_SUPPORT1_RENDER_TARGET },
            { texture_usage::depth_stencil, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL },
        };
        const bool depth = has(description.usage, texture_usage::depth_stencil);
        for (const usage_requirement& requirement : requirements)
        {
            if (!has(description.usage, requirement.usage))
            {
                continue;
            }
            const DXGI_FORMAT checked = requirement.usage == texture_usage::shader_resource && depth ?
                                            to_native_depth_view(description.format) :
                                            native;
            if (!supports(checked, requirement.support))
            {
                return unsupported(usage_name(requirement.usage));
            }
        }
        return {};
    }

    result<void> resource_manager::validate(const buffer_description& description) const
    {
        const std::string name = label("buffer", description.debug_name);
        if (description.size == 0)
        {
            return std::unexpected(invalid(std::format("{}: size is 0", name)));
        }
        const bool viewed = has(description.usage, buffer_usage::shader_resource) ||
                            has(description.usage, buffer_usage::unordered_access);
        if (description.memory != memory_type::gpu_only && has(description.usage, buffer_usage::unordered_access))
        {
            return std::unexpected(
                invalid(std::format("{}: upload and readback buffers can't have unordered_access", name)));
        }
        if (description.memory == memory_type::readback && has(description.usage, buffer_usage::shader_resource))
        {
            return std::unexpected(invalid(std::format("{}: readback buffers can't have shader_resource", name)));
        }
        if (viewed && description.structure_stride > 0 && description.size % description.structure_stride != 0)
        {
            return std::unexpected(invalid(
                std::format(
                    "{}: size {} isn't a multiple of structure_stride {}",
                    name,
                    description.size,
                    description.structure_stride)));
        }
        if (viewed && description.structure_stride == 0 && description.size % 4 != 0)
        {
            return std::unexpected(invalid(
                std::format(
                    "{}: a raw buffer view needs a size that is a multiple of 4 ({})", name, description.size)));
        }
        return {};
    }

    result<void> resource_manager::create_views(texture_record& record)
    {
        const texture_description& description = record.description;
        if (has(description.usage, texture_usage::render_target) ||
            has(description.usage, texture_usage::depth_stencil))
        {
            record.views = std::make_unique<view_cache>(descriptors_);
        }
        if (record.placed)
        {
            return {}; // views come from descriptor ranges (§8.4)
        }
        if (has(description.usage, texture_usage::shader_resource))
        {
            auto srv = descriptors_.create_srv(*record.resource.get(), texture_srv(description));
            if (!srv)
            {
                return std::unexpected(std::move(srv.error()));
            }
            record.srv = std::move(*srv);
        }
        if (has(description.usage, texture_usage::unordered_access))
        {
            const std::uint32_t mips = mip_level_count(description);
            record.uav.reserve(mips);
            for (std::uint32_t mip = 0; mip < mips; ++mip)
            {
                auto uav = descriptors_.create_uav(*record.resource.get(), texture_uav(description, mip));
                if (!uav)
                {
                    return std::unexpected(std::move(uav.error()));
                }
                record.uav.push_back(std::move(*uav));
            }
        }
        return {};
    }

    result<void> resource_manager::create_views(buffer_record& record)
    {
        const buffer_description& description = record.description;
        if (record.placed)
        {
            return {};
        }
        if (has(description.usage, buffer_usage::shader_resource))
        {
            auto srv = descriptors_.create_srv(*record.resource.get(), buffer_srv(description));
            if (!srv)
            {
                return std::unexpected(std::move(srv.error()));
            }
            record.srv = std::move(*srv);
        }
        if (has(description.usage, buffer_usage::unordered_access))
        {
            auto uav = descriptors_.create_uav(*record.resource.get(), buffer_uav(description));
            if (!uav)
            {
                return std::unexpected(std::move(uav.error()));
            }
            record.uav = std::move(*uav);
        }
        return {};
    }

    result<texture_handle> resource_manager::create_texture(const texture_description& description)
    {
        if (auto valid = validate(description); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        auto record = allocator_->create_texture(description);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        if (auto views = create_views(*record); !views)
        {
            return std::unexpected(std::move(views.error()));
        }
        auto bits = textures_.insert(std::move(*record));
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<texture_handle>(*bits);
    }

    result<buffer_handle> resource_manager::create_buffer(const buffer_description& description)
    {
        if (auto valid = validate(description); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        auto record = allocator_->create_buffer(description);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        if (auto views = create_views(*record); !views)
        {
            return std::unexpected(std::move(views.error()));
        }
        auto bits = buffers_.insert(std::move(*record));
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<buffer_handle>(*bits);
    }

    result<sampler_handle> resource_manager::create_sampler(const sampler_description& description)
    {
        if (description.maximum_anisotropy < 1 || description.maximum_anisotropy > D3D12_MAX_MAXANISOTROPY)
        {
            return std::unexpected(invalid(
                std::format(
                    "sampler: maximum_anisotropy is {}, it must be between 1 and {}",
                    description.maximum_anisotropy,
                    D3D12_MAX_MAXANISOTROPY)));
        }
        const D3D12_SAMPLER_DESC native{
            .Filter = to_native_filter(
                description.minification, description.magnification, description.mip, description.compare.has_value()),
            .AddressU = to_native(description.address_u),
            .AddressV = to_native(description.address_v),
            .AddressW = to_native(description.address_w),
            .MipLODBias = description.mip_bias,
            .MaxAnisotropy = description.maximum_anisotropy,
            .ComparisonFunc = description.compare ? to_native(*description.compare) : D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = { description.border_color[0],
                             description.border_color[1],
                             description.border_color[2],
                             description.border_color[3] },
            .MinLOD = 0.0f,
            .MaxLOD = D3D12_FLOAT32_MAX,
        };
        auto lease = descriptors_.create_sampler(native);
        if (!lease)
        {
            return std::unexpected(std::move(lease.error()));
        }
        auto bits = samplers_.insert(sampler_record{ std::move(*lease) });
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<sampler_handle>(*bits);
    }

    result<heap_handle> resource_manager::create_heap(const heap_description& description)
    {
        if (description.size == 0)
        {
            return std::unexpected(invalid(std::format("{}: size is 0", label("heap", description.debug_name))));
        }
        auto heap = allocator_->create_heap(description);
        if (!heap)
        {
            return std::unexpected(std::move(heap.error()));
        }
        heap_record record{ .heap = std::move(*heap),
                            .description = description,
                            .name = std::string{ description.debug_name } };
        record.description.debug_name = {};
        auto bits = heaps_.insert(std::move(record));
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<heap_handle>(*bits);
    }

    namespace
    {
        // Offset alignment and bounds of a placed resource
        result<void> check_placement(
            const heap_record& heap,
            const std::uint64_t offset,
            const allocation_requirements& requirements,
            const std::string_view name)
        {
            if (requirements.alignment != 0 && offset % requirements.alignment != 0)
            {
                return std::unexpected(invalid(
                    std::format(
                        "{}: offset {} isn't a multiple of the required alignment {}",
                        name,
                        offset,
                        requirements.alignment)));
            }
            const std::uint64_t heap_size = heap.heap->allocation->GetSize();
            if (offset > heap_size || requirements.size > heap_size - offset)
            {
                return std::unexpected(invalid(
                    std::format(
                        "{}: {} bytes at offset {} don't fit in the heap ({} bytes)",
                        name,
                        requirements.size,
                        offset,
                        heap_size)));
            }
            return {};
        }
    } // namespace

    result<texture_handle> resource_manager::create_placed_texture(
        const heap_handle heap, const std::uint64_t offset, const texture_description& description)
    {
        const heap_record* const heap_entry = heaps_.find(to_bits(heap));
        if (heap_entry == nullptr)
        {
            return std::unexpected(
                make_error(error_kind::invalid_handle, "create_placed_texture: stale or invalid heap handle"));
        }
        if (auto valid = validate(description); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        if (auto placement = check_placement(
                *heap_entry, offset, requirements(description), label("placed texture", description.debug_name));
            !placement)
        {
            return std::unexpected(std::move(placement.error()));
        }

        auto record = allocator_->create_placed_texture(heap_entry->heap, offset, description);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        std::shared_ptr<placed_heap> shared = record->heap;
        if (auto views = create_views(*record); !views)
        {
            shared->live_resources.fetch_sub(1, std::memory_order_relaxed);
            return std::unexpected(std::move(views.error()));
        }
        auto bits = textures_.insert(std::move(*record));
        if (!bits)
        {
            shared->live_resources.fetch_sub(1, std::memory_order_relaxed);
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<texture_handle>(*bits);
    }

    result<buffer_handle> resource_manager::create_placed_buffer(
        const heap_handle heap, const std::uint64_t offset, const buffer_description& description)
    {
        const heap_record* const heap_entry = heaps_.find(to_bits(heap));
        if (heap_entry == nullptr)
        {
            return std::unexpected(
                make_error(error_kind::invalid_handle, "create_placed_buffer: stale or invalid heap handle"));
        }
        if (auto valid = validate(description); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        const std::string name = label("placed buffer", description.debug_name);
        if (description.memory != memory_type::gpu_only)
        {
            return std::unexpected(invalid(std::format("{}: placed buffers are gpu_only (heaps are DEFAULT)", name)));
        }
        if (auto placement = check_placement(*heap_entry, offset, requirements(description), name); !placement)
        {
            return std::unexpected(std::move(placement.error()));
        }

        auto record = allocator_->create_placed_buffer(heap_entry->heap, offset, description);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        std::shared_ptr<placed_heap> shared = record->heap;
        auto bits = buffers_.insert(std::move(*record));
        if (!bits)
        {
            shared->live_resources.fetch_sub(1, std::memory_order_relaxed);
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<buffer_handle>(*bits);
    }

    result<texture_handle> resource_manager::create_shared_texture(const texture_description& description)
    {
        if (auto valid = validate(description, true); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        auto record = allocator_->create_shared_texture(description);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        if (auto views = create_views(*record); !views)
        {
            return std::unexpected(std::move(views.error()));
        }
        auto bits = textures_.insert(std::move(*record));
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<texture_handle>(*bits);
    }

    result<HANDLE> resource_manager::export_shared_handle(const texture_handle texture)
    {
        texture_record* const record = textures_.find(to_bits(texture));
        if (record == nullptr)
        {
            return std::unexpected(make_error(error_kind::invalid_handle, "export_shared_handle: stale or invalid texture handle"));
        }
        if (!has(record->description.usage, texture_usage::shared))
        {
            return std::unexpected(invalid(std::format(
                "export_shared_handle: {} wasn't created with sharing::create_shared_texture", label("texture", record->name))));
        }
        const std::lock_guard lock{ export_mutex_ };
        return export_texture(device_, *record);
    }

    void resource_manager::destroy(const texture_handle texture) noexcept
    {
        destroy_record(textures_, lifetime_, texture, "texture");
    }

    void resource_manager::destroy(const buffer_handle buffer) noexcept
    {
        destroy_record(buffers_, lifetime_, buffer, "buffer");
    }

    void resource_manager::destroy(const sampler_handle sampler) noexcept
    {
        destroy_record(samplers_, lifetime_, sampler, "sampler");
    }

    void resource_manager::destroy(const heap_handle heap) noexcept
    {
        std::optional<heap_record> record = heaps_.extract(to_bits(heap));
        if (!record)
        {
            AVIS_RHI_CHECK(false, "destroy: stale or invalid heap handle");
            return;
        }
        // A programming error (§8.5); the memory stays alive for the placed resources either way (§21.3)
        AVIS_RHI_CHECK(
            record->heap->live_resources.load(std::memory_order_relaxed) == 0,
            std::format(
                "destroy: {} still has {} live placed resource(s); destroy them first",
                label("heap", record->name),
                record->heap->live_resources.load(std::memory_order_relaxed)));
        lifetime_.destroy(release_entry{ std::move(*record) });
    }

    const texture_record* resource_manager::find(const texture_handle texture) const noexcept
    {
        return textures_.find(to_bits(texture));
    }

    const buffer_record* resource_manager::find(const buffer_handle buffer) const noexcept
    {
        return buffers_.find(to_bits(buffer));
    }

    const sampler_record* resource_manager::find(const sampler_handle sampler) const noexcept
    {
        return samplers_.find(to_bits(sampler));
    }

    const heap_record* resource_manager::find(const heap_handle heap) const noexcept
    {
        return heaps_.find(to_bits(heap));
    }

    result<buffer_record> resource_manager::create_internal_buffer(const buffer_description& description)
    {
        if (auto valid = validate(description); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        return allocator_->create_buffer(description);
    }

    allocation_requirements resource_manager::requirements(const texture_description& description) const
    {
        return allocator_->requirements(to_native(description));
    }

    allocation_requirements resource_manager::requirements(const buffer_description& description) const
    {
        return allocator_->requirements(to_native(description));
    }

    memory_budget resource_manager::local_budget() const
    {
        return allocator_->local_budget();
    }
} // namespace avis::rhi::d3d12
