#include "avis/runtime/graphics/descriptors.h"

namespace graphics
{
    descriptor_handle::descriptor_handle(const std::uint32_t descriptor_index) :
        heap_descriptor_index{ descriptor_index }
    {}

    cpu_descriptor_handle::cpu_descriptor_handle(const std::size_t ptr, const std::uint32_t descriptor_index) :
        descriptor_handle(descriptor_index),
        native_handle{ .ptr = ptr }
    {}

    gpu_descriptor_handle::gpu_descriptor_handle(const std::uint64_t ptr, const std::uint32_t descriptor_index) :
        descriptor_handle(descriptor_index),
        native_handle{ .ptr = ptr }
    {}

    descriptor_heap::descriptor_heap(const com_ptr<ID3D12DescriptorHeap> heap) :
        native_descriptor_heap{ heap },
        type{ descriptor_heap_type::cbv_srv_uav },
        shader_visible{ false }
    {
        D3D12_DESCRIPTOR_HEAP_DESC descriptor = native_descriptor_heap->GetDesc();
        switch (descriptor.Type)
        {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
            type = descriptor_heap_type::cbv_srv_uav;
            break;

        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
            type = descriptor_heap_type::sampler;
            break;

        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
            type = descriptor_heap_type::rtv;
            break;

        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
            type = descriptor_heap_type::dsv;
            break;

        default:
            throw std::runtime_error{ "Encountered invalid heap type" };
        }

        shader_visible = (descriptor.Flags & D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ==
                         D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    }

    descriptor_heap_type descriptor_heap::heap_type() const
    {
        return descriptor_heap_type::cbv_srv_uav;
    }

    std::size_t descriptor_heap::size() const
    {
        return 0;
    }
} // namespace graphics
