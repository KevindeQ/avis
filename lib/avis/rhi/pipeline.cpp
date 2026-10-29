#include "avis/rhi/pipeline.h"

#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/pipeline_factory.h"
#include "avis/rhi/render_hardware_interface.h"

namespace avis::rhi
{
    pipelines::pipelines(render_hardware_interface& hardware_interface) noexcept : state_{ &hardware_interface.state() }
    {}

    result<root_signature_handle> pipelines::create_root_signature(const root_signature_description& description)
    {
        return state_->pipelines().create_root_signature(description);
    }

    result<graphics_pipeline_handle> pipelines::create_pipeline(const graphics_pipeline_description& description)
    {
        return state_->pipelines().create_pipeline(description);
    }

    result<compute_pipeline_handle> pipelines::create_pipeline(const compute_pipeline_description& description)
    {
        return state_->pipelines().create_pipeline(description);
    }
} // namespace avis::rhi
