#ifndef RUNTIME_GRAPHICS_RESOURCE_H
#define RUNTIME_GRAPHICS_RESOURCE_H

#include "avis/core/common.h"

namespace graphics
{
    struct resource_descriptor
    {
    public:
    private:
    };

    class basic_resource
    {
    public:
        basic_resource();
        explicit basic_resource(std::size_t descriptor_index);
        basic_resource(std::size_t descriptor_index, bool alias);
        basic_resource(const basic_resource&) = default;
        basic_resource(basic_resource&&) = default;
        virtual ~basic_resource() = default;

        std::size_t descriptor_index() const
        {
            return descriptor_index_;
        }

        bool alias() const
        {
            return alias_;
        }

        basic_resource& operator=(const basic_resource&) = default;
        basic_resource& operator=(basic_resource&&) = default;

    private:
        std::size_t descriptor_index_;
        bool alias_;
    };

    class resource : public basic_resource
    {
    public:
        ENGINE_API resource();
        ENGINE_API explicit resource(std::size_t descriptor_index);
        ENGINE_API resource(const resource& other);
        ENGINE_API resource(resource&&) = default;
        ENGINE_API virtual ~resource() = default;

        void add_ref();
        void release();

        std::size_t ref_count() const
        {
            return *reference_count_;
        }

        resource& operator=(const resource& other);
        resource& operator=(resource&&) = default;

        bool operator==(const resource& other) const
        {
            return descriptor_index() == other.descriptor_index();
        }

    private:
        std::shared_ptr<std::size_t> reference_count_;
    };
} // namespace graphics

#endif
