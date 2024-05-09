#ifndef RUNTIME_GRAPHICS_DESCRIPTORS_H
#define RUNTIME_GRAPHICS_DESCRIPTORS_H

#include "avis/core/common.h"

namespace graphics
{
    class descriptor_handle
    {
    public:
        descriptor_handle(const std::uint32_t descriptor_index);

    private:
        std::uint32_t heap_descriptor_index;
    };

    class cpu_descriptor_handle : public descriptor_handle
    {
    public:
        cpu_descriptor_handle(const std::size_t ptr, const std::uint32_t descriptor_index);

    private:
        D3D12_CPU_DESCRIPTOR_HANDLE native_handle;
    };

    class gpu_descriptor_handle : public descriptor_handle
    {
    public:
        gpu_descriptor_handle(const std::uint64_t ptr, const std::uint32_t descriptor_index);

    private:
        D3D12_GPU_DESCRIPTOR_HANDLE native_handle;
    };

    enum class descriptor_heap_type
    {
        cbv_srv_uav,
        sampler,
        rtv,
        dsv,
    };

    class descriptor_heap
    {
    public:
        descriptor_heap(const com_ptr<ID3D12DescriptorHeap> heap);
        descriptor_heap(const descriptor_heap& other);
        descriptor_heap(descriptor_heap&& other);

        descriptor_heap& operator=(const descriptor_heap& rhs);
        descriptor_heap& operator=(descriptor_heap&& rhs);

        descriptor_heap_type heap_type() const;

        std::size_t size() const;

    private:
        com_ptr<ID3D12DescriptorHeap> native_descriptor_heap;

        descriptor_heap_type type;
        bool shader_visible;
    };

    template<typename handle_t>
    concept descriptor_handle_type = std::is_base_of_v<descriptor_handle, handle_t>;

    /*template<typename policy>
    concept descriptor_allocation_policy = requires(policy p) {

    };*/

    template<descriptor_handle_type handle_t/*, descriptor_allocation_policy allocation_policy*/>
    class descriptor_allocator /*: protected allocation_policy*/
    {
    public:
        using handle_type = handle_t;

        descriptor_allocator();
        descriptor_allocator(const descriptor_allocator<handle_type>&) = delete;
        descriptor_allocator(descriptor_allocator&& other);
        ~descriptor_allocator();

        descriptor_allocator<handle_type>& operator=(const descriptor_allocator<handle_type>&) = delete;
        descriptor_allocator<handle_type>& operator=(descriptor_allocator<handle_type>&&);

        handle_type allocate();
        void deallocate(const handle_type& handle);

    private:
        descriptor_heap heap;

        std::atomic<std::uint32_t> next_free_slot_index;
    };

    template<descriptor_handle_type handle_t/*, descriptor_allocation_policy allocation_policy*/>
    descriptor_allocator<handle_t /*, allocation_policy*/>::descriptor_allocator() : next_free_slot_index{ 0 }
    {

    }

    template<descriptor_handle_type handle_t/*, descriptor_allocation_policy allocation_policy*/>
    descriptor_allocator<handle_t /*, allocation_policy*/>::~descriptor_allocator()
    {

    }

    template<descriptor_handle_type handle_t/*, descriptor_allocation_policy allocation_policy*/>
    descriptor_allocator<handle_t /*, allocation_policy*/>::handle_type
        descriptor_allocator<handle_t /*, allocation_policy*/>::allocate()
    {
        std::uint32_t descriptor_slot_index = next_free_slot_index.fetch_add(1u);
        if (descriptor_slot_index > heap.size())
        {

        }
    }
    
    template<descriptor_handle_type handle_t/*, descriptor_allocation_policy allocation_policy*/>
    void descriptor_allocator<handle_t /*, allocation_policy*/>::deallocate(
        const typename descriptor_allocator<handle_t /*, allocation_policy*/>::handle_type& handle)
    {
        throw std::runtime_error{ "Linear allocation of descriptors doesn't support deallocation" };
    }

} // namespace graphics

#endif
