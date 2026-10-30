#ifndef AVIS_GRAPHICS_ROOT_SIGNATURE_REGISTRY_H
#define AVIS_GRAPHICS_ROOT_SIGNATURE_REGISTRY_H

// Candidate root signatures and the choice of one per pipeline (docs/design/18-graphics-shaders-and-pipelines.md §10).
// The renderer adds its signatures in order of preference; the pipeline cache asks for the first one that provides
// every register its shaders bind, or checks the one a pipeline names. Depends on the RHI only.

#include "avis/error.h"
#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/root_signature.h"
#include "avis/rhi/types.h"

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace avis::graphics
{
    class root_signature_registry
    {
    public:
        // One stage of a pipeline: its stage (for visibility) and what its shader binds. bindings must not be null: a
        // shader without bindings can only be used with a named signature, unchecked (the pipeline cache reports
        // shader_bindings_missing before asking).
        struct stage_bindings
        {
            shader_stage stage;
            const shader_bindings* bindings;
        };

        explicit root_signature_registry(rhi::pipelines pipelines); // the RHI's pipelines view (09 §4.3)
        ~root_signature_registry();
        root_signature_registry(const root_signature_registry&) = delete;
        root_signature_registry& operator=(const root_signature_registry&) = delete;

        // Creates the RHI root signature and appends it to the candidates; the description is copied, and its debug
        // name defaults to `name`. Candidates are tried in the order they were added. Fails with invalid_argument for
        // an empty or already registered name, or with the RHI's error.
        result<rhi::root_signature_handle> add(std::string name, const rhi::root_signature_description& description);

        // The first candidate compatible with every stage (§10.1). no_compatible_root_signature lists each candidate
        // with its first mismatch ("standard: pixel shader b3 space0 (64 bytes) not visible").
        result<rhi::root_signature_handle> find_compatible(std::span<const stage_bindings> stages) const;

        // root_signature_incompatible names the first mismatch; invalid_argument for a handle that isn't registered
        result<void>
            check_compatible(rhi::root_signature_handle signature, std::span<const stage_bindings> stages) const;

        // Calls of find_compatible and check_compatible so far, for tests of the pipeline cache's single matching per
        // key
        std::uint64_t lookup_count() const noexcept;

    private:
        struct candidate; // name, handle, owned copy of the description

        rhi::pipelines pipelines_;
        mutable std::shared_mutex mutex_; // add is rare (startup, a new binding model); lookups are shared
        std::vector<candidate> candidates_;
        mutable std::atomic<std::uint64_t> lookups_{ 0 };
    };
} // namespace avis::graphics

#endif
