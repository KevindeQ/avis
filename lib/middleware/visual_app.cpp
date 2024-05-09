#include "avis/middleware/visual_app.h"

visual_app::visual_app(const basic_app_config& config) :
    basic_app(config),

    graphics_device{ false, true },

    shader_object_manager{},

    root_signature_bindless{},
    pso_cs_draw_point_cloud{}
{
    load_pipeline();
}

void visual_app::on_render_frame()
{
    struct pass_data
    {
        graphics::virtual_resource res1;
        graphics::virtual_resource res2;
    };

    graphics::resource_graph resources;
    graphics::render_pipeline pipeline{ resources };
    // auto resource_data1 = pipeline.add_graphics_pass<pass_data>(
    //     "test 1",
    //     [&](graphics::render_pass_builder& builder, pass_data& data)
    //     {
    //         data.res1 = builder.create_buffer();
    //         data.res2 = builder.create_texture();
    //     },
    //     [=](const pass_data& data, graphics::render_pass_resources& resources, graphics::graphics_context& context)
    //     {
    //         /*context.configure_pipeline();*/

    //        /*context.set_render_target();*/

    //        /* context.set_vertex_buffer(); */
    //        context.draw_instanced(3, 1, 0, 0);
    //    });

    auto resource_data2 = pipeline.add_compute_pass<pass_data>(
        "test 2",
        [&](graphics::render_pass_builder& builder, pass_data& data)
        {
            /*data.res2 = builder.read(resource_data1.res1);
            data.res3 = builder.write(resource_data1.res2);*/
        },
        [=](const pass_data& data, graphics::render_pass_resources& resources, graphics::compute_context& context)
        {
            context.configure_pipeline(pso_cs_draw_point_cloud);



            context.dispatch(1, 1, 1);
        });

    /*auto resource_data3 = pipeline.add_copy_pass<pass_data>(
        "test 3",
        [&](graphics::render_pass_builder& builder, pass_data& data)
        {
            data.res1 = builder.create_texture();
            data.res2 = builder.write(resource_data2.res3);
        },
        [=](const pass_data& data, graphics::render_pass_resources& resources, graphics::copy_context& context) {

        });*/

    pipeline.build();
    pipeline.execute();
}

void visual_app::load_pipeline()
{
    std::future<graphics::shader_ref> shader_draw_point_cloud =
        load_compiled_shader(".\\assets\\cso\\cs_draw_point_cloud.cso");

    graphics::root_signature_builder root_signature_builder{};
    root_signature_builder.with_parameter_constant(0, 0, 64, graphics::shader_visibility::all);
    root_signature_builder.with_flags(
        graphics::root_signature_flags::allow_input_assembler_input_layout |
        graphics::root_signature_flags::cbv_srv_uav_heap_directly_indexed |
        graphics::root_signature_flags::sampler_heap_directly_indexed);
    root_signature_bindless = root_signature_builder.build(graphics_device);

    graphics::compute_pipeline_state_builder cs_state_builder{};
    cs_state_builder.with_root_signature(root_signature_bindless);
    cs_state_builder.with_compute_shader(shader_draw_point_cloud.get());
    pso_cs_draw_point_cloud = cs_state_builder.build(graphics_device);
}

std::future<graphics::shader_ref> visual_app::load_compiled_shader(const std::string_view& filename)
{
    io::file_descriptor file_cso = file_context.create_descriptor(filename);
    std::future<graphics::shader_ref> shader_ref = file_load_service.async_read_file<graphics::shader_ref>(
        file_cso,
        [&](const streams::memory_stream& stream)
        {
            return shader_object_manager.create_new(stream.data());
        });

    return shader_ref;
}
