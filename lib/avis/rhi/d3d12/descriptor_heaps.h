#ifndef AVIS_RHI_D3D12_DESCRIPTOR_HEAPS_H
#define AVIS_RHI_D3D12_DESCRIPTOR_HEAPS_H

// The five descriptor heaps behind bindless indices, render targets and depth views (docs/design/09-rhi.md §9,
// §22.2): persistent and transient regions of one shader-visible CBV/SRV/UAV heap with its CPU staging mirror, the
// sampler heap, and the CPU-only RTV and DSV heaps.

#include "avis/error.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/leaf_mutex.h"
#include "avis/rhi/d3d12/transient_links.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace avis::rhi::d3d12
{
    class descriptor_heaps;
    class device;
    class resource_manager;

    // Lock-free stack of free indices with a tag against ABA (§9.2): a 64-bit {index, tag} head, and next links in an
    // array parallel to the heap
    class index_free_list
    {
    public:
        // Every index in [begin, end) is free; indices below `begin` (null descriptors) are never handed out
        void initialise(std::uint32_t begin, std::uint32_t end);

        std::optional<std::uint32_t> pop() noexcept;
        void push(std::uint32_t index) noexcept;

        // Walks the list: tests only, with no concurrent pop or push
        std::uint32_t count() const noexcept;

    private:
        static constexpr std::uint32_t none = 0xFFFF'FFFF;

        std::atomic<std::uint64_t> head_{ none }; // low 32 bits: index; high 32 bits: tag
        std::unique_ptr<std::atomic<std::uint32_t>[]> next_;
    };

    // Owns one index of the persistent region or of the sampler heap, and gives it back to its free list when
    // destroyed. Records hold leases, so freeing a record frees its indices (§23.8, D-91). Move-only; a moved-from or
    // default lease owns nothing.
    class descriptor_lease
    {
    public:
        descriptor_lease() noexcept = default;
        descriptor_lease(descriptor_lease&& other) noexcept;
        descriptor_lease& operator=(descriptor_lease&& other) noexcept;
        descriptor_lease(const descriptor_lease&) = delete;
        descriptor_lease& operator=(const descriptor_lease&) = delete;
        ~descriptor_lease();

        // 0 (the null descriptor) when empty
        bindless_index index() const noexcept;

    private:
        friend class descriptor_heaps;

        enum class pool : std::uint8_t
        {
            persistent,
            sampler
        };

        descriptor_lease(descriptor_heaps& heaps, std::uint32_t index, pool kind) noexcept;
        void release() noexcept;

        descriptor_heaps* heaps_ = nullptr;
        bindless_index index_;
        pool pool_ = pool::persistent;
    };

    // Behind descriptor_range (§6): a bump allocator over [first, first + count). Index `first` is a null descriptor.
    // Holds an atomic, so it's never moved: created and released through a unique_ptr (create_range, §23.8). Its
    // destructor returns the range.
    struct descriptor_range_state
    {
        descriptor_range_state(descriptor_heaps& owner, std::uint32_t first_index, std::uint32_t index_count) noexcept;
        ~descriptor_range_state();

        descriptor_range_state(const descriptor_range_state&) = delete;
        descriptor_range_state& operator=(const descriptor_range_state&) = delete;

        transient_links links; // reset check, deferred release (§23.6); set by the transient_objects view (6.9)
        descriptor_heaps* heaps;
        const resource_manager* resources = nullptr; // resolves texture/buffer handles; set by the view (6.9)
        std::uint32_t first;
        std::uint32_t count;
        std::atomic<std::uint32_t> used{ 1 }; // past the null descriptor
    };

    class descriptor_heaps
    {
    public:
        static result<std::unique_ptr<descriptor_heaps>>
            create(device& device, const descriptor_heap_configuration& configuration);

        descriptor_heaps(const descriptor_heaps&) = delete;
        descriptor_heaps& operator=(const descriptor_heaps&) = delete;

        // ---- persistent region: written to the staging heap, then copied to the shader-visible heap (§9.2) ----
        result<descriptor_lease> create_srv(ID3D12Resource& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view);
        result<descriptor_lease> create_uav(ID3D12Resource& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& view);

        // ---- transient region: carved into descriptor ranges (§6) ----
        // First fit under range_mutex_; `count` includes the range's null descriptor, so it must be at least 2
        result<std::unique_ptr<descriptor_range_state>> create_range(std::uint32_t count);
        // Written straight into the shader-visible heap; UAVs also into the staging heap at the same index (D-89)
        result<bindless_index> create_transient_srv(
            descriptor_range_state& range, ID3D12Resource& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view);
        result<bindless_index> create_transient_uav(
            descriptor_range_state& range, ID3D12Resource& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& view);

        // ---- sampler heap, CPU-only RTV and DSV heaps: free lists ----
        result<descriptor_lease> create_sampler(const D3D12_SAMPLER_DESC& sampler);
        result<D3D12_CPU_DESCRIPTOR_HANDLE>
            create_rtv(ID3D12Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC& view);
        result<D3D12_CPU_DESCRIPTOR_HANDLE>
            create_dsv(ID3D12Resource& resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& view);
        void free_rtv(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept; // from ~view_cache only (§22.2.3)
        void free_dsv(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;

        // For UAV clears: persistent and transient indices alike (the staging heap mirrors both regions, D-89)
        D3D12_CPU_DESCRIPTOR_HANDLE staging_cpu_handle(bindless_index index) const noexcept;
        D3D12_CPU_DESCRIPTOR_HANDLE shader_visible_cpu_handle(bindless_index index) const noexcept;
        D3D12_GPU_DESCRIPTOR_HANDLE shader_visible_gpu_handle(bindless_index index) const noexcept;
        D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle(bindless_index index) const noexcept;
        std::array<ID3D12DescriptorHeap*, 2> shader_visible_heaps() noexcept; // SetDescriptorHeaps on acquire

        const descriptor_heap_configuration& configuration() const noexcept;

        // White-box checks: free indices left (walks the lists; no concurrent use)
        std::uint32_t free_persistent_count() const noexcept;
        std::uint32_t free_sampler_count() const noexcept;

    private:
        friend class descriptor_lease;
        friend struct descriptor_range_state;

        descriptor_heaps(device& device, const descriptor_heap_configuration& configuration) noexcept;

        void free_persistent(bindless_index index) noexcept; // from ~descriptor_lease
        void free_sampler(bindless_index index) noexcept; // from ~descriptor_lease
        void free_range(descriptor_range_state& range) noexcept; // from ~descriptor_range_state

        // A null Texture2D SRV at `index` of the shader-visible heap (and of the staging heap when asked)
        void write_null_srv(std::uint32_t index, bool staging) noexcept;

        device& device_;
        descriptor_heap_configuration configuration_;
        com_ptr<ID3D12DescriptorHeap> resource_heap_; // shader-visible, persistent + transient
        com_ptr<ID3D12DescriptorHeap> resource_staging_heap_; // CPU-only mirror of both regions, same indices
        com_ptr<ID3D12DescriptorHeap> sampler_heap_; // shader-visible
        com_ptr<ID3D12DescriptorHeap> render_target_heap_;
        com_ptr<ID3D12DescriptorHeap> depth_stencil_heap_;
        std::uint32_t resource_increment_ = 0;
        std::uint32_t sampler_increment_ = 0;
        std::uint32_t render_target_increment_ = 0;
        std::uint32_t depth_stencil_increment_ = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE resource_cpu_start_{};
        D3D12_GPU_DESCRIPTOR_HANDLE resource_gpu_start_{};
        D3D12_CPU_DESCRIPTOR_HANDLE staging_cpu_start_{};
        D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_start_{};
        D3D12_CPU_DESCRIPTOR_HANDLE render_target_cpu_start_{};
        D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_cpu_start_{};
        index_free_list persistent_free_;
        index_free_list sampler_free_;
        index_free_list render_target_free_;
        index_free_list depth_stencil_free_;
        leaf_mutex range_mutex_{ "descriptor_heaps::range_mutex_" };
        std::vector<std::pair<std::uint32_t, std::uint32_t>> free_ranges_; // (first, count), sorted by first
    };

    // Lazily created RTV/DSV per (mip, first_layer, layer_count, format) of one texture (§8.3, §22.2.3). Lives in the
    // texture record through a unique_ptr, because it holds a mutex.
    class view_cache
    {
    public:
        explicit view_cache(descriptor_heaps& heaps) noexcept;

        // Frees every entry. Runs when the texture record is destroyed: in the deferred release, after every list that
        // could use the texture has completed, so no recording thread is inside the cache and no lock is taken.
        ~view_cache();

        view_cache(const view_cache&) = delete;
        view_cache& operator=(const view_cache&) = delete;

        result<D3D12_CPU_DESCRIPTOR_HANDLE>
            render_target(ID3D12Resource& resource, const texture_description& texture, const texture_view& view);
        result<D3D12_CPU_DESCRIPTOR_HANDLE>
            depth_stencil(ID3D12Resource& resource, const texture_description& texture, const texture_view& view);

        // Entries created so far (tests)
        std::size_t size() const noexcept;

    private:
        struct entry
        {
            std::uint64_t key;
            D3D12_CPU_DESCRIPTOR_HANDLE handle;
        };

        descriptor_heaps& heaps_;
        mutable leaf_mutex mutex_{ "view_cache::mutex_" };
        std::vector<entry> entries_; // key bit 63: depth-stencil
    };
} // namespace avis::rhi::d3d12

#endif
