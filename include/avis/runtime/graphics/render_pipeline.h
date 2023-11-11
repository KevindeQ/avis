#ifndef RUNTIME_GRAPHICS_RENDER_PIPELINE_H
#define RUNTIME_GRAPHICS_RENDER_PIPELINE_H

#include "avis/runtime/graphics/render_pass.h"
#include "avis/runtime/graphics/resource_graph.h"

namespace graphics
{
    class render_pipeline
    {
    public:
        ENGINE_API explicit render_pipeline(resource_graph& resources);

        template<typename render_pass_data>
        render_pass_data add_graphics_pass(
            const std::string& render_pass_name,
            typename render_pass::config_callback_type<render_pass_data> configure_pass,
            typename render_pass::graphics_exec_callback_type<render_pass_data> execute_pass);

        template<typename render_pass_data>
        render_pass_data add_compute_pass(
            const std::string& render_pass_name,
            typename render_pass::config_callback_type<render_pass_data> configure_pass,
            typename render_pass::compute_exec_callback_type<render_pass_data> execute_pass);

        template<typename render_pass_data>
        render_pass_data add_copy_pass(
            const std::string& render_pass_name,
            typename render_pass::config_callback_type<render_pass_data> configure_pass,
            typename render_pass::copy_exec_callback_type<render_pass_data> execute_pass);

        ENGINE_API void build();
        ENGINE_API void execute();

    private:
        template<typename render_pass_data>
        void configure_render_pass(
            typename render_pass::config_callback_type<render_pass_data> configure_pass, render_pass_data& pass_data);

    private:
        resource_graph& resource_graph_;

        std::vector<render_pass> render_passes_;
    };

    template<typename render_pass_data>
    inline render_pass_data render_pipeline::add_graphics_pass(
        const std::string& render_pass_name,
        typename render_pass::config_callback_type<render_pass_data> configure_pass,
        typename render_pass::graphics_exec_callback_type<render_pass_data> execute_pass)
    {
        render_pass_data pass_data;
        configure_render_pass(configure_pass, pass_data);

        // render_passes_.emplace_back(render_pass_name, pass_data, execute_pass);

        // TODO: Eliminate return by copy. Make it return by reference instead. However, this requires that the
        // allocated render_pass can't move in memory, which could happen with the usage of std::vector. It could
        // relocate its storage each time an element is added.
        return pass_data;
    }

    template<typename render_pass_data>
    inline render_pass_data render_pipeline::add_compute_pass(
        const std::string& render_pass_name,
        typename render_pass::config_callback_type<render_pass_data> configure_pass,
        typename render_pass::compute_exec_callback_type<render_pass_data> execute_pass)
    {
        render_pass_data pass_data;
        configure_render_pass(configure_pass, pass_data);

        // render_passes_.emplace_back(render_pass_name, configure_pass, execute_pass);

        // TODO: Eliminate return by copy. Make it return by reference instead. However, this requires that the
        // allocated render_pass can't move in memory, which could happen with the usage of std::vector. It could
        // relocate its storage each time an element is added.
        return pass_data;
    }

    template<typename render_pass_data>
    inline render_pass_data render_pipeline::add_copy_pass(
        const std::string& render_pass_name,
        typename render_pass::config_callback_type<render_pass_data> configure_pass,
        typename render_pass::copy_exec_callback_type<render_pass_data> execute_pass)
    {
        render_pass_data pass_data;
        configure_render_pass(configure_pass, pass_data);

        // render_passes_.emplace_back(render_pass_name, configure_pass, execute_pass);

        // TODO: Eliminate return by copy. Make it return by reference instead. However, this requires that the
        // allocated render_pass can't move in memory, which could happen with the usage of std::vector. It could
        // relocate its storage each time an element is added.
        return pass_data;
    }

    template<typename render_pass_data>
    inline void render_pipeline::configure_render_pass(
        typename render_pass::config_callback_type<render_pass_data> configure_pass, render_pass_data& pass_data)
    {
        if (configure_pass)
        {
            // render_pass_builder builder{ pass, resource_graph_ };
            // configure_pass(pass_data);
        }
    }
} // namespace graphics

#endif
