// command_recorder<Queue> and command_list<Queue> (docs/design/09-rhi.md §7, §24.3), with explicit instantiations for
// the three queue types, and the command_lists view

#include "avis/rhi/command_list.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/command_list_pool.h"
#include "avis/rhi/d3d12/conversions/barriers.h"
#include "avis/rhi/d3d12/conversions/resources.h"
#include "avis/rhi/d3d12/descriptor_heaps.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/pipeline_factory.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/d3d12/timestamp_query_set_state.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/transient.h"

#include <array>
#include <format>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace avis::rhi
{
    namespace
    {
        using d3d12::bound_pipeline;
        using d3d12::command_list_record;
        using d3d12::list_state;

        // The checks every recording call makes (§24.3): the recorder's generation matches the record and the list is
        // recording, in every build (a failure is asserted in Debug and logged in Release, and the call is ignored);
        // in debug builds also that no other thread is inside a call on the list
        class recording_call
        {
        public:
            recording_call(command_list_record* const record, const std::uint32_t generation, const char* const call) noexcept
            {
                if (!AVIS_RHI_CHECK(
                        record != nullptr && record->generation.load(std::memory_order_relaxed) == generation &&
                            record->state.load(std::memory_order_acquire) == list_state::recording,
                        std::format("{}: the command list was submitted, discarded or moved from", call)))
                {
                    return;
                }
                record_ = record;
#ifndef NDEBUG
                const std::thread::id previous = record->recording_thread.exchange(std::this_thread::get_id());
                AVIS_RHI_CHECK(
                    previous == std::thread::id{},
                    std::format("{}: another thread is recording into the same command list", call));
#endif
            }

            ~recording_call()
            {
#ifndef NDEBUG
                if (record_ != nullptr)
                {
                    record_->recording_thread.store(std::thread::id{});
                }
#endif
            }

            recording_call(const recording_call&) = delete;
            recording_call& operator=(const recording_call&) = delete;

            explicit operator bool() const noexcept
            {
                return record_ != nullptr;
            }

            command_list_record* operator->() const noexcept
            {
                return record_;
            }

            d3d12::graphics_list_interface& list() const noexcept
            {
                return *record_->list.get();
            }

            const d3d12::resource_manager& resources() const noexcept
            {
                return record_->context->resources;
            }

        private:
            command_list_record* record_ = nullptr;
        };

        // A record lookup that asserts on a stale handle
        template<class Record, class Handle>
        const Record* found(const Record* record, const char* const call, const char* const what) noexcept
        {
            AVIS_RHI_CHECK(record != nullptr, std::format("{}: stale or invalid {} handle", call, what));
            return record;
        }

        UINT subresource_index(const texture_description& description, const subresource value) noexcept
        {
            return value.mip + value.layer * d3d12::mip_level_count(description);
        }

        D3D12_RESOURCE_DESC1 footprint_description(const texture_description& description) noexcept
        {
            D3D12_RESOURCE_DESC1 native = d3d12::to_native(description);
            native.MipLevels = static_cast<UINT16>(d3d12::mip_level_count(description));
            return native;
        }

        std::wstring to_wide(const std::string_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
            return wide;
        }

        // The bound root signature for root calls (§12.1): by the bound pipeline's kind. Debug checks: a pipeline is
        // bound, the parameter exists, its kind matches, the constants fit, and compute slots see it (`all`).
        const d3d12::root_signature_record* root_for(
            const recording_call& call,
            const std::uint32_t parameter,
            const root_parameter_kind kind,
            const std::uint32_t constant_end,
            const char* const name) noexcept
        {
            const bound_pipeline bound = call->pipeline;
            if (!AVIS_RHI_CHECK(bound != bound_pipeline::none, std::format("{}: no pipeline is bound yet", name)))
            {
                return nullptr;
            }
            const d3d12::root_signature_record* const root =
                bound == bound_pipeline::graphics ? call->graphics_root : call->compute_root;
            if (!AVIS_RHI_CHECK(
                    parameter < root->parameters.size(),
                    std::format("{}: parameter {} isn't in the bound root signature", name, parameter)))
            {
                return nullptr;
            }
            const root_parameter& described = root->parameters[parameter];
            if (!AVIS_RHI_CHECK(
                    described.kind == kind, std::format("{}: parameter {} is of another kind", name, parameter)) ||
                !AVIS_RHI_CHECK(
                    kind != root_parameter_kind::constants || constant_end <= described.constant_count,
                    std::format("{}: the constants overrun parameter {}", name, parameter)) ||
                !AVIS_RHI_CHECK(
                    bound == bound_pipeline::graphics || described.visibility == shader_visibility::all,
                    std::format("{}: parameter {} isn't visible to compute shaders", name, parameter)))
            {
                return nullptr;
            }
            return root;
        }

        template<class Native, std::size_t Inline = 32>
        class barrier_array
        {
        public:
            explicit barrier_array(const std::size_t count)
            {
                if (count > Inline)
                {
                    overflow_.resize(count);
                }
            }

            Native* data() noexcept
            {
                return overflow_.empty() ? inline_.data() : overflow_.data();
            }

        private:
            std::array<Native, Inline> inline_{};
            std::vector<Native> overflow_;
        };

        // The texture's RTV or DSV through its view cache (§22.2.3)
        result<D3D12_CPU_DESCRIPTOR_HANDLE>
            target_view(const recording_call& call, const texture_view& view, const bool depth, const char* const name)
        {
            const d3d12::texture_record* const texture = found<d3d12::texture_record, texture_handle>(
                call.resources().find(view.texture), name, "texture");
            if (texture == nullptr)
            {
                return std::unexpected(make_error(error_kind::invalid_handle, "stale texture"));
            }
            if (!AVIS_RHI_CHECK(
                    texture->views != nullptr,
                    std::format("{}: the texture has neither render_target nor depth_stencil usage", name)))
            {
                return std::unexpected(make_error(error_kind::invalid_argument, "no view cache"));
            }
            return depth ? texture->views->depth_stencil(*texture->resource.get(), texture->description, view) :
                           texture->views->render_target(*texture->resource.get(), texture->description, view);
        }

        template<class Values>
        void clear_uav(
            const recording_call& call,
            const d3d12::texture_record& texture,
            const bindless_index index,
            const Values& values) noexcept
        {
            d3d12::descriptor_heaps& heaps = call->context->descriptors;
            if constexpr (std::is_same_v<typename Values::element_type, const float>)
            {
                call.list().ClearUnorderedAccessViewFloat(
                    heaps.shader_visible_gpu_handle(index),
                    heaps.staging_cpu_handle(index),
                    texture.resource.get(),
                    values.data(),
                    0,
                    nullptr);
            }
            else
            {
                call.list().ClearUnorderedAccessViewUint(
                    heaps.shader_visible_gpu_handle(index),
                    heaps.staging_cpu_handle(index),
                    texture.resource.get(),
                    values.data(),
                    0,
                    nullptr);
            }
        }
    } // namespace

    // ---- every queue type ----

    template<queue_type Queue>
    void command_recorder<Queue>::copy_buffer(
        const buffer_handle destination,
        const std::uint64_t destination_offset,
        const buffer_handle source,
        const std::uint64_t source_offset,
        const std::uint64_t size)
    {
        const recording_call call{ record_, generation_, "copy_buffer" };
        if (!call)
        {
            return;
        }
        const auto* const target = found<d3d12::buffer_record, buffer_handle>(call.resources().find(destination), "copy_buffer", "buffer");
        const auto* const origin = found<d3d12::buffer_record, buffer_handle>(call.resources().find(source), "copy_buffer", "buffer");
        if (target == nullptr || origin == nullptr)
        {
            return;
        }
        call.list().CopyBufferRegion(target->resource.get(), destination_offset, origin->resource.get(), source_offset, size);
    }

    template<queue_type Queue>
    void command_recorder<Queue>::copy_texture(
        const texture_handle destination,
        const subresource destination_subresource,
        const texture_handle source,
        const subresource source_subresource)
    {
        const recording_call call{ record_, generation_, "copy_texture" };
        if (!call)
        {
            return;
        }
        const auto* const target = found<d3d12::texture_record, texture_handle>(call.resources().find(destination), "copy_texture", "texture");
        const auto* const origin = found<d3d12::texture_record, texture_handle>(call.resources().find(source), "copy_texture", "texture");
        if (target == nullptr || origin == nullptr)
        {
            return;
        }
        D3D12_TEXTURE_COPY_LOCATION to{ .pResource = target->resource.get(),
                                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        to.SubresourceIndex = subresource_index(target->description, destination_subresource);
        D3D12_TEXTURE_COPY_LOCATION from{ .pResource = origin->resource.get(),
                                          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        from.SubresourceIndex = subresource_index(origin->description, source_subresource);
        call.list().CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }

    template<queue_type Queue>
    void command_recorder<Queue>::copy_buffer_to_texture(
        const texture_handle destination,
        const subresource destination_subresource,
        const buffer_handle source,
        const std::uint64_t source_offset)
    {
        const recording_call call{ record_, generation_, "copy_buffer_to_texture" };
        if (!call ||
            !AVIS_RHI_CHECK(
                source_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0,
                "copy_buffer_to_texture: the buffer offset must be 512-byte aligned"))
        {
            return;
        }
        const auto* const target = found<d3d12::texture_record, texture_handle>(call.resources().find(destination), "copy_buffer_to_texture", "texture");
        const auto* const origin = found<d3d12::buffer_record, buffer_handle>(call.resources().find(source), "copy_buffer_to_texture", "buffer");
        if (target == nullptr || origin == nullptr)
        {
            return;
        }
        const UINT index = subresource_index(target->description, destination_subresource);
        const D3D12_RESOURCE_DESC1 native = footprint_description(target->description);
        D3D12_TEXTURE_COPY_LOCATION from{ .pResource = origin->resource.get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        call->context->device.native().GetCopyableFootprints1(
            &native, index, 1, source_offset, &from.PlacedFootprint, nullptr, nullptr, nullptr);
        D3D12_TEXTURE_COPY_LOCATION to{ .pResource = target->resource.get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        to.SubresourceIndex = index;
        call.list().CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }

    template<queue_type Queue>
    void command_recorder<Queue>::copy_texture_to_buffer(
        const buffer_handle destination,
        const std::uint64_t destination_offset,
        const texture_handle source,
        const subresource source_subresource)
    {
        const recording_call call{ record_, generation_, "copy_texture_to_buffer" };
        if (!call ||
            !AVIS_RHI_CHECK(
                destination_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0,
                "copy_texture_to_buffer: the buffer offset must be 512-byte aligned"))
        {
            return;
        }
        const auto* const target = found<d3d12::buffer_record, buffer_handle>(call.resources().find(destination), "copy_texture_to_buffer", "buffer");
        const auto* const origin = found<d3d12::texture_record, texture_handle>(call.resources().find(source), "copy_texture_to_buffer", "texture");
        if (target == nullptr || origin == nullptr)
        {
            return;
        }
        const UINT index = subresource_index(origin->description, source_subresource);
        const D3D12_RESOURCE_DESC1 native = footprint_description(origin->description);
        D3D12_TEXTURE_COPY_LOCATION to{ .pResource = target->resource.get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        call->context->device.native().GetCopyableFootprints1(
            &native, index, 1, destination_offset, &to.PlacedFootprint, nullptr, nullptr, nullptr);
        D3D12_TEXTURE_COPY_LOCATION from{ .pResource = origin->resource.get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        from.SubresourceIndex = index;
        call.list().CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }

    template<queue_type Queue>
    void command_recorder<Queue>::barrier(
        const std::span<const global_barrier> global_barriers,
        const std::span<const buffer_barrier> buffer_barriers,
        const std::span<const texture_barrier> texture_barriers)
    {
        const recording_call call{ record_, generation_, "barrier" };
        if (!call)
        {
            return;
        }

        barrier_array<D3D12_GLOBAL_BARRIER> globals{ global_barriers.size() };
        UINT global_count = 0;
        for (const global_barrier& entry : global_barriers)
        {
            globals.data()[global_count++] = { .SyncBefore = d3d12::to_native(entry.before.synchronization),
                                               .SyncAfter = d3d12::to_native(entry.after.synchronization),
                                               .AccessBefore = d3d12::to_native(entry.before.access),
                                               .AccessAfter = d3d12::to_native(entry.after.access) };
        }

        barrier_array<D3D12_BUFFER_BARRIER> buffers{ buffer_barriers.size() };
        UINT buffer_count = 0;
        for (const buffer_barrier& entry : buffer_barriers)
        {
            const auto* const buffer = found<d3d12::buffer_record, buffer_handle>(call.resources().find(entry.buffer), "barrier", "buffer");
            if (buffer == nullptr)
            {
                continue;
            }
            buffers.data()[buffer_count++] = { .SyncBefore = d3d12::to_native(entry.before.synchronization),
                                               .SyncAfter = d3d12::to_native(entry.after.synchronization),
                                               .AccessBefore = d3d12::to_native(entry.before.access),
                                               .AccessAfter = d3d12::to_native(entry.after.access),
                                               .pResource = buffer->resource.get(),
                                               .Offset = 0,
                                               .Size = UINT64_MAX };
        }

        barrier_array<D3D12_TEXTURE_BARRIER> textures{ texture_barriers.size() };
        UINT texture_count = 0;
        for (const texture_barrier& entry : texture_barriers)
        {
            const auto* const texture = found<d3d12::texture_record, texture_handle>(call.resources().find(entry.texture), "barrier", "texture");
            if (texture == nullptr)
            {
                continue;
            }
            textures.data()[texture_count++] = {
                .SyncBefore = d3d12::to_native(entry.before.synchronization),
                .SyncAfter = d3d12::to_native(entry.after.synchronization),
                .AccessBefore = d3d12::to_native(entry.before.access),
                .AccessAfter = d3d12::to_native(entry.after.access),
                .LayoutBefore = d3d12::to_native(entry.before.layout),
                .LayoutAfter = d3d12::to_native(entry.after.layout),
                .pResource = texture->resource.get(),
                .Subresources = d3d12::to_native(entry.range, texture->description),
                .Flags = entry.discard ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE,
            };
        }

        std::array<D3D12_BARRIER_GROUP, 3> groups{};
        UINT group_count = 0;
        if (global_count > 0)
        {
            groups[group_count].Type = D3D12_BARRIER_TYPE_GLOBAL;
            groups[group_count].NumBarriers = global_count;
            groups[group_count++].pGlobalBarriers = globals.data();
        }
        if (buffer_count > 0)
        {
            groups[group_count].Type = D3D12_BARRIER_TYPE_BUFFER;
            groups[group_count].NumBarriers = buffer_count;
            groups[group_count++].pBufferBarriers = buffers.data();
        }
        if (texture_count > 0)
        {
            groups[group_count].Type = D3D12_BARRIER_TYPE_TEXTURE;
            groups[group_count].NumBarriers = texture_count;
            groups[group_count++].pTextureBarriers = textures.data();
        }
        if (group_count > 0)
        {
            call.list().Barrier(group_count, groups.data());
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::begin_event(const std::string_view name)
    {
        const recording_call call{ record_, generation_, "begin_event" };
        if (!call)
        {
            return;
        }
        // PIX's unicode event format: metadata 0, a null-terminated UTF-16 string
        const std::wstring wide = to_wide(name);
        call.list().BeginEvent(0, wide.c_str(), static_cast<UINT>((wide.size() + 1) * sizeof(wchar_t)));
    }

    template<queue_type Queue>
    void command_recorder<Queue>::end_event()
    {
        const recording_call call{ record_, generation_, "end_event" };
        if (call)
        {
            call.list().EndEvent();
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::write_timestamp(timestamp_query_set& set, const std::uint32_t index)
    {
        const recording_call call{ record_, generation_, "write_timestamp" };
        d3d12::timestamp_query_set_state* const state = set.state_;
        if (!call ||
            !AVIS_RHI_CHECK(
                state != nullptr && state->queue == call->queue,
                "write_timestamp: the query set is for another queue than the list's submission queue") ||
            !AVIS_RHI_CHECK(index < state->count, "write_timestamp: index out of range"))
        {
            return;
        }
        call.list().EndQuery(state->heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, index);

        // Extend this set's last run when the index follows it, else start a run (§24.4)
        for (auto run = call->timestamps.rbegin(); run != call->timestamps.rend(); ++run)
        {
            if (run->set == state)
            {
                if (run->end == index)
                {
                    ++run->end;
                    return;
                }
                break;
            }
        }
        call->timestamps.push_back({ .set = state, .first = index, .end = index + 1 });
    }

    // ---- compute and graphics ----

    template<queue_type Queue>
    void command_recorder<Queue>::set_pipeline(const compute_pipeline_handle pipeline)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "set_pipeline" };
        if (!call)
        {
            return;
        }
        const d3d12::compute_pipeline_record* const record = found<d3d12::compute_pipeline_record, compute_pipeline_handle>(
            call->context->pipelines.find(pipeline), "set_pipeline", "compute pipeline");
        if (record == nullptr)
        {
            return;
        }
        call.list().SetPipelineState(record->state.get());
        if (call->compute_root != record->root_signature)
        {
            call.list().SetComputeRootSignature(record->root_signature->native.get());
            call->compute_root = record->root_signature;
        }
        call->pipeline = bound_pipeline::compute;
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_root_constants(
        const std::uint32_t parameter, const std::span<const std::uint32_t> values, const std::uint32_t offset)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "set_root_constants" };
        if (!call ||
            root_for(call,
                     parameter,
                     root_parameter_kind::constants,
                     offset + static_cast<std::uint32_t>(values.size()),
                     "set_root_constants") == nullptr)
        {
            return;
        }
        if (call->pipeline == bound_pipeline::graphics)
        {
            call.list().SetGraphicsRoot32BitConstants(parameter, static_cast<UINT>(values.size()), values.data(), offset);
        }
        else
        {
            call.list().SetComputeRoot32BitConstants(parameter, static_cast<UINT>(values.size()), values.data(), offset);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_root_constant_buffer(const std::uint32_t parameter, const gpu_address address)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "set_root_constant_buffer" };
        if (!call || root_for(call, parameter, root_parameter_kind::constant_buffer, 0, "set_root_constant_buffer") == nullptr)
        {
            return;
        }
        if (call->pipeline == bound_pipeline::graphics)
        {
            call.list().SetGraphicsRootConstantBufferView(parameter, address.value);
        }
        else
        {
            call.list().SetComputeRootConstantBufferView(parameter, address.value);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_root_shader_resource(const std::uint32_t parameter, const gpu_address address)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "set_root_shader_resource" };
        if (!call || root_for(call, parameter, root_parameter_kind::shader_resource, 0, "set_root_shader_resource") == nullptr)
        {
            return;
        }
        if (call->pipeline == bound_pipeline::graphics)
        {
            call.list().SetGraphicsRootShaderResourceView(parameter, address.value);
        }
        else
        {
            call.list().SetComputeRootShaderResourceView(parameter, address.value);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_root_unordered_access(const std::uint32_t parameter, const gpu_address address)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "set_root_unordered_access" };
        if (!call ||
            root_for(call, parameter, root_parameter_kind::unordered_access, 0, "set_root_unordered_access") == nullptr)
        {
            return;
        }
        if (call->pipeline == bound_pipeline::graphics)
        {
            call.list().SetGraphicsRootUnorderedAccessView(parameter, address.value);
        }
        else
        {
            call.list().SetComputeRootUnorderedAccessView(parameter, address.value);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::dispatch(
        const std::uint32_t group_count_x, const std::uint32_t group_count_y, const std::uint32_t group_count_z)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "dispatch" };
        if (call && AVIS_RHI_CHECK(call->pipeline == bound_pipeline::compute, "dispatch: no compute pipeline is bound"))
        {
            call.list().Dispatch(group_count_x, group_count_y, group_count_z);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::dispatch_indirect(const buffer_handle arguments, const std::uint64_t offset)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "dispatch_indirect" };
        if (!call || !AVIS_RHI_CHECK(call->pipeline == bound_pipeline::compute, "dispatch_indirect: no compute pipeline is bound"))
        {
            return;
        }
        const auto* const buffer = found<d3d12::buffer_record, buffer_handle>(call.resources().find(arguments), "dispatch_indirect", "buffer");
        if (buffer != nullptr)
        {
            call.list().ExecuteIndirect(
                &call->context->pipelines.command_signatures().dispatch(), 1, buffer->resource.get(), offset, nullptr, 0);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::clear_unordered_access_float(
        const texture_handle texture, const std::uint32_t mip, const std::span<const float, 4> values)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "clear_unordered_access_float" };
        if (!call)
        {
            return;
        }
        const auto* const record = found<d3d12::texture_record, texture_handle>(call.resources().find(texture), "clear_unordered_access_float", "texture");
        if (record != nullptr &&
            AVIS_RHI_CHECK(mip < record->uav.size(), "clear_unordered_access_float: the texture has no persistent UAV for this mip"))
        {
            clear_uav(call, *record, record->uav[mip].index(), values);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::clear_unordered_access_uint(
        const texture_handle texture, const std::uint32_t mip, const std::span<const std::uint32_t, 4> values)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "clear_unordered_access_uint" };
        if (!call)
        {
            return;
        }
        const auto* const record = found<d3d12::texture_record, texture_handle>(call.resources().find(texture), "clear_unordered_access_uint", "texture");
        if (record != nullptr &&
            AVIS_RHI_CHECK(mip < record->uav.size(), "clear_unordered_access_uint: the texture has no persistent UAV for this mip"))
        {
            clear_uav(call, *record, record->uav[mip].index(), values);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::clear_unordered_access_float(
        const texture_handle texture, const bindless_index view, const std::span<const float, 4> values)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "clear_unordered_access_float" };
        if (!call)
        {
            return;
        }
        const auto* const record = found<d3d12::texture_record, texture_handle>(call.resources().find(texture), "clear_unordered_access_float", "texture");
        if (record != nullptr && AVIS_RHI_CHECK(view.value != 0, "clear_unordered_access_float: the view index is the null descriptor"))
        {
            clear_uav(call, *record, view, values);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::clear_unordered_access_uint(
        const texture_handle texture, const bindless_index view, const std::span<const std::uint32_t, 4> values)
        requires(Queue != queue_type::copy)
    {
        const recording_call call{ record_, generation_, "clear_unordered_access_uint" };
        if (!call)
        {
            return;
        }
        const auto* const record = found<d3d12::texture_record, texture_handle>(call.resources().find(texture), "clear_unordered_access_uint", "texture");
        if (record != nullptr && AVIS_RHI_CHECK(view.value != 0, "clear_unordered_access_uint: the view index is the null descriptor"))
        {
            clear_uav(call, *record, view, values);
        }
    }

    // ---- graphics only ----

    template<queue_type Queue>
    void command_recorder<Queue>::set_pipeline(const graphics_pipeline_handle pipeline)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "set_pipeline" };
        if (!call)
        {
            return;
        }
        const d3d12::graphics_pipeline_record* const record = found<d3d12::graphics_pipeline_record, graphics_pipeline_handle>(
            call->context->pipelines.find(pipeline), "set_pipeline", "graphics pipeline");
        if (record == nullptr)
        {
            return;
        }
        call.list().SetPipelineState(record->state.get());
        call.list().IASetPrimitiveTopology(record->topology);
        if (call->graphics_root != record->root_signature)
        {
            call.list().SetGraphicsRootSignature(record->root_signature->native.get());
            call->graphics_root = record->root_signature;
        }
        call->pipeline = bound_pipeline::graphics;
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_render_targets(
        const std::span<const texture_view> colors, const std::optional<texture_view> depth)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "set_render_targets" };
        if (!call || !AVIS_RHI_CHECK(colors.size() <= 8, "set_render_targets: more than 8 render targets"))
        {
            return;
        }
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> targets{};
        for (std::size_t index = 0; index < colors.size(); ++index)
        {
            auto view = target_view(call, colors[index], false, "set_render_targets");
            if (!AVIS_RHI_CHECK(view.has_value(), "set_render_targets: no render target view"))
            {
                return;
            }
            targets[index] = *view;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE depth_view{};
        if (depth)
        {
            auto view = target_view(call, *depth, true, "set_render_targets");
            if (!AVIS_RHI_CHECK(view.has_value(), "set_render_targets: no depth view"))
            {
                return;
            }
            depth_view = *view;
        }
        call.list().OMSetRenderTargets(
            static_cast<UINT>(colors.size()), targets.data(), FALSE, depth ? &depth_view : nullptr);
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_viewport(const viewport& value)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "set_viewport" };
        if (call)
        {
            const D3D12_VIEWPORT native{ value.x, value.y, value.width, value.height, value.minimum_depth, value.maximum_depth };
            call.list().RSSetViewports(1, &native);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_scissor(const rectangle& value)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "set_scissor" };
        if (call)
        {
            const D3D12_RECT native{ value.left, value.top, value.right, value.bottom };
            call.list().RSSetScissorRects(1, &native);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::set_index_buffer(const buffer_handle buffer, const index_type type, const std::uint64_t offset)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "set_index_buffer" };
        if (!call)
        {
            return;
        }
        const auto* const record = found<d3d12::buffer_record, buffer_handle>(call.resources().find(buffer), "set_index_buffer", "buffer");
        if (record == nullptr || !AVIS_RHI_CHECK(offset <= record->description.size, "set_index_buffer: offset past the end"))
        {
            return;
        }
        const D3D12_INDEX_BUFFER_VIEW view{ .BufferLocation = record->address.value + offset,
                                            .SizeInBytes = static_cast<UINT>(record->description.size - offset),
                                            .Format = type == index_type::uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
        call.list().IASetIndexBuffer(&view);
    }

    template<queue_type Queue>
    void command_recorder<Queue>::draw(
        const std::uint32_t vertex_count,
        const std::uint32_t instance_count,
        const std::uint32_t first_vertex,
        const std::uint32_t first_instance)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "draw" };
        if (call && AVIS_RHI_CHECK(call->pipeline == bound_pipeline::graphics, "draw: no graphics pipeline is bound"))
        {
            call.list().DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::draw_indexed(
        const std::uint32_t index_count,
        const std::uint32_t instance_count,
        const std::uint32_t first_index,
        const std::int32_t vertex_offset,
        const std::uint32_t first_instance)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "draw_indexed" };
        if (call && AVIS_RHI_CHECK(call->pipeline == bound_pipeline::graphics, "draw_indexed: no graphics pipeline is bound"))
        {
            call.list().DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
        }
    }

    namespace
    {
        void execute_indirect(
            const recording_call& call,
            ID3D12CommandSignature& signature,
            const buffer_handle arguments,
            const std::uint64_t offset,
            const std::uint32_t maximum_count,
            const std::optional<buffer_handle> count,
            const std::uint64_t count_offset,
            const char* const name)
        {
            if (!AVIS_RHI_CHECK(call->pipeline == bound_pipeline::graphics, std::format("{}: no graphics pipeline is bound", name)))
            {
                return;
            }
            const auto* const argument_buffer = found<d3d12::buffer_record, buffer_handle>(call.resources().find(arguments), name, "buffer");
            const d3d12::buffer_record* count_buffer = nullptr;
            if (count)
            {
                count_buffer = found<d3d12::buffer_record, buffer_handle>(call.resources().find(*count), name, "buffer");
                if (count_buffer == nullptr)
                {
                    return;
                }
            }
            if (argument_buffer != nullptr)
            {
                call.list().ExecuteIndirect(
                    &signature,
                    maximum_count,
                    argument_buffer->resource.get(),
                    offset,
                    count_buffer != nullptr ? count_buffer->resource.get() : nullptr,
                    count_offset);
            }
        }
    } // namespace

    template<queue_type Queue>
    void command_recorder<Queue>::draw_indirect(
        const buffer_handle arguments,
        const std::uint64_t offset,
        const std::uint32_t maximum_count,
        const std::optional<buffer_handle> count,
        const std::uint64_t count_offset)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "draw_indirect" };
        if (call)
        {
            execute_indirect(
                call, call->context->pipelines.command_signatures().draw(), arguments, offset, maximum_count, count, count_offset, "draw_indirect");
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::draw_indexed_indirect(
        const buffer_handle arguments,
        const std::uint64_t offset,
        const std::uint32_t maximum_count,
        const std::optional<buffer_handle> count,
        const std::uint64_t count_offset)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "draw_indexed_indirect" };
        if (call)
        {
            execute_indirect(
                call,
                call->context->pipelines.command_signatures().draw_indexed(),
                arguments,
                offset,
                maximum_count,
                count,
                count_offset,
                "draw_indexed_indirect");
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::clear_render_target(const texture_view& view, const std::span<const float, 4> color)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "clear_render_target" };
        if (!call)
        {
            return;
        }
        if (auto target = target_view(call, view, false, "clear_render_target");
            AVIS_RHI_CHECK(target.has_value(), "clear_render_target: no render target view"))
        {
            call.list().ClearRenderTargetView(*target, color.data(), 0, nullptr);
        }
    }

    template<queue_type Queue>
    void command_recorder<Queue>::clear_depth(const texture_view& view, const float depth)
        requires(Queue == queue_type::graphics)
    {
        const recording_call call{ record_, generation_, "clear_depth" };
        if (!call)
        {
            return;
        }
        if (auto target = target_view(call, view, true, "clear_depth");
            AVIS_RHI_CHECK(target.has_value(), "clear_depth: no depth view"))
        {
            call.list().ClearDepthStencilView(*target, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
        }
    }

    template<queue_type Queue>
    queue_type command_recorder<Queue>::submission_queue() const noexcept
    {
        return record_ != nullptr ? record_->queue : Queue;
    }

    // ---- command_list ----

    template<queue_type Queue>
    command_list<Queue>::command_list(command_list_record* const record, const std::uint32_t generation) noexcept :
        command_recorder<Queue>{ record, generation }
    {}

    template<queue_type Queue>
    command_list<Queue>::command_list(command_list&& other) noexcept :
        command_recorder<Queue>{ std::exchange(other.record_, nullptr), other.generation_ }
    {}

    template<queue_type Queue>
    command_list<Queue>& command_list<Queue>::operator=(command_list&& other) noexcept
    {
        if (this != &other)
        {
            discard();
            this->record_ = std::exchange(other.record_, nullptr);
            this->generation_ = other.generation_;
        }
        return *this;
    }

    template<queue_type Queue>
    command_list<Queue>::~command_list()
    {
        discard();
    }

    template<queue_type Queue>
    void command_list<Queue>::discard() noexcept
    {
        command_list_record* const record = std::exchange(this->record_, nullptr);
        if (record != nullptr && record->generation.load(std::memory_order_relaxed) == this->generation_ &&
            record->state.load(std::memory_order_acquire) == list_state::recording)
        {
            record->pool->discard(*record);
        }
    }

    // ---- command_lists ----

    command_lists::command_lists(render_hardware_interface& hardware_interface) noexcept :
        state_{ &hardware_interface.state() }
    {}

    template<queue_type Queue>
    result<command_list<Queue>> command_lists::acquire(const std::string_view debug_name)
    {
        auto record = state_->command_lists().acquire(Queue, debug_name);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        return command_list<Queue>{ *record, (*record)->generation.load(std::memory_order_relaxed) };
    }

    template class command_recorder<queue_type::graphics>;
    template class command_recorder<queue_type::compute>;
    template class command_recorder<queue_type::copy>;
    template class command_list<queue_type::graphics>;
    template class command_list<queue_type::compute>;
    template class command_list<queue_type::copy>;
    template result<command_list<queue_type::graphics>> command_lists::acquire(std::string_view);
    template result<command_list<queue_type::compute>> command_lists::acquire(std::string_view);
    template result<command_list<queue_type::copy>> command_lists::acquire(std::string_view);
} // namespace avis::rhi
