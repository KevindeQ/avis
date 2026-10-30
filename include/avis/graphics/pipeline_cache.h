#ifndef AVIS_GRAPHICS_PIPELINE_CACHE_H
#define AVIS_GRAPHICS_PIPELINE_CACHE_H

// Pipelines by description, with shader ids instead of bytecode (docs/design/18-graphics-shaders-and-pipelines.md
// §11). On a miss the cache resolves the shaders through a library, checks their stages, picks or checks the root
// signature through a root_signature_registry and creates the PSO through the RHI. Depends on the RHI only.

#include "avis/error.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>

namespace avis::graphics
{
    // The same as the RHI descriptions, with shader ids instead of bytecode
    struct graphics_pipeline_description
    {
        shader_id vertex_shader;
        shader_id pixel_shader; // {} = depth-only
        rhi::graphics_pipeline_state state; // rasterizer, depth, blend, formats, topology (09 §12)
        std::optional<rhi::root_signature_handle> root_signature; // checked when set, else matched (§10)
        std::string_view debug_name; // copied by the RHI at creation; not part of the key
    };

    struct compute_pipeline_description
    {
        shader_id compute_shader;
        std::optional<rhi::root_signature_handle> root_signature;
        std::string_view debug_name;
    };

    class pipeline_cache
    {
    public:
        // Takes only the RHI's pipelines view (09 §4.3): creating PSOs is all it does with the RHI. Pipelines live as
        // long as the RHI (D-87); the cache only forgets the mapping. If the RHI gains pipeline destroy, the cache must
        // drop the entries it destroys. `root_signatures` must outlive the cache.
        pipeline_cache(rhi::pipelines pipelines, const root_signature_registry& root_signatures);
        ~pipeline_cache();
        pipeline_cache(const pipeline_cache&) = delete;
        pipeline_cache& operator=(const pipeline_cache&) = delete;

        // Thread-safe; PSOs are created outside the lock, so concurrent misses never wait for each other. Fails before
        // any RHI call with:
        // - shader_not_found: an id isn't in the library;
        // - shader_stage_mismatch: a shader in the wrong slot;
        // - shader_bindings_missing: a shader without bindings and no named root signature;
        // - root_signature_incompatible or no_compatible_root_signature from the registry.
        // A named root signature is checked against the shaders that have bindings; the others aren't checked. The
        // RHI's errors pass through.
        result<rhi::graphics_pipeline_handle>
            get_or_create(const shader_library& shaders, const graphics_pipeline_description& description);
        result<rhi::compute_pipeline_handle>
            get_or_create(const shader_library& shaders, const compute_pipeline_description& description);

        // Distinct descriptions cached so far, for tests
        std::size_t size() const;

    private:
        // key = xxh3 over the state fields (not debug_name), the library's serial, the shader ids and the explicit
        // root signature if any; libraries are immutable, so (serial, id) identifies the bytecode without hashing it
        rhi::pipelines pipelines_;
        const root_signature_registry& root_signatures_;
        mutable std::shared_mutex mutex_; // shared for lookups, exclusive for inserts (Q1)
        std::unordered_map<std::uint64_t, rhi::graphics_pipeline_handle> graphics_;
        std::unordered_map<std::uint64_t, rhi::compute_pipeline_handle> compute_;
    };
} // namespace avis::graphics

#endif
