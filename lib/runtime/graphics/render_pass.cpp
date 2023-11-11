#include "avis/runtime/graphics/render_pass.h"

namespace graphics
{
    void render_pass::reference_resource(resource handle)
    {
        resources_.push_back(handle);
    }

    void render_pass::add_ref()
    {
        reference_count_ += 1;
    }

    void render_pass::release()
    {
        reference_count_ -= 1;
    }
} // namespace graphics
