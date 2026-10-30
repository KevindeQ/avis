#include "avis/graphics/root_signature_registry.h"

#include "root_signature_matching.h"

#include "avis/detail/check.h"
#include "avis/graphics/error.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace avis::graphics
{
    namespace detail
    {
        namespace
        {
            std::string_view stage_name(const shader_stage stage) noexcept
            {
                switch (stage)
                {
                case shader_stage::vertex:
                    return "vertex";
                case shader_stage::pixel:
                    return "pixel";
                case shader_stage::compute:
                    return "compute";
                }
                return "unknown";
            }

            // A compute shader only sees `all`; a graphics stage also sees parameters limited to it
            bool visible(const rhi::shader_visibility visibility, const shader_stage stage) noexcept
            {
                switch (visibility)
                {
                case rhi::shader_visibility::all:
                    return true;
                case rhi::shader_visibility::vertex:
                    return stage == shader_stage::vertex;
                case rhi::shader_visibility::pixel:
                    return stage == shader_stage::pixel;
                }
                return false;
            }

            template<class Slot>
            bool at(const Slot& slot, const std::uint32_t shader_register, const std::uint32_t register_space) noexcept
            {
                return slot.shader_register == shader_register && slot.register_space == register_space;
            }

            std::optional<std::string> mismatch(
                const rhi::root_signature_description& description,
                const shader_stage stage,
                const constant_buffer_binding& binding)
            {
                bool hidden = false;
                std::optional<std::uint32_t> too_small; // bytes the largest visible root constants hold
                for (const rhi::root_parameter& parameter : description.parameters)
                {
                    const bool is_constant_buffer = parameter.kind == rhi::root_parameter_kind::constant_buffer;
                    const bool is_constants = parameter.kind == rhi::root_parameter_kind::constants;
                    if ((!is_constant_buffer && !is_constants) ||
                        !at(parameter, binding.shader_register, binding.register_space))
                    {
                        continue;
                    }
                    if (!visible(parameter.visibility, stage))
                    {
                        hidden = true;
                        continue;
                    }
                    const std::uint32_t capacity = parameter.constant_count * 4;
                    if (is_constant_buffer || capacity >= binding.size)
                    {
                        return std::nullopt;
                    }
                    too_small = std::max(too_small.value_or(0), capacity);
                }

                const std::string where = std::format(
                    "{} shader b{} space{} ({} bytes)",
                    stage_name(stage),
                    binding.shader_register,
                    binding.register_space,
                    binding.size);
                if (too_small)
                {
                    return std::format("{}: root constants hold {}", where, *too_small);
                }
                if (hidden)
                {
                    return std::format("{} not visible", where);
                }
                return std::format("{}: no root parameter", where);
            }

            std::optional<std::string> mismatch(
                const rhi::root_signature_description& description,
                const shader_stage stage,
                const buffer_binding& binding)
            {
                const bool reading = binding.kind == buffer_binding_kind::shader_resource;
                const rhi::root_parameter_kind wanted =
                    reading ? rhi::root_parameter_kind::shader_resource : rhi::root_parameter_kind::unordered_access;
                bool hidden = false;
                for (const rhi::root_parameter& parameter : description.parameters)
                {
                    if (parameter.kind != wanted || !at(parameter, binding.shader_register, binding.register_space))
                    {
                        continue;
                    }
                    if (visible(parameter.visibility, stage))
                    {
                        return std::nullopt;
                    }
                    hidden = true;
                }

                const std::string where = std::format(
                    "{} shader {}{} space{}",
                    stage_name(stage),
                    reading ? 't' : 'u',
                    binding.shader_register,
                    binding.register_space);
                if (hidden)
                {
                    return std::format("{} not visible", where);
                }
                return std::format("{}: no root {}", where, reading ? "shader resource" : "unordered access");
            }

            std::optional<std::string> mismatch(
                const rhi::root_signature_description& description,
                const shader_stage stage,
                const sampler_binding& binding)
            {
                bool hidden = false;
                for (const rhi::static_sampler& sampler : description.static_samplers)
                {
                    if (!at(sampler, binding.shader_register, binding.register_space))
                    {
                        continue;
                    }
                    if (visible(sampler.visibility, stage))
                    {
                        return std::nullopt;
                    }
                    hidden = true;
                }

                const std::string where = std::format(
                    "{} shader s{} space{}", stage_name(stage), binding.shader_register, binding.register_space);
                if (hidden)
                {
                    return std::format("{} not visible", where);
                }
                return std::format("{}: no static sampler", where);
            }
        } // namespace

        std::optional<std::string> first_mismatch(
            const rhi::root_signature_description& description,
            const std::span<const root_signature_registry::stage_bindings> stages)
        {
            for (const root_signature_registry::stage_bindings& stage : stages)
            {
                if (!AVIS_CHECK(
                        "graphics",
                        stage.bindings != nullptr,
                        "root signature matching: a stage without bindings (the caller reports "
                        "shader_bindings_missing)"))
                {
                    return std::format("{} shader: no bindings", stage_name(stage.stage));
                }
                for (const constant_buffer_binding& binding : stage.bindings->constant_buffers)
                {
                    if (auto found = mismatch(description, stage.stage, binding))
                    {
                        return found;
                    }
                }
                for (const buffer_binding& binding : stage.bindings->buffers)
                {
                    if (auto found = mismatch(description, stage.stage, binding))
                    {
                        return found;
                    }
                }
                for (const sampler_binding& binding : stage.bindings->samplers)
                {
                    if (auto found = mismatch(description, stage.stage, binding))
                    {
                        return found;
                    }
                }
            }
            return std::nullopt;
        }
    } // namespace detail

    struct root_signature_registry::candidate
    {
        candidate(
            std::string name,
            const rhi::root_signature_handle handle,
            const rhi::root_signature_description& description) :
            name{ std::move(name) },
            handle{ handle },
            parameters{ description.parameters.begin(), description.parameters.end() },
            samplers{ description.static_samplers.begin(), description.static_samplers.end() }
        {}

        // A view of the owned copy; stays valid while the candidate isn't moved
        rhi::root_signature_description description() const noexcept
        {
            return { .parameters = parameters, .static_samplers = samplers, .debug_name = name };
        }

        std::string name;
        rhi::root_signature_handle handle;
        std::vector<rhi::root_parameter> parameters;
        std::vector<rhi::static_sampler> samplers;
    };

    root_signature_registry::root_signature_registry(const rhi::pipelines pipelines) : pipelines_{ pipelines } {}

    root_signature_registry::~root_signature_registry() = default;

    result<rhi::root_signature_handle>
        root_signature_registry::add(std::string name, const rhi::root_signature_description& description)
    {
        if (name.empty())
        {
            return std::unexpected(
                make_error(error_kind::invalid_argument, "root_signature_registry::add: empty candidate name"));
        }

        // Exclusive for the whole call: adding is rare, and the name check and the append must not race
        const std::unique_lock lock{ mutex_ };
        if (std::ranges::any_of(candidates_, [&](const candidate& existing) { return existing.name == name; }))
        {
            return std::unexpected(make_error(
                error_kind::invalid_argument,
                std::format("root_signature_registry::add: a root signature named '{}' is already registered", name)));
        }

        rhi::root_signature_description named = description;
        if (named.debug_name.empty())
        {
            named.debug_name = name;
        }
        auto handle = pipelines_.create_root_signature(named);
        if (!handle)
        {
            return std::unexpected(std::move(handle.error()));
        }
        candidates_.emplace_back(std::move(name), *handle, description);
        return *handle;
    }

    result<rhi::root_signature_handle>
        root_signature_registry::find_compatible(const std::span<const stage_bindings> stages) const
    {
        lookups_.fetch_add(1, std::memory_order_relaxed);
        const std::shared_lock lock{ mutex_ };
        if (candidates_.empty())
        {
            return std::unexpected(make_error(
                error_kind::no_compatible_root_signature, "no compatible root signature: none is registered"));
        }

        std::string reasons;
        for (const candidate& candidate : candidates_)
        {
            const auto mismatch = detail::first_mismatch(candidate.description(), stages);
            if (!mismatch)
            {
                return candidate.handle;
            }
            reasons += std::format("{}{}: {}", reasons.empty() ? "" : "; ", candidate.name, *mismatch);
        }
        return std::unexpected(make_error(
            error_kind::no_compatible_root_signature, std::format("no compatible root signature: {}", reasons)));
    }

    result<void> root_signature_registry::check_compatible(
        const rhi::root_signature_handle signature, const std::span<const stage_bindings> stages) const
    {
        lookups_.fetch_add(1, std::memory_order_relaxed);
        const std::shared_lock lock{ mutex_ };
        const auto found = std::ranges::find_if(
            candidates_, [&](const candidate& candidate) { return candidate.handle == signature; });
        if (found == candidates_.end())
        {
            return std::unexpected(make_error(
                error_kind::invalid_argument,
                "root_signature_registry::check_compatible: the root signature isn't registered"));
        }
        if (const auto mismatch = detail::first_mismatch(found->description(), stages))
        {
            return std::unexpected(make_error(
                error_kind::root_signature_incompatible,
                std::format("root signature '{}': {}", found->name, *mismatch)));
        }
        return {};
    }

    std::uint64_t root_signature_registry::lookup_count() const noexcept
    {
        return lookups_.load(std::memory_order_relaxed);
    }
} // namespace avis::graphics
