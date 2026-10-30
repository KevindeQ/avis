#ifndef AVIS_LIB_GRAPHICS_ROOT_SIGNATURE_MATCHING_H
#define AVIS_LIB_GRAPHICS_ROOT_SIGNATURE_MATCHING_H

// The compatibility check of root_signature_registry (docs/design/18-graphics-shaders-and-pipelines.md §10.1), a pure
// function over a description and the pipeline's bindings, exposed so tests run it without a device

#include "avis/graphics/root_signature_registry.h"
#include "avis/rhi/root_signature.h"

#include <optional>
#include <span>
#include <string>

namespace avis::graphics::detail
{
    // nullopt when every binding of every stage is provided by a parameter or static sampler visible to that stage;
    // otherwise the first binding that isn't, for example "pixel shader b3 space0 (64 bytes) not visible"
    std::optional<std::string> first_mismatch(
        const rhi::root_signature_description& description,
        std::span<const root_signature_registry::stage_bindings> stages);
} // namespace avis::graphics::detail

#endif
