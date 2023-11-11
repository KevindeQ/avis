#include "avis/runtime/graphics/resource.h"

namespace graphics
{
    basic_resource::basic_resource() : descriptor_index_{std::numeric_limits<std::size_t>::infinity()}, alias_{false} {}

    basic_resource::basic_resource(std::size_t descriptor_index) : descriptor_index_{descriptor_index}, alias_{false} {}

    basic_resource::basic_resource(std::size_t descriptor_index, bool alias) :
        descriptor_index_{descriptor_index},
        alias_{alias}
    {}

    resource::resource() : basic_resource{}, reference_count_{0} {}

    resource::resource(std::size_t descriptor_index) : basic_resource(descriptor_index), reference_count_{0} {}

    resource::resource(const resource& other) :
        basic_resource(descriptor_index(), true),
        reference_count_{other.reference_count_}
    {}

    void resource::add_ref()
    {
        *reference_count_ += 1;
    }

    void resource::release()
    {
        *reference_count_ -= 1;
    }

    resource& resource::operator=(const resource& other)
    {
        basic_resource::operator=(other);
        reference_count_ = other.reference_count_;

        return *this;
    }
} // namespace graphics
