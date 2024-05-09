#include "avis/middleware/data_formats/ply/ply_parser.h"
#include "avis/middleware/geometry/data_store.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_dispatcher.h"
#include "avis/middleware/runtime.h"
#include "avis/middleware/visual_app.h"
#include "avis/middleware/window.h"
#include "avis/runtime/data/point_cloud.h"

// struct file_output
//{};
//
// struct file_parser
//{
//     file_output operator()(const streams::memory_stream& data);
// };

class sample_app : public visual_app
{
public:
    sample_app(const basic_app_config& config);

    void on_update() override;

private:
    void load_content();

    void configure_input();

private:
    std::unique_ptr<data::point_cloud> point_cloud;

    window render_window;

    input::input_dispatcher input_dispatcher;
    input::input_context global_input_context;
    input::input_context movement_input_context;
};

sample_app::sample_app(const basic_app_config& config) :
    visual_app(config),

    point_cloud{},

    render_window{ { L"D3D12 Demo", 800, 800 } },

    input_dispatcher{},
    global_input_context{},
    movement_input_context{}
{
    configure_input();

    load_content();
}

void sample_app::on_update()
{
    input_dispatcher.dispatch_input();
}

void sample_app::load_content()
{
    io::basic_file_descriptor database_file{ ".\\assets\\maps\\point_cloud.adf" };
    io::basic_file_descriptor index_file{ ".\\assets\\maps\\point_cloud.aif" };
    point_cloud = std::make_unique<data::point_cloud>(index_file, database_file);
}

void sample_app::configure_input()
{
    render_window.on_message(
        WM_INPUT,
        [this](WPARAM wparam, LPARAM lparam)
        {
            input_dispatcher.handle_raw_input(wparam, lparam);
            return 0;
        });

    global_input_context.add_mapping();
    global_input_context.add_mapping();
    global_input_context.add_mapping();
    input_dispatcher.push_context(global_input_context);

    movement_input_context.add_mapping();
    movement_input_context.add_mapping();
    movement_input_context.add_mapping();
    movement_input_context.add_mapping();
    input_dispatcher.push_context(movement_input_context);
}

int __stdcall wWinMain(
    _In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    runtime_builder engine_builder;
    engine_builder.configure_engine(
        [](configuration_builder& builder)
        {
            // builder.use_root_path("");
            // builder.configure_from_json("E:\\Projects\\D3D12TechDemo\\build\\bin\\Debug\\test.txt");
            // builder.configure_from_ini("");
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

// file_output file_parser::operator()(const streams::memory_stream& data)
//{
//     return file_output{};
// }
