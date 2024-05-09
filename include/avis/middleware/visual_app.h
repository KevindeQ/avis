#ifndef MIDDLEWARE_VISUAL_APP_H
#define MIDDLEWARE_VISUAL_APP_H

#include "avis/core/common.h"
#include "avis/middleware/basic_app.h"
#include "avis/runtime/graphics/graphics_device.h"
#include "avis/runtime/graphics/pipeline_state.h"
#include "avis/runtime/graphics/render_pass.h"
#include "avis/runtime/graphics/render_pipeline.h"
#include "avis/runtime/graphics/root_signature.h"

class visual_app : public basic_app
{
public:
    visual_app(const basic_app_config& config);

    void on_render_frame() override;

private:
    void load_pipeline();

    std::future<graphics::shader_ref> load_compiled_shader(const std::string_view& filename);

    graphics::graphics_device graphics_device;

    graphics::shader_manager shader_object_manager;

    graphics::root_signature root_signature_bindless;
    graphics::pipeline_state pso_cs_draw_point_cloud;
};

#endif
