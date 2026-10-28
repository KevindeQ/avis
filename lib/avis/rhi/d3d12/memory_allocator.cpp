#include "avis/rhi/d3d12/memory_allocator.h"

#include "avis/log.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/barriers.h"
#include "avis/rhi/d3d12/conversions/formats.h"
#include "avis/rhi/d3d12/conversions/resources.h"
#include "avis/rhi/d3d12/device.h"

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::uint64_t heap_alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64 KiB

        std::string label(const std::string_view kind, const std::string_view name)
        {
            return name.empty() ? std::string{ kind } : std::format("{} '{}'", kind, name);
        }

        // The optimized clear value of a render target or depth texture (§8.1); none for other usages. Depth uses the
        // depth format, even when the resource is typeless (§20.2.3).
        std::optional<D3D12_CLEAR_VALUE> clear_value_of(const texture_description& description) noexcept
        {
            if (!description.optimized_clear)
            {
                return std::nullopt;
            }
            const clear_value& clear = *description.optimized_clear;
            D3D12_CLEAR_VALUE native{ .Format = to_native(description.format) };
            if (description.usage.count(texture_usage::depth_stencil) != 0)
            {
                native.DepthStencil = { .Depth = clear.depth, .Stencil = clear.stencil };
                return native;
            }
            if (description.usage.count(texture_usage::render_target) != 0)
            {
                native.Color[0] = clear.color[0];
                native.Color[1] = clear.color[1];
                native.Color[2] = clear.color[2];
                native.Color[3] = clear.color[3];
                return native;
            }
            return std::nullopt;
        }

        texture_record texture_record_for(const texture_description& description)
        {
            texture_record record;
            record.description = description;
            record.description.debug_name = {};
            record.name = std::string{ description.debug_name };
            return record;
        }

        buffer_record buffer_record_for(const buffer_description& description)
        {
            buffer_record record;
            record.description = description;
            record.description.debug_name = {};
            record.name = std::string{ description.debug_name };
            return record;
        }

        D3D12_HEAP_FLAGS heap_flags(const heap_kind kind) noexcept
        {
            switch (kind)
            {
            case heap_kind::all_resources:
                return D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES; // resource heap tier 2 (§4.4)
            case heap_kind::buffers:
                return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            case heap_kind::render_targets_and_textures:
                return D3D12_HEAP_FLAG_DENY_BUFFERS;
            }
            return D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
        }

        // Upload memory is never read on the CPU; readback memory is
        result<std::span<std::byte>> map(
            ID3D12Resource& resource, const memory_type memory, const std::uint64_t size, const std::string_view name)
        {
            if (memory == memory_type::gpu_only)
            {
                return std::span<std::byte>{};
            }
            const D3D12_RANGE nothing_read{ 0, 0 };
            void* data = nullptr;
            if (auto mapped = check(
                    resource.Map(0, memory == memory_type::upload ? &nothing_read : nullptr, &data),
                    std::format("Map ({})", name));
                !mapped)
            {
                return std::unexpected(std::move(mapped.error()));
            }
            return std::span<std::byte>{ static_cast<std::byte*>(data), static_cast<std::size_t>(size) };
        }
    } // namespace

    memory_allocator::memory_allocator(device& device) noexcept : device_{ device } {}

    result<std::unique_ptr<memory_allocator>> memory_allocator::create(device& device)
    {
        std::unique_ptr<memory_allocator> allocator{ new memory_allocator{ device } };
        const D3D12MA::ALLOCATOR_DESC description{
            .Flags = D3D12MA_RECOMMENDED_ALLOCATOR_FLAGS,
            .pDevice = &device.native(),
            .PreferredBlockSize = 0,
            .pAllocationCallbacks = nullptr,
            .pAdapter = &device.adapter(),
        };
        if (auto created =
                check(D3D12MA::CreateAllocator(&description, &allocator->allocator_), "D3D12MA::CreateAllocator");
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        return allocator;
    }

    result<texture_record> memory_allocator::create_texture(const texture_description& description)
    {
        return create_texture(description, D3D12_HEAP_FLAG_NONE, false);
    }

    result<texture_record> memory_allocator::create_shared_texture(const texture_description& description)
    {
        return create_texture(description, D3D12_HEAP_FLAG_SHARED, true);
    }

    result<texture_record> memory_allocator::create_texture(
        const texture_description& description, const D3D12_HEAP_FLAGS extra_heap_flags, const bool committed)
    {
        texture_record record = texture_record_for(description);
        const D3D12_RESOURCE_DESC1 native = to_native(description);
        const std::optional<D3D12_CLEAR_VALUE> clear = clear_value_of(description);
        const D3D12MA::ALLOCATION_DESC allocation{
            .Flags = committed ? D3D12MA::ALLOCATION_FLAG_COMMITTED : D3D12MA::ALLOCATION_FLAG_NONE,
            .HeapType = D3D12_HEAP_TYPE_DEFAULT,
            .ExtraHeapFlags = extra_heap_flags,
            .CustomPool = nullptr,
            .pPrivateData = nullptr,
        };
        if (auto created = check(
                allocator_->CreateResource3(
                    &allocation,
                    &native,
                    to_native(description.initial_layout),
                    clear ? &*clear : nullptr,
                    0,
                    nullptr,
                    &record.allocation,
                    IID_PPV_ARGS(&record.resource)),
                std::format("CreateResource3 ({})", label("texture", record.name)));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        set_debug_name(device_.debug(), *record.resource.get(), record.name);
        return record;
    }

    result<buffer_record> memory_allocator::create_buffer(const buffer_description& description)
    {
        buffer_record record = buffer_record_for(description);
        const D3D12_RESOURCE_DESC1 native = to_native(description);
        const D3D12MA::ALLOCATION_DESC allocation{
            .Flags = D3D12MA::ALLOCATION_FLAG_NONE,
            .HeapType = to_native(description.memory),
            .ExtraHeapFlags = D3D12_HEAP_FLAG_NONE,
            .CustomPool = nullptr,
            .pPrivateData = nullptr,
        };
        if (auto created = check(
                allocator_->CreateResource3(
                    &allocation,
                    &native,
                    D3D12_BARRIER_LAYOUT_UNDEFINED, // buffers have no layout
                    nullptr,
                    0,
                    nullptr,
                    &record.allocation,
                    IID_PPV_ARGS(&record.resource)),
                std::format("CreateResource3 ({})", label("buffer", record.name)));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        set_debug_name(device_.debug(), *record.resource.get(), record.name);
        record.address = { record.resource->GetGPUVirtualAddress() };

        auto mapped = map(*record.resource.get(), description.memory, native.Width, label("buffer", record.name));
        if (!mapped)
        {
            return std::unexpected(std::move(mapped.error()));
        }
        record.mapped = *mapped;
        return record;
    }

    result<std::shared_ptr<placed_heap>> memory_allocator::create_heap(const heap_description& description)
    {
        const std::uint64_t size = (description.size + heap_alignment - 1) / heap_alignment * heap_alignment;
        const D3D12MA::ALLOCATION_DESC allocation{
            .Flags = D3D12MA::ALLOCATION_FLAG_COMMITTED, // a heap of its own
            .HeapType = D3D12_HEAP_TYPE_DEFAULT,
            .ExtraHeapFlags = heap_flags(description.kind),
            .CustomPool = nullptr,
            .pPrivateData = nullptr,
        };
        const D3D12_RESOURCE_ALLOCATION_INFO info{ .SizeInBytes = size, .Alignment = heap_alignment };

        auto heap = std::make_shared<placed_heap>();
        if (auto created = check(
                allocator_->AllocateMemory(&allocation, &info, &heap->allocation),
                std::format("AllocateMemory ({})", label("heap", description.debug_name)));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        if (ID3D12Heap* const native = heap->allocation->GetHeap(); native != nullptr)
        {
            set_debug_name(device_.debug(), *native, description.debug_name);
        }
        return heap;
    }

    result<texture_record> memory_allocator::create_placed_texture(
        std::shared_ptr<placed_heap> heap, const std::uint64_t offset, const texture_description& description)
    {
        texture_record record = texture_record_for(description);
        const D3D12_RESOURCE_DESC1 native = to_native(description);
        const std::optional<D3D12_CLEAR_VALUE> clear = clear_value_of(description);
        if (auto created = check(
                allocator_->CreateAliasingResource2(
                    heap->allocation.get(),
                    offset,
                    &native,
                    D3D12_BARRIER_LAYOUT_UNDEFINED, // the first use is an aliasing barrier with discard (§10.1)
                    clear ? &*clear : nullptr,
                    0,
                    nullptr,
                    IID_PPV_ARGS(&record.resource)),
                std::format("CreateAliasingResource2 ({})", label("placed texture", record.name)));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        set_debug_name(device_.debug(), *record.resource.get(), record.name);
        record.placed = true;
        heap->live_resources.fetch_add(1, std::memory_order_relaxed);
        record.heap = std::move(heap);
        return record;
    }

    result<buffer_record> memory_allocator::create_placed_buffer(
        std::shared_ptr<placed_heap> heap, const std::uint64_t offset, const buffer_description& description)
    {
        buffer_record record = buffer_record_for(description);
        const D3D12_RESOURCE_DESC1 native = to_native(description);
        if (auto created = check(
                allocator_->CreateAliasingResource2(
                    heap->allocation.get(),
                    offset,
                    &native,
                    D3D12_BARRIER_LAYOUT_UNDEFINED,
                    nullptr,
                    0,
                    nullptr,
                    IID_PPV_ARGS(&record.resource)),
                std::format("CreateAliasingResource2 ({})", label("placed buffer", record.name)));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        set_debug_name(device_.debug(), *record.resource.get(), record.name);
        record.address = { record.resource->GetGPUVirtualAddress() };
        record.placed = true;
        heap->live_resources.fetch_add(1, std::memory_order_relaxed);
        record.heap = std::move(heap);
        return record;
    }

    allocation_requirements memory_allocator::requirements(const D3D12_RESOURCE_DESC1& description) const
    {
        const D3D12_RESOURCE_ALLOCATION_INFO info =
            device_.native().GetResourceAllocationInfo2(0, 1, &description, nullptr);
        return { .size = info.SizeInBytes, .alignment = info.Alignment };
    }

    memory_budget memory_allocator::local_budget() const
    {
        D3D12MA::Budget local{};
        allocator_->GetBudget(&local, nullptr);
        const memory_budget budget{ .budget = local.BudgetBytes, .usage = local.UsageBytes };

        // 90%: the point where the OS may start demoting resources to system memory
        if (budget.budget > 0 && budget.usage > budget.budget / 10 * 9)
        {
            const std::int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
            std::int64_t last = last_budget_warning_.load(std::memory_order_relaxed);
            const auto second =
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds{ 1 });
            if (now - last > second.count() &&
                last_budget_warning_.compare_exchange_strong(last, now, std::memory_order_relaxed))
            {
                log::get("rhi")->warn(
                    "local video memory: {} MiB used of a {} MiB budget (over 90%)",
                    budget.usage >> 20,
                    budget.budget >> 20);
            }
        }
        return budget;
    }
} // namespace avis::rhi::d3d12
