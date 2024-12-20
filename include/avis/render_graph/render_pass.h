#ifndef RENDER_GRAPH_RENDER_PASS_H
#define RENDER_GRAPH_RENDER_PASS_H

#include "avis/common.h"
#include "avis/graphics/command_context.h"
#include "avis/render_graph/render_pass_builder.h"
#include "avis/render_graph/resource_bag.h"

namespace render_graph
{
    class render_pass
    {
    public:
        template<typename render_pass_data>
        using config_callback_type = std::function<void(render_pass_builder&, render_pass_data&)>;

        template<typename render_pass_data>
        using graphics_exec_callback_type =
            std::function<void(const render_pass_data&, resource_bag&, graphics::graphics_context&)>;

        template<typename render_pass_data>
        using compute_exec_callback_type =
            std::function<void(const render_pass_data&, resource_bag&, graphics::compute_context&)>;

        template<typename render_pass_data>
        using copy_exec_callback_type = std::function<void(const render_pass_data&, resource_bag&, graphics::copy_context&)>;

    private:
        using internal_exec_callback_type = std::function<void(resource_bag&)>;

    public:
        template<typename render_pass_data>
        render_pass(
            const std::string& pass_name,
            render_pass_data& pass_data,
            graphics_exec_callback_type<render_pass_data> exec_callback);

        template<typename render_pass_data>
        render_pass(
            const std::string& pass_name,
            render_pass_data& pass_data,
            compute_exec_callback_type<render_pass_data> exec_callback);

        template<typename render_pass_data>
        render_pass(
            const std::string& pass_name,
            render_pass_data& pass_data,
            copy_exec_callback_type<render_pass_data> exec_callback);

        void execute(resource_bag& resources)
        {
            exec_callback(resources);
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

        std::string name;
        std::aligned_storage<64, alignof(std::max_align_t)>::type data;
        internal_exec_callback_type exec_callback;
    };

    template<typename render_pass_data>
    inline render_pass::render_pass(
        const std::string& pass_name,
        render_pass_data& pass_data,
        graphics_exec_callback_type<render_pass_data> exec_callback) :
        name{ name },
        data{},
        exec_callback{}
    {
        store_render_pass_data<render_pass_data>(pass_data);
        exec_callback = convert_to_internal_callback(exec_callback);
    }

    template<typename render_pass_data>
    inline render_pass::render_pass(
        const std::string& pass_name,
        render_pass_data& pass_data,
        compute_exec_callback_type<render_pass_data> exec_callback) :
        name{ pass_name },
        data{},
        exec_callback{}
    {
        store_render_pass_data<render_pass_data>(pass_data);
        exec_callback = convert_to_internal_callback(exec_callback);
    }

    template<typename render_pass_data>
    inline render_pass::render_pass(
        const std::string& pass_name,
        render_pass_data& pass_data,
        copy_exec_callback_type<render_pass_data> exec_callback) :
        name{ pass_name },
        data{},
        exec_callback{}
    {
        store_render_pass_data<render_pass_data>(pass_data);
        exec_callback = convert_to_internal_callback(exec_callback);
    }

    template<typename render_pass_data>
    inline void render_pass::store_render_pass_data(render_pass_data& pass_data)
    {
        static_assert(
            (alignof(render_pass_data) - 1 + sizeof(render_pass_data)) <= sizeof(data),
            "render_pass_data can't be stored in the provided storage space because the type size is too large.");
        static_assert(
            alignof(render_pass_data) <= alignof(std::max_align_t),
            "alignment of render_pass_data exceeds max. supported alignment. The max support aligned equals "
            "the alignent of std::max_align_t.");

        new (&data) render_pass_data{ pass_data };
    }

    template<typename render_pass_data>
    inline render_pass::internal_exec_callback_type
        render_pass::convert_to_internal_callback(graphics_exec_callback_type<render_pass_data> exec_callback)
    {
        return [this, exec_callback](resource_bag& resources)
        {
            auto context = graphics::graphics_context::begin(name);
            exec_callback(*std::launder(reinterpret_cast<render_pass_data*>(&data)), resources, context);
        };
    }

    template<typename render_pass_data>
    inline render_pass::internal_exec_callback_type
        render_pass::convert_to_internal_callback(compute_exec_callback_type<render_pass_data> exec_callback)
    {
        return [this, exec_callback](resource_bag& resources)
        {
            auto context = graphics::compute_context::begin(name);
            exec_callback(*std::launder(reinterpret_cast<render_pass_data*>(&data)), resources, context);
        };
    }

    template<typename render_pass_data>
    inline render_pass::internal_exec_callback_type
        render_pass::convert_to_internal_callback(copy_exec_callback_type<render_pass_data> exec_callback)
    {
        return [this, exec_callback](resource_bag& resources)
        {
            auto context = graphics::copy_context::begin(name);
            exec_callback(*std::launder(reinterpret_cast<render_pass_data*>(&data)), resources, context);
        };
    }
} // namespace graphics

#endif
