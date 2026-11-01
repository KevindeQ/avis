#include "rendering/material_table.h"

#include "avis/detail/check.h"
#include "avis/graphics/binding_layout.h"
#include "avis/graphics/error.h"
#include "avis/graphics/upload_queue.h"
#include "avis/log.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";
        constexpr auto warning_interval = std::chrono::seconds{ 1 };
        constexpr std::uint64_t slot_size = sizeof(gpu::material_data);

        std::string describe(const scene::material_handle handle, const std::uint64_t version)
        {
            return std::format("material {}:{} version {}", handle.index, handle.generation, version);
        }

        // What stands in for a texture that isn't resident yet
        constexpr default_texture stand_in(const material_texture field) noexcept
        {
            switch (field)
            {
            case material_texture::normal:
                return default_texture::flat_normal;
            case material_texture::emissive:
                return default_texture::black;
            case material_texture::base_color:
            case material_texture::metallic_roughness:
            case material_texture::occlusion:
                break;
            }
            return default_texture::white;
        }

        bool same_data(const gpu::material_data& left, const gpu::material_data& right) noexcept
        {
            return std::memcmp(&left, &right, sizeof(gpu::material_data)) == 0;
        }

        // The default material: white, lit, opaque, no textures
        gpu::material_data default_material_data() noexcept
        {
            gpu::material_data data{};
            data.base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };
            data.metallic_factor = 0.0f;
            data.roughness_factor = 1.0f;
            data.normal_scale = 1.0f;
            data.occlusion_strength = 1.0f;
            data.alpha_cutoff = 0.5f;
            data.sampler_index = static_cast<std::uint32_t>(graphics::binding_layout::static_sampler::anisotropic_wrap);
            data.shading_model = gpu::shading_model_lit;
            data.alpha_mode = gpu::alpha_mode_opaque;
            data.blend_operation = gpu::blend_operation_normal;
            data.point_size = 0.05f;
            data.line_width = 1.0f;
            return data;
        }
    } // namespace

    gpu::material_data make_material_data(
        const scene::material_description& description,
        const texture_residency& textures,
        std::array<referenced_texture, material_texture_count>& referenced)
    {
        gpu::material_data data{};
        data.base_color_factor = { description.base_color_factor.red,
                                   description.base_color_factor.green,
                                   description.base_color_factor.blue,
                                   description.base_color_factor.alpha };
        data.metallic_factor = description.metallic_factor;
        data.roughness_factor = description.roughness_factor;
        data.normal_scale = description.normal_scale;
        data.occlusion_strength = description.occlusion_strength;
        data.emissive_factor = { description.emissive_factor.x(),
                                 description.emissive_factor.y(),
                                 description.emissive_factor.z() };
        data.alpha_cutoff = description.alpha_cutoff;
        // Material textures repeat (glTF's default wrap); the 16x anisotropic sampler is the wrapping one
        data.sampler_index = static_cast<std::uint32_t>(graphics::binding_layout::static_sampler::anisotropic_wrap);
        data.shading_model =
            description.shading == scene::shading_model::unlit ? gpu::shading_model_unlit : gpu::shading_model_lit;
        switch (description.alpha)
        {
        case scene::alpha_mode::opaque:
            data.alpha_mode = gpu::alpha_mode_opaque;
            break;
        case scene::alpha_mode::mask:
            data.alpha_mode = gpu::alpha_mode_mask;
            break;
        case scene::alpha_mode::blend:
            data.alpha_mode = gpu::alpha_mode_blend;
            break;
        }
        switch (description.blend)
        {
        case scene::blend_operation::normal:
            data.blend_operation = gpu::blend_operation_normal;
            break;
        case scene::blend_operation::additive:
            data.blend_operation = gpu::blend_operation_additive;
            break;
        case scene::blend_operation::subtractive:
            data.blend_operation = gpu::blend_operation_subtractive;
            break;
        }
        data.flags = (description.double_sided ? gpu::material_flag_double_sided : 0u) |
                     (description.use_vertex_color ? gpu::material_flag_use_vertex_color : 0u);
        data.point_size = description.point_size;
        data.line_width = description.line_width;

        const std::array<std::pair<scene::texture_handle, std::uint32_t*>, material_texture_count> fields{ {
            { description.base_color_texture, &data.base_color_texture },
            { description.metallic_roughness_texture, &data.metallic_roughness_texture },
            { description.normal_texture, &data.normal_texture },
            { description.occlusion_texture, &data.occlusion_texture },
            { description.emissive_texture, &data.emissive_texture },
        } };
        for (std::size_t field = 0; field < material_texture_count; ++field)
        {
            const auto [handle, index] = fields[field];
            referenced[field] = {};
            if (!handle.valid())
            {
                *index = 0; // no texture: the factors alone
                continue;
            }
            switch (textures.status(handle))
            {
            case texture_status::resident:
                {
                    const resident_texture* resident = textures.find(handle);
                    *index = resident->srv.value;
                    referenced[field] = { .handle = handle, .version = resident->version };
                    break;
                }
            case texture_status::pending:
                *index = textures.default_srv(stand_in(static_cast<material_texture>(field))).value;
                break;
            case texture_status::failed:
            case texture_status::unknown:
                *index = textures.default_srv(default_texture::missing).value;
                break;
            }
        }
        return data;
    }

    result<std::unique_ptr<material_table>> material_table::create(
        rhi::resources resources,
        graphics::upload_queue& uploads,
        const rhi::synchronization synchronization,
        texture_residency& textures,
        const std::uint32_t capacity)
    {
        if (capacity < 2 || capacity > maximum_slots)
        {
            return std::unexpected{ graphics::make_error(
                graphics::error_kind::invalid_argument,
                std::format("material table: {} slots, expected 2 to {}", capacity, maximum_slots)) };
        }
        auto buffer = resources.create_buffer(
            { .size = std::uint64_t{ capacity } * slot_size,
              .structure_stride = static_cast<std::uint32_t>(slot_size),
              .usage = rhi::buffer_usage::shader_resource | rhi::buffer_usage::copy_destination,
              .memory = rhi::memory_type::gpu_only,
              .debug_name = "material table" });
        if (!buffer)
        {
            return std::unexpected{ std::move(buffer).error() };
        }

        std::unique_ptr<material_table> table{ new material_table{
            resources, uploads, synchronization, textures, *buffer, capacity } };
        entry& defaults = table->default_entry_;
        defaults.material.version = 1;
        defaults.material.slot = default_slot;
        defaults.material.data = default_material_data();
        auto ticket = uploads.upload_buffer(*buffer, 0, std::as_bytes(std::span{ &defaults.material.data, 1 }));
        if (!ticket)
        {
            return std::unexpected{ std::move(ticket).error() };
        }
        defaults.written = true;
        defaults.ticket = *ticket;
        ++table->writes_;
        return table;
    }

    material_table::material_table(
        rhi::resources resources,
        graphics::upload_queue& uploads,
        const rhi::synchronization synchronization,
        texture_residency& textures,
        const rhi::buffer_handle buffer,
        const std::uint32_t capacity) noexcept :
        resources_{ resources },
        uploads_{ uploads },
        synchronization_{ synchronization },
        textures_{ textures },
        buffer_{ buffer },
        buffer_srv_{ resources.srv(buffer) },
        capacity_{ capacity }
    {}

    material_table::~material_table()
    {
        resources_.destroy(buffer_);
    }

    std::optional<std::uint32_t> material_table::allocate_slot() noexcept
    {
        if (!free_slots_.empty())
        {
            // The lowest freed slot, so slots stay compact
            const auto lowest = std::ranges::min_element(free_slots_);
            const std::uint32_t slot = *lowest;
            *lowest = free_slots_.back();
            free_slots_.pop_back();
            return slot;
        }
        if (next_slot_ < capacity_)
        {
            return next_slot_++;
        }
        return std::nullopt;
    }

    void material_table::free_slot(const std::uint32_t slot) noexcept
    {
        try
        {
            free_slots_.push_back(slot);
        }
        catch (...)
        {
            // Out of memory: the slot is lost, which only costs capacity
        }
    }

    void material_table::update(const scene::snapshot& snapshot, const std::uint64_t frame_index)
    {
        frame_index_ = frame_index;
        for (auto& [handle, owner] : records_)
        {
            owner.in_latest_snapshot = false;
        }

        for (const scene::versioned_material_entry& listed : snapshot.materials)
        {
            if (listed.material.version == 0 || !listed.material.data)
            {
                continue; // a stale handle: instances draw the default material
            }
            record& owner = records_[listed.handle];
            owner.in_latest_snapshot = true;

            std::array<referenced_texture, material_texture_count> referenced{};
            const gpu::material_data data = make_material_data(*listed.material.data, textures_, referenced);

            // The newest entry that may still draw: up to date, or still to be written
            entry* newest = nullptr;
            for (entry& item : owner.entries)
            {
                if (!item.abandoned)
                {
                    newest = &item;
                }
            }
            if (newest != nullptr && newest->material.version == listed.material.version &&
                same_data(newest->material.data, data))
            {
                if (!newest->written)
                {
                    write(listed.handle, owner, *newest); // the ring was full last frame
                }
                continue;
            }

            // A new version, or new texture resolutions: a new slot (frames in flight read the old one)
            const bool rewrite = newest != nullptr && newest->material.version == listed.material.version;
            for (entry& item : owner.entries)
            {
                if (!item.ready && !item.abandoned)
                {
                    item.abandoned = true;
                }
            }
            const std::optional<std::uint32_t> slot = allocate_slot();
            if (!slot)
            {
                ++failed_writes_;
                warn(
                    listed.handle,
                    owner,
                    listed.material.version,
                    std::format("all {} material slots are in use", capacity_));
                continue;
            }

            entry item;
            item.material.version = listed.material.version;
            item.material.slot = *slot;
            item.material.data = data;
            item.material.textures = referenced;
            item.material.last_used_frame = frame_index;
            owner.entries.push_back(item);
            if (rewrite)
            {
                ++rewrites_;
            }
            write(listed.handle, owner, owner.entries.back());
        }
    }

    bool material_table::write(const scene::material_handle handle, record& owner, entry& item)
    {
        result<graphics::upload_ticket> ticket = uploads_.upload_buffer(
            buffer_,
            std::uint64_t{ item.material.slot } * slot_size,
            std::as_bytes(std::span{ &item.material.data, 1 }));
        if (!ticket)
        {
            ++failed_writes_;
            warn(handle, owner, item.material.version, std::format("upload failed: {}", ticket.error()));
            return false;
        }
        item.written = true;
        item.ticket = *ticket;
        item.material.last_used_frame = frame_index_;
        ++writes_;
        return true;
    }

    void material_table::warn(
        const scene::material_handle handle, record& owner, const std::uint64_t version, const std::string& reason)
    {
        const auto now = std::chrono::steady_clock::now();
        if (owner.last_warning && now - *owner.last_warning < warning_interval)
        {
            return;
        }
        owner.last_warning = now;
        log::get(logger_name)
            ->warn(
                "residency: {} not written this frame: {}. Retried next frame; the previous slot or the default "
                "material draws.",
                describe(handle, version),
                reason);
    }

    void material_table::after_flush()
    {
        const auto resolve = [this](entry& item)
        {
            if (!item.ticket)
            {
                return;
            }
            const std::optional<rhi::synchronization_point> point = uploads_.point(*item.ticket);
            if (!AVIS_CHECK(
                    logger_name,
                    point.has_value(),
                    "residency: an upload ticket resolved to nullopt, so its flush hasn't happened (11 §3.6): flush "
                    "the upload queue before after_flush"))
            {
                return; // kept for the next after_flush
            }
            item.material.ready_after = rhi::latest(item.material.ready_after, *point);
            item.ticket.reset();
            if (item.written && !item.abandoned)
            {
                item.ready = true;
            }
        };

        resolve(default_entry_);
        frame_ready_after_ = default_entry_.material.ready_after;
        for (auto& [handle, owner] : records_)
        {
            for (entry& item : owner.entries)
            {
                resolve(item);
            }
            if (!owner.in_latest_snapshot)
            {
                continue;
            }
            if (entry* current = newest_ready(owner))
            {
                current->material.last_used_frame = frame_index_;
                frame_ready_after_ = rhi::latest(frame_ready_after_, current->material.ready_after);
                for (const referenced_texture& texture : current->material.textures)
                {
                    if (texture.handle.valid())
                    {
                        textures_.mark_used(texture.handle, texture.version, frame_index_);
                    }
                }
            }
        }
    }

    std::uint32_t material_table::slot(const scene::material_handle material) const
    {
        const resident_material* current = find(material);
        return current == nullptr ? default_slot : current->slot;
    }

    const resident_material* material_table::find(const scene::material_handle material) const
    {
        const auto found = records_.find(material);
        if (found == records_.end())
        {
            return nullptr;
        }
        const entry* current = newest_ready(found->second);
        return current == nullptr ? nullptr : &current->material;
    }

    void material_table::retire(const std::uint64_t completed_frame_index)
    {
        for (auto record_it = records_.begin(); record_it != records_.end();)
        {
            record& owner = record_it->second;
            const entry* current = owner.in_latest_snapshot ? newest_ready(owner) : nullptr;
            const std::optional<std::uint32_t> current_slot =
                current != nullptr ? std::optional{ current->material.slot } : std::nullopt;

            std::erase_if(
                owner.entries,
                [&](entry& item)
                {
                    if (item.ready && item.material.slot == current_slot)
                    {
                        return false;
                    }
                    if (!item.ready && !item.abandoned)
                    {
                        if (owner.in_latest_snapshot)
                        {
                            return false; // still to be written or flushed
                        }
                        item.abandoned = true; // its material left the snapshot first
                    }
                    if (item.ticket || item.material.last_used_frame > completed_frame_index)
                    {
                        return false;
                    }
                    if (item.material.ready_after.value != 0 &&
                        !synchronization_.is_complete(item.material.ready_after))
                    {
                        return false;
                    }
                    free_slot(item.material.slot);
                    return true;
                });

            if (!owner.in_latest_snapshot && owner.entries.empty())
            {
                record_it = records_.erase(record_it);
            }
            else
            {
                ++record_it;
            }
        }
    }

    material_table_statistics material_table::statistics() const
    {
        material_table_statistics statistics{ .capacity = capacity_,
                                              .slots_used = next_slot_ - static_cast<std::uint32_t>(free_slots_.size()),
                                              .writes = writes_,
                                              .rewrites = rewrites_,
                                              .failed_writes = failed_writes_ };
        for (const auto& [handle, owner] : records_)
        {
            if (newest_ready(owner) != nullptr)
            {
                ++statistics.materials;
            }
            for (const entry& item : owner.entries)
            {
                if (!item.ready && !item.abandoned)
                {
                    ++statistics.pending_writes;
                }
            }
        }
        return statistics;
    }

    std::vector<std::uint64_t> material_table::versions(const scene::material_handle material) const
    {
        std::vector<std::uint64_t> held;
        if (const auto found = records_.find(material); found != records_.end())
        {
            for (const entry& item : found->second.entries)
            {
                held.push_back(item.material.version);
            }
        }
        return held;
    }

    const material_table::entry* material_table::newest_ready(const record& owner) noexcept
    {
        for (auto item = owner.entries.rbegin(); item != owner.entries.rend(); ++item)
        {
            if (item->ready)
            {
                return &*item;
            }
        }
        return nullptr;
    }

    material_table::entry* material_table::newest_ready(record& owner) noexcept
    {
        return const_cast<entry*>(newest_ready(std::as_const(owner)));
    }
} // namespace avis::visualizer::rendering
