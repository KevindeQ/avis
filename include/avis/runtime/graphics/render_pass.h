#ifndef RUNTIME_GRAPHICS_RENDER_PASS_H
#define RUNTIME_GRAPHICS_RENDER_PASS_H

#include "avis/core/common.h"
#include "avis/runtime/graphics/command_context.h"
#include "avis/runtime/graphics/render_pass_builder.h"
#include "avis/runtime/graphics/resource_graph.h"

namespace graphics
{
    class render_pass_resources
    {
    public:
    };

    class render_pass
    {
    public:
        template<typename render_pass_data>
        using config_callback_type = std::function<void(render_pass_builder&, render_pass_data&)>;

        template<typename render_pass_data>
        using graphics_exec_callback_type =
            std::function<void(const render_pass_data&, render_pass_resources&, graphics_context&)>;

        template<typename render_pass_data>
        using compute_exec_callback_type =
            std::function<void(const render_pass_data&, render_pass_resources&, compute_context&)>;

        template<typename render_pass_data>
        using copy_exec_callback_type =
            std::function<void(const render_pass_data&, render_pass_resources&, copy_context&)>;

    private:
        using internal_exec_callback_type = std::function<void(render_pass_resources&)>;

    public:
        template<typename render_pass_data>
        render_pass(
            const std::string& name,
            render_pass_data& pass_data,
            graphics_exec_callback_type<render_pass_data> exec_callback);

        template<typename render_pass_data>
        render_pass(
            const std::string& name,
            render_pass_data& pass_data,
            compute_exec_callback_type<render_pass_data> exec_callback);

        template<typename render_pass_data>
        render_pass(
            const std::string& name,
            render_pass_data& pass_data,
            copy_exec_callback_type<render_pass_data> exec_callback);

        void execute(render_pass_resources& resources)
        {
            exec_callback_(resources);
        }

        void reference_resource(resource handle);

        void add_ref();
        void release();
        std::size_t ref_count() const
        {
            return reference_count_;
        }

    private:
        template<typename render_pass_data>
        void store_render_pass_data(render_pass_data& pass_data);

        template<typename render_pass_data>
        internal_exec_callback_type
            convert_to_internal_callback(graphics_exec_callback_type<render_pass_data> exec_callback);

        template<typename render_pass_data>
        internal_exec_callback_type
            convert_to_internal_callback(compute_exec_callback_type<render_pass_data> exec_callback);

        template<typename render_pass_data>
        internal_exec_callback_type
            convert_to_internal_callback(copy_exec_callback_type<render_pass_data> exec_callback);

    private:
        std::string name_;
        std::aligned_storage<64, alignof(std::max_align_t)>::type data_;
        internal_exec_callback_type exec_callback_;

        std::size_t reference_count_;
        std::vector<resource> resources_;
    };

    template<typename render_pass_data>
    inline render_pass::render_pass(
        const std::string& name,
        render_pass_data& pass_data,
        graphics_exec_callback_type<render_pass_data> exec_callback) :
        name_{name},
        data_{},
        exec_callback_{},
        reference_count_{0}
    {
        store_render_pass_data<render_pass_data>(pass_data);
        exec_callback_ = convert_to_internal_callback(exec_callback);
    }

    template<typename render_pass_data>
    inline render_pass::render_pass(
        const std::string& name,
        render_pass_data& pass_data,
        compute_exec_callback_type<render_pass_data> exec_callback) :
        name_{name},
        data_{},
        exec_callback_{},
        reference_count_{0}
    {
        store_render_pass_data<render_pass_data>(pass_data);
        exec_callback_ = convert_to_internal_callback(exec_callback);
    }

    template<typename render_pass_data>
    inline render_pass::render_pass(
        const std::string& name, render_pass_data& pass_data, copy_exec_callback_type<render_pass_data> exec_callback) :
        name_{name},
        data_{},
        exec_callback_{},
        reference_count_{0}
    {
        store_render_pass_data<render_pass_data>(pass_data);
        exec_callback_ = convert_to_internal_callback(exec_callback);
    }

    template<typename render_pass_data>
    inline void render_pass::store_render_pass_data(render_pass_data& pass_data)
    {
        static_assert(
            (alignof(render_pass_data) - 1 + sizeof(render_pass_data)) <= sizeof(data_),
            "render_pass_data can't be stored in the provided storage space because the type size is too large.");
        static_assert(
            alignof(render_pass_data) <= alignof(std::max_align_t),
            "alignment of render_pass_data exceeds max. supported alignment. The max support aligned equals "
            "the alignent of std::max_align_t.");

        new (&data_) render_pass_data{pass_data};
    }

    template<typename render_pass_data>
    inline render_pass::internal_exec_callback_type
        render_pass::convert_to_internal_callback(graphics_exec_callback_type<render_pass_data> exec_callback)
    {
        return [this, exec_callback](render_pass_resources& resources)
        {
            auto context = graphics_context::begin(name_);
            exec_callback(*std::launder(reinterpret_cast<render_pass_data*>(&data_)), resources, context);
        };
    }

    template<typename render_pass_data>
    inline render_pass::internal_exec_callback_type
        render_pass::convert_to_internal_callback(compute_exec_callback_type<render_pass_data> exec_callback)
    {
        return [this, exec_callback](render_pass_resources& resources)
        {
            auto context = compute_context::begin(name_);
            exec_callback(*std::launder(reinterpret_cast<render_pass_data*>(&data_)), resources, context);
        };
    }

    template<typename render_pass_data>
    inline render_pass::internal_exec_callback_type
        render_pass::convert_to_internal_callback(copy_exec_callback_type<render_pass_data> exec_callback)
    {
        return [this, exec_callback](render_pass_resources& resources)
        {
            auto context = copy_context::begin(name_);
            exec_callback(*std::launder(reinterpret_cast<render_pass_data*>(&data_)), resources, context);
        };
    }
} // namespace graphics

#endif
