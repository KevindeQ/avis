#include "avis/rhi/d3d12/descriptor_heaps.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/formats.h"
#include "avis/rhi/d3d12/conversions/resources.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/error.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <string_view>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::uint64_t pack(const std::uint32_t index, const std::uint32_t tag) noexcept
        {
            return (static_cast<std::uint64_t>(tag) << 32) | index;
        }

        constexpr std::uint32_t index_of(const std::uint64_t head) noexcept
        {
            return static_cast<std::uint32_t>(head);
        }

        constexpr std::uint32_t tag_of(const std::uint64_t head) noexcept
        {
            return static_cast<std::uint32_t>(head >> 32);
        }

        avis::error heap_full(const std::string_view heap, const std::uint32_t capacity)
        {
            return make_error(
                error_kind::descriptor_heap_full,
                std::format("{} is full ({} descriptors; see descriptor_heap_configuration)", heap, capacity));
        }

        result<com_ptr<ID3D12DescriptorHeap>> create_heap(
            device& device,
            const D3D12_DESCRIPTOR_HEAP_TYPE type,
            const std::uint32_t count,
            const bool shader_visible,
            const std::string_view name)
        {
            const D3D12_DESCRIPTOR_HEAP_DESC description{
                .Type = type,
                .NumDescriptors = count,
                .Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                .NodeMask = 0,
            };
            com_ptr<ID3D12DescriptorHeap> heap;
            if (auto created = check(
                    device.native().CreateDescriptorHeap(&description, IID_PPV_ARGS(&heap)),
                    std::format("CreateDescriptorHeap ({})", name));
                !created)
            {
                return std::unexpected(std::move(created.error()));
            }
            set_debug_name(device.debug(), *heap.get(), name);
            return heap;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE offset(
            const D3D12_CPU_DESCRIPTOR_HANDLE start, const std::uint32_t index, const std::uint32_t increment) noexcept
        {
            return { start.ptr + static_cast<SIZE_T>(index) * increment };
        }
    } // namespace

    // ---- index_free_list ----

    void index_free_list::initialise(const std::uint32_t begin, const std::uint32_t end)
    {
        next_ = std::make_unique<std::atomic<std::uint32_t>[]>(end);
        std::uint32_t head = none;
        for (std::uint32_t index = end; index > begin; --index)
        {
            next_[index - 1].store(head, std::memory_order_relaxed);
            head = index - 1;
        }
        head_.store(pack(head, 0), std::memory_order_release);
    }

    std::optional<std::uint32_t> index_free_list::pop() noexcept
    {
        std::uint64_t head = head_.load(std::memory_order_acquire);
        for (;;)
        {
            const std::uint32_t index = index_of(head);
            if (index == none)
            {
                return std::nullopt;
            }
            // May read a stale link if another thread popped `index` meanwhile; the tag makes the CAS fail then
            const std::uint32_t next = next_[index].load(std::memory_order_relaxed);
            if (head_.compare_exchange_weak(
                    head, pack(next, tag_of(head) + 1), std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return index;
            }
        }
    }

    void index_free_list::push(const std::uint32_t index) noexcept
    {
        std::uint64_t head = head_.load(std::memory_order_relaxed);
        for (;;)
        {
            next_[index].store(index_of(head), std::memory_order_relaxed);
            if (head_.compare_exchange_weak(
                    head, pack(index, tag_of(head) + 1), std::memory_order_release, std::memory_order_relaxed))
            {
                return;
            }
        }
    }

    std::uint32_t index_free_list::count() const noexcept
    {
        std::uint32_t total = 0;
        for (std::uint32_t index = index_of(head_.load(std::memory_order_acquire)); index != none;
             index = next_[index].load(std::memory_order_relaxed))
        {
            ++total;
        }
        return total;
    }

    // ---- descriptor_lease ----

    descriptor_lease::descriptor_lease(descriptor_heaps& heaps, const std::uint32_t index, const pool kind) noexcept :
        heaps_{ &heaps },
        index_{ index },
        pool_{ kind }
    {}

    descriptor_lease::descriptor_lease(descriptor_lease&& other) noexcept :
        heaps_{ std::exchange(other.heaps_, nullptr) },
        index_{ std::exchange(other.index_, bindless_index{}) },
        pool_{ other.pool_ }
    {}

    descriptor_lease& descriptor_lease::operator=(descriptor_lease&& other) noexcept
    {
        if (this != &other)
        {
            release();
            heaps_ = std::exchange(other.heaps_, nullptr);
            index_ = std::exchange(other.index_, bindless_index{});
            pool_ = other.pool_;
        }
        return *this;
    }

    descriptor_lease::~descriptor_lease()
    {
        release();
    }

    bindless_index descriptor_lease::index() const noexcept
    {
        return index_;
    }

    void descriptor_lease::release() noexcept
    {
        if (heaps_ == nullptr)
        {
            return;
        }
        if (pool_ == pool::persistent)
        {
            heaps_->free_persistent(index_);
        }
        else
        {
            heaps_->free_sampler(index_);
        }
        heaps_ = nullptr;
        index_ = {};
    }

    // ---- descriptor_range_state ----

    descriptor_range_state::descriptor_range_state(
        descriptor_heaps& owner, const std::uint32_t first_index, const std::uint32_t index_count) noexcept :
        heaps{ &owner },
        first{ first_index },
        count{ index_count }
    {}

    descriptor_range_state::~descriptor_range_state()
    {
        heaps->free_range(*this);
    }

    // ---- descriptor_heaps ----

    descriptor_heaps::descriptor_heaps(device& device, const descriptor_heap_configuration& configuration) noexcept :
        device_{ device },
        configuration_{ configuration }
    {}

    result<std::unique_ptr<descriptor_heaps>>
        descriptor_heaps::create(device& device, const descriptor_heap_configuration& configuration)
    {
        std::unique_ptr<descriptor_heaps> heaps{ new descriptor_heaps{ device, configuration } };
        const std::uint32_t resource_count =
            configuration.persistent_descriptor_count + configuration.transient_descriptor_count;

        struct heap_to_create
        {
            com_ptr<ID3D12DescriptorHeap>* heap;
            D3D12_DESCRIPTOR_HEAP_TYPE type;
            std::uint32_t count;
            bool shader_visible;
            std::string_view name;
        };
        const std::array to_create{
            heap_to_create{ &heaps->resource_heap_,
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                            resource_count,
                            true,
                            "CBV/SRV/UAV heap" },
            heap_to_create{ &heaps->resource_staging_heap_,
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                            resource_count,
                            false,
                            "CBV/SRV/UAV staging heap" },
            heap_to_create{ &heaps->sampler_heap_,
                            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                            configuration.sampler_count,
                            true,
                            "sampler heap" },
            heap_to_create{ &heaps->render_target_heap_,
                            D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                            configuration.render_target_view_count,
                            false,
                            "RTV heap" },
            heap_to_create{ &heaps->depth_stencil_heap_,
                            D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                            configuration.depth_stencil_view_count,
                            false,
                            "DSV heap" },
        };
        for (const heap_to_create& entry : to_create)
        {
            auto heap = create_heap(device, entry.type, entry.count, entry.shader_visible, entry.name);
            if (!heap)
            {
                return std::unexpected(std::move(heap.error()));
            }
            *entry.heap = std::move(*heap);
        }

        device_interface& native = device.native();
        heaps->resource_increment_ = native.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        heaps->sampler_increment_ = native.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        heaps->render_target_increment_ = native.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        heaps->depth_stencil_increment_ = native.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        heaps->resource_cpu_start_ = heaps->resource_heap_->GetCPUDescriptorHandleForHeapStart();
        heaps->resource_gpu_start_ = heaps->resource_heap_->GetGPUDescriptorHandleForHeapStart();
        heaps->staging_cpu_start_ = heaps->resource_staging_heap_->GetCPUDescriptorHandleForHeapStart();
        heaps->sampler_cpu_start_ = heaps->sampler_heap_->GetCPUDescriptorHandleForHeapStart();
        heaps->render_target_cpu_start_ = heaps->render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        heaps->depth_stencil_cpu_start_ = heaps->depth_stencil_heap_->GetCPUDescriptorHandleForHeapStart();

        // Index 0 of the persistent region and of the sampler heap are never handed out: a default bindless_index
        // finds a null SRV, or a default linear/wrap sampler, instead of faulting under GPU-based validation (§9.2)
        heaps->persistent_free_.initialise(1, configuration.persistent_descriptor_count);
        heaps->sampler_free_.initialise(1, configuration.sampler_count);
        heaps->render_target_free_.initialise(0, configuration.render_target_view_count);
        heaps->depth_stencil_free_.initialise(0, configuration.depth_stencil_view_count);
        heaps->write_null_srv(0, true);

        const D3D12_SAMPLER_DESC default_sampler{
            .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .MipLODBias = 0.0f,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = { 0.0f, 0.0f, 0.0f, 0.0f },
            .MinLOD = 0.0f,
            .MaxLOD = D3D12_FLOAT32_MAX,
        };
        native.CreateSampler(&default_sampler, heaps->sampler_cpu_handle({ 0 }));

        if (configuration.transient_descriptor_count > 0)
        {
            heaps->free_ranges_.emplace_back(
                configuration.persistent_descriptor_count, configuration.transient_descriptor_count);
        }
        return heaps;
    }

    void descriptor_heaps::write_null_srv(const std::uint32_t index, const bool staging) noexcept
    {
        const D3D12_SHADER_RESOURCE_VIEW_DESC null_view{
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
        };
        device_.native().CreateShaderResourceView(nullptr, &null_view, shader_visible_cpu_handle({ index }));
        if (staging)
        {
            device_.native().CreateShaderResourceView(nullptr, &null_view, staging_cpu_handle({ index }));
        }
    }

    result<descriptor_lease>
        descriptor_heaps::create_srv(ID3D12Resource& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view)
    {
        const std::optional<std::uint32_t> index = persistent_free_.pop();
        if (!index)
        {
            return std::unexpected(
                heap_full("the persistent descriptor region", configuration_.persistent_descriptor_count));
        }
        device_.native().CreateShaderResourceView(&resource, &view, staging_cpu_handle({ *index }));
        device_.native().CopyDescriptorsSimple(
            1,
            shader_visible_cpu_handle({ *index }),
            staging_cpu_handle({ *index }),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return descriptor_lease{ *this, *index, descriptor_lease::pool::persistent };
    }

    result<descriptor_lease>
        descriptor_heaps::create_uav(ID3D12Resource& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& view)
    {
        const std::optional<std::uint32_t> index = persistent_free_.pop();
        if (!index)
        {
            return std::unexpected(
                heap_full("the persistent descriptor region", configuration_.persistent_descriptor_count));
        }
        // The staging copy stays: ClearUnorderedAccessView* needs a CPU handle
        device_.native().CreateUnorderedAccessView(&resource, nullptr, &view, staging_cpu_handle({ *index }));
        device_.native().CopyDescriptorsSimple(
            1,
            shader_visible_cpu_handle({ *index }),
            staging_cpu_handle({ *index }),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return descriptor_lease{ *this, *index, descriptor_lease::pool::persistent };
    }

    result<std::unique_ptr<descriptor_range_state>> descriptor_heaps::create_range(const std::uint32_t count)
    {
        if (count < 2)
        {
            return std::unexpected(make_error(
                error_kind::invalid_argument,
                std::format(
                    "descriptor range: count is {}, it must be at least 2 (one is the null descriptor)", count)));
        }

        std::uint32_t first = 0;
        {
            const std::lock_guard lock{ range_mutex_ };
            const auto found =
                std::ranges::find_if(free_ranges_, [&](const auto& range) { return range.second >= count; });
            if (found == free_ranges_.end())
            {
                return std::unexpected(heap_full(
                    std::format("the transient descriptor region (no {} free descriptors in a row)", count),
                    configuration_.transient_descriptor_count));
            }
            first = found->first;
            found->first += count;
            found->second -= count;
            if (found->second == 0)
            {
                free_ranges_.erase(found);
            }
        }

        write_null_srv(first, false);
        return std::make_unique<descriptor_range_state>(*this, first, count);
    }

    void descriptor_heaps::free_range(descriptor_range_state& range) noexcept
    {
        const std::lock_guard lock{ range_mutex_ };
        // Sorted insert, then merge with the neighbours
        auto position =
            std::ranges::lower_bound(free_ranges_, range.first, {}, [](const auto& entry) { return entry.first; });
        position = free_ranges_.insert(position, { range.first, range.count }); // out of memory terminates (§23.8)
        if (const auto next = position + 1;
            next != free_ranges_.end() && position->first + position->second == next->first)
        {
            position->second += next->second;
            free_ranges_.erase(next);
        }
        if (position != free_ranges_.begin())
        {
            const auto previous = position - 1;
            if (previous->first + previous->second == position->first)
            {
                previous->second += position->second;
                free_ranges_.erase(position);
            }
        }
    }

    result<bindless_index> descriptor_heaps::create_transient_srv(
        descriptor_range_state& range, ID3D12Resource& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view)
    {
        const std::uint32_t offset_in_range = range.used.fetch_add(1, std::memory_order_relaxed);
        if (offset_in_range >= range.count)
        {
            return std::unexpected(make_error(
                error_kind::transient_memory_exhausted,
                std::format("descriptor range is full ({} descriptors)", range.count)));
        }
        const bindless_index index{ range.first + offset_in_range };
        device_.native().CreateShaderResourceView(&resource, &view, shader_visible_cpu_handle(index));
        return index;
    }

    result<bindless_index> descriptor_heaps::create_transient_uav(
        descriptor_range_state& range, ID3D12Resource& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& view)
    {
        const std::uint32_t offset_in_range = range.used.fetch_add(1, std::memory_order_relaxed);
        if (offset_in_range >= range.count)
        {
            return std::unexpected(make_error(
                error_kind::transient_memory_exhausted,
                std::format("descriptor range is full ({} descriptors)", range.count)));
        }
        const bindless_index index{ range.first + offset_in_range };
        device_.native().CreateUnorderedAccessView(&resource, nullptr, &view, shader_visible_cpu_handle(index));
        device_.native().CreateUnorderedAccessView(&resource, nullptr, &view, staging_cpu_handle(index));
        return index;
    }

    result<descriptor_lease> descriptor_heaps::create_sampler(const D3D12_SAMPLER_DESC& sampler)
    {
        const std::optional<std::uint32_t> index = sampler_free_.pop();
        if (!index)
        {
            return std::unexpected(heap_full("the sampler heap", configuration_.sampler_count));
        }
        device_.native().CreateSampler(&sampler, sampler_cpu_handle({ *index }));
        return descriptor_lease{ *this, *index, descriptor_lease::pool::sampler };
    }

    result<D3D12_CPU_DESCRIPTOR_HANDLE>
        descriptor_heaps::create_rtv(ID3D12Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC& view)
    {
        const std::optional<std::uint32_t> index = render_target_free_.pop();
        if (!index)
        {
            return std::unexpected(heap_full("the RTV heap", configuration_.render_target_view_count));
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = offset(render_target_cpu_start_, *index, render_target_increment_);
        device_.native().CreateRenderTargetView(&resource, &view, handle);
        return handle;
    }

    result<D3D12_CPU_DESCRIPTOR_HANDLE>
        descriptor_heaps::create_dsv(ID3D12Resource& resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& view)
    {
        const std::optional<std::uint32_t> index = depth_stencil_free_.pop();
        if (!index)
        {
            return std::unexpected(heap_full("the DSV heap", configuration_.depth_stencil_view_count));
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = offset(depth_stencil_cpu_start_, *index, depth_stencil_increment_);
        device_.native().CreateDepthStencilView(&resource, &view, handle);
        return handle;
    }

    void descriptor_heaps::free_rtv(const D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
    {
        render_target_free_.push(
            static_cast<std::uint32_t>((handle.ptr - render_target_cpu_start_.ptr) / render_target_increment_));
    }

    void descriptor_heaps::free_dsv(const D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
    {
        depth_stencil_free_.push(
            static_cast<std::uint32_t>((handle.ptr - depth_stencil_cpu_start_.ptr) / depth_stencil_increment_));
    }

    void descriptor_heaps::free_persistent(const bindless_index index) noexcept
    {
        persistent_free_.push(index.value);
    }

    void descriptor_heaps::free_sampler(const bindless_index index) noexcept
    {
        sampler_free_.push(index.value);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_heaps::staging_cpu_handle(const bindless_index index) const noexcept
    {
        return offset(staging_cpu_start_, index.value, resource_increment_);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_heaps::shader_visible_cpu_handle(const bindless_index index) const noexcept
    {
        return offset(resource_cpu_start_, index.value, resource_increment_);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE descriptor_heaps::shader_visible_gpu_handle(const bindless_index index) const noexcept
    {
        return { resource_gpu_start_.ptr + static_cast<UINT64>(index.value) * resource_increment_ };
    }

    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_heaps::sampler_cpu_handle(const bindless_index index) const noexcept
    {
        return offset(sampler_cpu_start_, index.value, sampler_increment_);
    }

    std::array<ID3D12DescriptorHeap*, 2> descriptor_heaps::shader_visible_heaps() noexcept
    {
        return { resource_heap_.get(), sampler_heap_.get() };
    }

    const descriptor_heap_configuration& descriptor_heaps::configuration() const noexcept
    {
        return configuration_;
    }

    std::uint32_t descriptor_heaps::free_persistent_count() const noexcept
    {
        return persistent_free_.count();
    }

    std::uint32_t descriptor_heaps::free_sampler_count() const noexcept
    {
        return sampler_free_.count();
    }

    // ---- view_cache ----

    namespace
    {
        constexpr std::uint64_t depth_stencil_key = std::uint64_t{ 1 } << 63;

        std::uint64_t view_key(const texture_view& view, const bool depth) noexcept
        {
            return (depth ? depth_stencil_key : 0) | (static_cast<std::uint64_t>(view.format) << 48) |
                   (static_cast<std::uint64_t>(view.layer_count) << 32) |
                   (static_cast<std::uint64_t>(view.first_layer) << 16) | view.mip;
        }

        DXGI_FORMAT view_format(const texture_description& texture, const texture_view& view) noexcept
        {
            return to_native(view.format == format::unknown ? texture.format : view.format);
        }
    } // namespace

    view_cache::view_cache(descriptor_heaps& heaps) noexcept : heaps_{ heaps } {}

    view_cache::~view_cache()
    {
        for (const entry& cached : entries_)
        {
            if ((cached.key & depth_stencil_key) != 0)
            {
                heaps_.free_dsv(cached.handle);
            }
            else
            {
                heaps_.free_rtv(cached.handle);
            }
        }
    }

    result<D3D12_CPU_DESCRIPTOR_HANDLE> view_cache::render_target(
        ID3D12Resource& resource, const texture_description& texture, const texture_view& view)
    {
        const std::uint64_t key = view_key(view, false);
        const std::lock_guard lock{ mutex_ };
        for (const entry& cached : entries_)
        {
            if (cached.key == key)
            {
                return cached.handle;
            }
        }

        const bool array = array_size(texture) > 1;
        D3D12_RENDER_TARGET_VIEW_DESC description{ .Format = view_format(texture, view) };
        switch (texture.dimension)
        {
        case texture_dimension::texture_1d:
            if (array)
            {
                description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                description.Texture1DArray = { .MipSlice = view.mip,
                                               .FirstArraySlice = view.first_layer,
                                               .ArraySize = view.layer_count };
            }
            else
            {
                description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                description.Texture1D = { .MipSlice = view.mip };
            }
            break;
        case texture_dimension::texture_3d:
            description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            description.Texture3D = { .MipSlice = view.mip,
                                      .FirstWSlice = view.first_layer,
                                      .WSize = view.layer_count };
            break;
        case texture_dimension::texture_2d:
        case texture_dimension::cube:
            if (array)
            {
                description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                description.Texture2DArray = { .MipSlice = view.mip,
                                               .FirstArraySlice = view.first_layer,
                                               .ArraySize = view.layer_count,
                                               .PlaneSlice = 0 };
            }
            else
            {
                description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                description.Texture2D = { .MipSlice = view.mip, .PlaneSlice = 0 };
            }
            break;
        }

        auto handle = heaps_.create_rtv(resource, description);
        if (handle)
        {
            entries_.push_back({ key, *handle });
        }
        return handle;
    }

    result<D3D12_CPU_DESCRIPTOR_HANDLE> view_cache::depth_stencil(
        ID3D12Resource& resource, const texture_description& texture, const texture_view& view)
    {
        const std::uint64_t key = view_key(view, true);
        const std::lock_guard lock{ mutex_ };
        for (const entry& cached : entries_)
        {
            if (cached.key == key)
            {
                return cached.handle;
            }
        }

        const bool array = array_size(texture) > 1;
        D3D12_DEPTH_STENCIL_VIEW_DESC description{ .Format = view_format(texture, view), .Flags = D3D12_DSV_FLAG_NONE };
        switch (texture.dimension)
        {
        case texture_dimension::texture_1d:
            if (array)
            {
                description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                description.Texture1DArray = { .MipSlice = view.mip,
                                               .FirstArraySlice = view.first_layer,
                                               .ArraySize = view.layer_count };
            }
            else
            {
                description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                description.Texture1D = { .MipSlice = view.mip };
            }
            break;
        case texture_dimension::texture_3d:
            return std::unexpected(make_error(
                error_kind::invalid_argument, "depth_stencil: a 3D texture can't have a depth-stencil view"));
        case texture_dimension::texture_2d:
        case texture_dimension::cube:
            if (array)
            {
                description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                description.Texture2DArray = { .MipSlice = view.mip,
                                               .FirstArraySlice = view.first_layer,
                                               .ArraySize = view.layer_count };
            }
            else
            {
                description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                description.Texture2D = { .MipSlice = view.mip };
            }
            break;
        }

        auto handle = heaps_.create_dsv(resource, description);
        if (handle)
        {
            entries_.push_back({ key, *handle });
        }
        return handle;
    }

    std::size_t view_cache::size() const noexcept
    {
        const std::lock_guard lock{ mutex_ };
        return entries_.size();
    }
} // namespace avis::rhi::d3d12
