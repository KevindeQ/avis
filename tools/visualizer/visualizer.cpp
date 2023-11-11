#include "avis/middleware/basic_app.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_dispatcher.h"
#include "avis/middleware/runtime.h"
#include "avis/middleware/window.h"
#include "avis/runtime/graphics/render_pass.h"
#include "avis/runtime/graphics/render_pipeline.h"
#include "avis/runtime/io/io_context.h"
#include "avis/runtime/parallel/thread_pool.h"

struct file_output
{};

struct file_parser
{
    file_output operator()(const streams::memory_stream& data);
};

class sample_app : public basic_app
{
public:
    sample_app(basic_app_config& config);

    void on_update() override;

    void on_render() override;

private:
    void load_content();

    void configure_input();

private:
    window render_window_;

    parallel::thread_pool threads_;
    io::io_context file_context_;
    io::io_service file_load_service_;

    input::input_dispatcher input_dispatcher_;
    input::input_context global_input_context_;
    input::input_context movement_input_context_;
};

sample_app::sample_app(basic_app_config& config) :
    basic_app(config),

    render_window_{{L"D3D12 Demo", 800, 800}},

    threads_{3},
    file_load_service_{threads_, 3}
{
    render_window_.on_message(
        WM_INPUT,
        [this](WPARAM wparam, LPARAM lparam)
        {
            input_dispatcher_.handle_raw_input(wparam, lparam);
            return 0;
        });

    load_content();
    configure_input();
}

void sample_app::on_update()
{
    input_dispatcher_.dispatch_input();
}

void sample_app::on_render()
{
    graphics::resource_graph resources;
    graphics::render_pipeline pipeline{resources};
    /*auto resource_data1 = pipeline.add_graphics_pass<pass_data>(
        "test 1",
        [&](graphics::render_pass_builder& builder, pass_data& data)
        {
            data.res1 = builder.create_buffer();
            data.res2 = builder.create_texture();
        },
        [=](const pass_data& data, graphics::render_pass_resources& resources, graphics::graphics_context& context) {

        });

    auto resource_data2 = pipeline.add_compute_pass<pass_data>(
        "test 2",
        [&](graphics::render_pass_builder& builder, pass_data& data)
        {
            data.res2 = builder.read(resource_data1.res1);
            data.res3 = builder.write(resource_data1.res2);
        },
        [=](const pass_data& data, graphics::render_pass_resources& resources, graphics::compute_context& context) {

        });

    auto resource_data3 = pipeline.add_copy_pass<pass_data>(
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

void sample_app::load_content()
{
    io::file_descriptor test_file =
        file_context_.create_descriptor("E:\\Projects\\D3D12TechDemo\\data\\models\\buddha\\buddha.obj");
    /*std::future<file_output> test_file_contents = file_load_service_.async_read_file<file_output>(
        test_file, [](const streams::memory_stream & data) { return file_output{}; });*/
    std::future<file_output> test_file_contents =
        file_load_service_.async_read_file<file_output>(test_file, file_parser{});
}

void sample_app::configure_input()
{
    global_input_context_.add_mapping();
    global_input_context_.add_mapping();
    global_input_context_.add_mapping();
    input_dispatcher_.push_context(global_input_context_);

    movement_input_context_.add_mapping();
    movement_input_context_.add_mapping();
    movement_input_context_.add_mapping();
    movement_input_context_.add_mapping();
    input_dispatcher_.push_context(movement_input_context_);
}

int __stdcall wWinMain(
    _In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    runtime_builder engine_builder;
    engine_builder.configure_engine(
        [](configuration_builder& builder)
        {
            builder.use_root_path("");
            builder.configure_from_json("E:\\Projects\\D3D12TechDemo\\build\\bin\\Debug\\test.txt");
            builder.configure_from_ini("");
            // builder.configure_from_commandline();
        });
    engine_builder.configure_logging(
        [](logging_builder& builder)
        {
            // builder.add_log();
            // builder.add_log();
        });
    runtime engine = engine_builder.build();

    basic_app_config configuration;
    configuration.fixed_time_delta = 16ms;

    return engine.execute<sample_app>(configuration);
}

file_output file_parser::operator()(const streams::memory_stream& data)
{
    return file_output{};
}
