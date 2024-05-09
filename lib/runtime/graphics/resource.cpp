#include "avis/runtime/graphics/resource.h"

namespace graphics
{
    /*basic_virtual_resource::basic_virtual_resource() : descriptor_index_{std::numeric_limits<std::size_t>::infinity()}, alias_{false} {}

    basic_virtual_resource::basic_virtual_resource(std::size_t descriptor_index) : descriptor_index_{descriptor_index}, alias_{false} {}

    basic_virtual_resource::basic_virtual_resource(std::size_t descriptor_index, bool alias) :
        descriptor_index_{descriptor_index},
        alias_{alias}
    {}

    virtual_resource::virtual_resource() : basic_virtual_resource{}, reference_count_{0} {}

    virtual_resource::virtual_resource(std::size_t descriptor_index) : basic_virtual_resource(descriptor_index), reference_count_{0} {}

    virtual_resource::virtual_resource(const virtual_resource& other) :
        basic_virtual_resource(descriptor_index(), true),
        reference_count_{other.reference_count_}
    {}

    void virtual_resource::add_ref()
    {
        *reference_count_ += 1;
    }

    void virtual_resource::release()
    {
        *reference_count_ -= 1;
    }

    virtual_resource& virtual_resource::operator=(const virtual_resource& other)
    {
        basic_virtual_resource::operator=(other);
        reference_count_ = other.reference_count_;

        return *this;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE constant_buffer::constant_buffer_view() const
    {
        return descriptor_handle_cbv;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE texture_2d::shader_resource_view() const
    {
        return descriptor_handle_srv;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE texture_2d::unorderend_access_view() const
    {
        return descriptor_handle_uav;
    }*/
} // namespace graphics
