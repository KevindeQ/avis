#include "rendering/frame_data.h"

#include "avis/graphics/frame_token.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";

        // Each array starts on this boundary in the staging buffer
        constexpr std::size_t staging_alignment = 256;
        constexpr std::size_t minimum_staging_bytes = std::size_t{ 64 } << 10;

        // Room for small scenes without a reallocation; every slot's buffer exists, so the passes can always bind one
        constexpr std::uint32_t minimum_instances = 256;
        constexpr std::uint32_t minimum_instance_indices = 1024;
        constexpr std::uint32_t minimum_lights = 64;

        constexpr std::size_t align_up(const std::size_t value, const std::size_t alignment) noexcept
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        // One array's copy from the staging buffer into its slot
        struct copy_region
        {
            std::uint32_t target = 0; // index in upload_pass_data::targets
            std::uint64_t source_offset = 0;
            std::uint64_t size = 0;
        };

        struct upload_pass_data
        {
            std::array<graphics::buffer, 3> targets{};
        };
    } // namespace

    frame_buffer_ring::frame_buffer_ring(
        const rhi::resources resources,
        std::string name,
        const std::uint32_t stride,
        const std::uint32_t slot_count,
        const std::uint32_t minimum_elements) :
        resources_{ resources },
        name_{ std::move(name) },
        stride_{ stride },
        minimum_elements_{ std::max<std::size_t>(minimum_elements, 1) },
        slots_(std::max<std::uint32_t>(slot_count, 1))
    {}

    frame_buffer_ring::~frame_buffer_ring()
    {
        for (const slot& item : slots_)
        {
            if (item.buffer.is_valid())
            {
                resources_.destroy(item.buffer);
            }
        }
    }

    result<void> frame_buffer_ring::reallocate(slot& target, const std::size_t elements)
    {
        std::size_t capacity = std::max(target.capacity, minimum_elements_);
        while (capacity < elements)
        {
            capacity *= 2;
        }

        const std::string debug_name = std::format("{} slot {}", name_, &target - slots_.data());
        auto buffer = resources_.create_buffer(
            { .size = std::uint64_t{ capacity } * stride_,
              .structure_stride = stride_,
              .usage = rhi::buffer_usage::shader_resource | rhi::buffer_usage::copy_destination,
              .memory = rhi::memory_type::gpu_only,
              .debug_name = debug_name });
        if (!buffer)
        {
            return std::unexpected{ std::move(buffer).error() };
        }

        // No frame after the completed one reads the slot: the old buffer can go (the release is deferred anyway)
        if (target.buffer.is_valid())
        {
            resources_.destroy(target.buffer);
        }
        target.buffer = *buffer;
        target.srv = resources_.srv(*buffer);
        target.capacity = capacity;
        ++allocations_;
        return {};
    }

    result<std::uint32_t> frame_buffer_ring::acquire(
        const std::size_t elements, const std::uint64_t frame_index, const std::uint64_t completed_frame_index)
    {
        // The free slot used longest ago, so a slot written by the frame before is read as long as possible
        std::optional<std::uint32_t> chosen;
        for (std::uint32_t index = 0; index < slots_.size(); ++index)
        {
            const slot& candidate = slots_[index];
            if (candidate.last_used_frame <= completed_frame_index &&
                (!chosen || candidate.last_used_frame < slots_[*chosen].last_used_frame))
            {
                chosen = index;
            }
        }
        if (!chosen)
        {
            log::get(logger_name)
                ->debug(
                    "frame data: every '{}' slot is in use by a frame in flight; adding slot {}", name_, slots_.size());
            chosen = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }

        slot& target = slots_[*chosen];
        if (target.capacity < std::max<std::size_t>(elements, 1) || !target.buffer.is_valid())
        {
            if (auto created = reallocate(target, elements); !created)
            {
                return std::unexpected{ std::move(created).error() };
            }
        }
        target.last_used_frame = frame_index;
        return *chosen;
    }

    void frame_buffer_ring::mark_used(const std::uint32_t slot, const std::uint64_t frame_index) noexcept
    {
        slots_[slot].last_used_frame = std::max(slots_[slot].last_used_frame, frame_index);
    }

    frame_data_upload::frame_data_upload(const rhi::resources resources, const std::uint32_t frames_in_flight) :
        resources_{ resources },
        instances_{ resources,
                    "frame instances",
                    static_cast<std::uint32_t>(sizeof(gpu::instance_data)),
                    frames_in_flight,
                    minimum_instances },
        instance_indices_{ resources,
                           "frame instance indices",
                           static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                           frames_in_flight,
                           minimum_instance_indices },
        lights_{ resources,
                 "frame lights",
                 static_cast<std::uint32_t>(sizeof(gpu::light_data)),
                 frames_in_flight,
                 minimum_lights },
        staging_(std::max<std::uint32_t>(frames_in_flight, 1))
    {}

    frame_data_upload::~frame_data_upload()
    {
        for (const staging& item : staging_)
        {
            if (item.buffer.is_valid())
            {
                resources_.destroy(item.buffer);
            }
        }
    }

    result<std::span<std::byte>> frame_data_upload::staging_for(const std::uint32_t context, const std::size_t bytes)
    {
        if (context >= staging_.size())
        {
            staging_.resize(context + 1);
        }
        staging& target = staging_[context];
        if (target.capacity < bytes || !target.buffer.is_valid())
        {
            std::size_t capacity = std::max(target.capacity, minimum_staging_bytes);
            while (capacity < bytes)
            {
                capacity *= 2;
            }
            const std::string debug_name = std::format("frame data staging {}", context);
            auto buffer = resources_.create_buffer(
                { .size = capacity,
                  .usage = rhi::buffer_usage::copy_source,
                  .memory = rhi::memory_type::upload,
                  .debug_name = debug_name });
            if (!buffer)
            {
                return std::unexpected{ std::move(buffer).error() };
            }
            // The context was recycled: frame N − F, its last reader, is complete
            if (target.buffer.is_valid())
            {
                resources_.destroy(target.buffer);
            }
            target = { .buffer = *buffer, .capacity = capacity };
        }
        return resources_.mapped_bytes(target.buffer);
    }

    result<frame_buffers> frame_data_upload::add_upload_pass(
        graphics::render_graph& graph,
        const graphics::frame_token& frame,
        const std::uint64_t completed_frame_index,
        const std::span<const gpu::instance_data> instances,
        const std::span<const gpu::light_data> lights,
        const std::span<const std::uint32_t> instance_indices,
        const bool reuse)
    {
        const std::uint64_t frame_index = frame.index();
        const bool reusing = reuse && written_.has_value();

        // The slots this frame reads
        written_slots slots;
        if (reusing)
        {
            slots = *written_;
            instances_.mark_used(slots.instances, frame_index);
            lights_.mark_used(slots.lights, frame_index);
        }
        else
        {
            written_.reset(); // a failure below leaves nothing to reuse
            auto instance_slot = instances_.acquire(instances.size(), frame_index, completed_frame_index);
            if (!instance_slot)
            {
                return std::unexpected{ std::move(instance_slot).error() };
            }
            auto light_slot = lights_.acquire(lights.size(), frame_index, completed_frame_index);
            if (!light_slot)
            {
                return std::unexpected{ std::move(light_slot).error() };
            }
            slots = { .instances = *instance_slot,
                      .lights = *light_slot,
                      .instance_count = static_cast<std::uint32_t>(instances.size()),
                      .light_count = static_cast<std::uint32_t>(lights.size()) };
        }
        auto index_slot = instance_indices_.acquire(instance_indices.size(), frame_index, completed_frame_index);
        if (!index_slot)
        {
            return std::unexpected{ std::move(index_slot).error() };
        }

        // The staging layout: each array that is copied, on a 256-byte boundary
        const std::array<std::span<const std::byte>, 3> arrays{
            reusing ? std::span<const std::byte>{} : std::as_bytes(instances),
            std::as_bytes(instance_indices),
            reusing ? std::span<const std::byte>{} : std::as_bytes(lights),
        };
        std::array<copy_region, 3> copies{};
        std::uint32_t copy_count = 0;
        std::size_t staging_bytes = 0;
        for (std::uint32_t target = 0; target < arrays.size(); ++target)
        {
            if (!arrays[target].empty())
            {
                copies[copy_count++] = { .target = target,
                                         .source_offset = staging_bytes,
                                         .size = arrays[target].size() };
                staging_bytes = align_up(staging_bytes + arrays[target].size(), staging_alignment);
            }
        }

        rhi::buffer_handle staging_buffer;
        if (copy_count > 0)
        {
            auto mapped = staging_for(frame.context_index(), staging_bytes);
            if (!mapped)
            {
                return std::unexpected{ std::move(mapped).error() };
            }
            for (std::uint32_t copy = 0; copy < copy_count; ++copy)
            {
                const std::span<const std::byte> source = arrays[copies[copy].target];
                std::memcpy(mapped->data() + copies[copy].source_offset, source.data(), source.size());
                uploaded_bytes_ += source.size();
            }
            staging_buffer = staging_[frame.context_index()].buffer;
        }

        // The slots in the graph, outside any view. No frame after the completed one used them, so `common` holds.
        const rhi::access_state common = rhi::access_states::common;
        frame_buffers buffers{
            .instances = graph.import_buffer("frame_instances", instances_.buffer(slots.instances), common, common),
            .instance_indices =
                graph.import_buffer("frame_instance_indices", instance_indices_.buffer(*index_slot), common, common),
            .lights = graph.import_buffer("frame_lights", lights_.buffer(slots.lights), common, common),
            .instances_srv = instances_.srv(slots.instances),
            .instance_indices_srv = instance_indices_.srv(*index_slot),
            .lights_srv = lights_.srv(slots.lights),
            .instance_count = slots.instance_count,
            .light_count = slots.light_count,
        };

        // Pass 0. The staging buffer isn't in the graph: it is written by the CPU before the submission, and copies
        // from an upload heap need no barrier.
        if (copy_count > 0)
        {
            const std::array<graphics::buffer, 3> targets{ buffers.instances,
                                                           buffers.instance_indices,
                                                           buffers.lights };
            const upload_pass_data& pass = graph.add_copy_pass<upload_pass_data>(
                "upload_frame_data",
                [&](graphics::render_pass_builder& builder, upload_pass_data& data)
                {
                    data.targets = targets;
                    for (std::uint32_t copy = 0; copy < copy_count; ++copy)
                    {
                        const std::uint32_t target = copies[copy].target;
                        data.targets[target] = builder.write(targets[target], graphics::buffer_write::copy_destination);
                    }
                },
                [copies, copy_count, staging_buffer](
                    const upload_pass_data& data,
                    const graphics::resource_bag& resources,
                    graphics::copy_pass_context& context)
                {
                    for (std::uint32_t copy = 0; copy < copy_count; ++copy)
                    {
                        const copy_region& region = copies[copy];
                        context.recorder().copy_buffer(
                            resources.buffer_handle(data.targets[region.target]),
                            0,
                            staging_buffer,
                            region.source_offset,
                            region.size);
                    }
                });
            buffers.instances = pass.targets[0];
            buffers.instance_indices = pass.targets[1];
            buffers.lights = pass.targets[2];
        }

        if (reusing)
        {
            ++reuses_;
        }
        else
        {
            ++uploads_;
            written_ = slots;
        }
        last_instance_slot_ = slots.instances;
        return buffers;
    }

    frame_data_statistics frame_data_upload::statistics() const
    {
        std::size_t staging_bytes = 0;
        for (const staging& item : staging_)
        {
            staging_bytes += item.capacity;
        }
        return {
            .uploads = uploads_,
            .reuses = reuses_,
            .uploaded_bytes = uploaded_bytes_,
            .instance_slot = last_instance_slot_,
            .instance_slots = instances_.slot_count(),
            .buffer_allocations = instances_.allocations() + instance_indices_.allocations() + lights_.allocations(),
            .staging_bytes = staging_bytes,
        };
    }
} // namespace avis::visualizer::rendering
