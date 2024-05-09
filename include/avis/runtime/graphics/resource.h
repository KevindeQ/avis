#ifndef RUNTIME_GRAPHICS_RESOURCE_H
#define RUNTIME_GRAPHICS_RESOURCE_H

#include "avis/core/common.h"
#include "avis/core/dynamic_bitset.h"

namespace graphics
{
    class memory_pool_vram
    {
    public:
    private:
    };

    class vram_allocator
    {
    public:
        memory_pool_vram allocate();
        void deallocate(const memory_pool_vram& pool);
    };

    class resource
    {
    public:
    };

    class static_resource_allocator
    {
    public:
        resource allocate();
        void dealloate(const resource& res);

    private:

        dynamic_bitset<> map_buddy_allocated
        dynamic_bitset<> map_block_split;
    };

    class dynamic_resource_allocator
    {
    public:
        dynamic_resource_allocator();
        ~dynamic_resource_allocator();
    };

    /*struct resource_descriptor
    {
    public:
    private:
    };

    class basic_virtual_resource
    {
    public:
        basic_virtual_resource();
        explicit basic_virtual_resource(std::size_t descriptor_index);
        basic_virtual_resource(std::size_t descriptor_index, bool alias);
        basic_virtual_resource(const basic_virtual_resource&) = default;
        basic_virtual_resource(basic_virtual_resource&&) = default;
        virtual ~basic_virtual_resource() = default;

        std::size_t descriptor_index() const
        {
            return descriptor_index_;
        }

        bool alias() const
        {
            return alias_;
        }

        basic_virtual_resource& operator=(const basic_virtual_resource&) = default;
        basic_virtual_resource& operator=(basic_virtual_resource&&) = default;

    private:
        std::size_t descriptor_index_;
        bool alias_;
    };

    class virtual_resource : public basic_virtual_resource
    {
    public:
        virtual_resource();
        explicit virtual_resource(std::size_t descriptor_index);
        virtual_resource(const virtual_resource& other);
        virtual_resource(virtual_resource&&) = default;
        virtual ~virtual_resource() = default;

        void add_ref();
        void release();

        std::size_t ref_count() const
        {
            return *reference_count_;
        }

        virtual_resource& operator=(const virtual_resource& other);
        virtual_resource& operator=(virtual_resource&&) = default;

        bool operator==(const virtual_resource& other) const
        {
            return descriptor_index() == other.descriptor_index();
        }

    private:
        std::shared_ptr<std::size_t> reference_count_;
    };

    class resource
    {
    public:
    private:
        com_ptr<ID3D12Resource> native_resource;
    };

    class vertex_buffer : public resource
    {
    public:
    private:
        D3D12_VERTEX_BUFFER_VIEW view;
    };

    class index_buffer : public resource
    {
    public:
    private:
        D3D12_INDEX_BUFFER_VIEW view;
    };

    class constant_buffer : public resource
    {
    public:
        D3D12_CPU_DESCRIPTOR_HANDLE constant_buffer_view() const;

    private:
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle_cbv;
    };

    class texture_2d : public resource
    {
    public:
        D3D12_CPU_DESCRIPTOR_HANDLE shader_resource_view() const;
        D3D12_CPU_DESCRIPTOR_HANDLE unorderend_access_view() const;

    private:
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle_srv;
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle_uav;
    };*/
} // namespace graphics

#endif
