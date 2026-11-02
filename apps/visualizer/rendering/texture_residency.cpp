#include "rendering/texture_residency.h"

#include "avis/detail/check.h"
#include "avis/graphics/error.h"
#include "avis/graphics/upload_queue.h"
#include "avis/log.h"

#include <algorithm>
#include <bit>
#include <format>
#include <tuple>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";
        constexpr auto warning_interval = std::chrono::seconds{ 1 };

        // RGBA8 values of the defaults, in default_texture order
        constexpr std::array<std::array<std::uint8_t, 4>, default_texture_count> default_texels{ {
            { 255, 255, 255, 255 },
            { 0, 0, 0, 255 },
            { 128, 128, 255, 255 },
            { 255, 0, 255, 255 },
        } };

        constexpr std::array<std::string_view, default_texture_count> default_names{
            "default texture white",
            "default texture black",
            "default texture flat normal",
            "default texture missing",
        };

        std::string describe(const scene::texture_handle handle, const std::uint64_t version)
        {
            return std::format("texture {}:{} version {}", handle.index, handle.generation, version);
        }

        bool generates_mips(const scene::texture_data& data, const rhi::format format) noexcept
        {
            return data.generate_mips &&
                   (format == rhi::format::rgba8_unorm || format == rhi::format::rgba8_unorm_srgb) &&
                   full_mip_count(data.width, data.height) > 1;
        }
    } // namespace

    std::uint32_t full_mip_count(const std::uint32_t width, const std::uint32_t height) noexcept
    {
        const std::uint32_t largest = std::max({ width, height, 1u });
        return static_cast<std::uint32_t>(std::bit_width(largest));
    }

    rhi::format to_rhi_format(const scene::texture_format format) noexcept
    {
        switch (format)
        {
        case scene::texture_format::rgba8_unorm:
            return rhi::format::rgba8_unorm;
        case scene::texture_format::rgba8_srgb:
            return rhi::format::rgba8_unorm_srgb;
        case scene::texture_format::bc4_unorm:
            return rhi::format::bc4_unorm;
        case scene::texture_format::bc5_unorm:
            return rhi::format::bc5_unorm;
        case scene::texture_format::bc7_unorm:
            return rhi::format::bc7_unorm;
        case scene::texture_format::bc7_srgb:
            return rhi::format::bc7_unorm_srgb;
        }
        return rhi::format::rgba8_unorm;
    }

    result<std::unique_ptr<texture_residency>> texture_residency::create(
        rhi::resources resources, graphics::upload_queue& uploads, const rhi::synchronization synchronization)
    {
        std::unique_ptr<texture_residency> residency{ new texture_residency{ resources, uploads, synchronization } };
        for (std::size_t which = 0; which < default_texture_count; ++which)
        {
            auto texture = resources.create_texture(
                { .width = 1,
                  .height = 1,
                  .format = rhi::format::rgba8_unorm,
                  .usage = rhi::texture_usage::shader_resource | rhi::texture_usage::copy_destination,
                  .initial_layout = rhi::texture_layout::common,
                  .debug_name = default_names[which] });
            if (!texture)
            {
                return std::unexpected{ std::move(texture).error() };
            }
            resident_texture& made = residency->defaults_[which];
            made.version = 1;
            made.texture = *texture;
            made.srv = resources.srv(*texture);

            const graphics::subresource_data texel{ .bytes = std::as_bytes(std::span{ default_texels[which] }) };
            auto ticket = uploads.upload_texture(*texture, std::span{ &texel, 1 });
            if (!ticket)
            {
                return std::unexpected{ std::move(ticket).error() };
            }
            residency->default_tickets_[which] = *ticket;
        }
        return residency;
    }

    texture_residency::texture_residency(
        rhi::resources resources, graphics::upload_queue& uploads, const rhi::synchronization synchronization) noexcept
        :
        resources_{ resources },
        uploads_{ uploads },
        synchronization_{ synchronization }
    {}

    texture_residency::~texture_residency()
    {
        for (auto& [handle, owner] : records_)
        {
            for (const entry& item : owner.entries)
            {
                resources_.destroy(item.texture.texture);
            }
        }
        for (const resident_texture& made : defaults_)
        {
            if (made.texture.is_valid())
            {
                resources_.destroy(made.texture);
            }
        }
    }

    void texture_residency::update(const scene::snapshot& snapshot, const std::uint64_t frame_index)
    {
        frame_index_ = frame_index;
        for (auto& [handle, owner] : records_)
        {
            owner.in_latest_snapshot = false;
        }

        for (const scene::versioned_texture_entry& listed : snapshot.textures)
        {
            if (listed.texture.version == 0 || !listed.texture.data)
            {
                continue; // a stale handle
            }
            record& owner = records_[listed.handle];
            owner.in_latest_snapshot = true;
            if (owner.failed_version == listed.texture.version)
            {
                continue;
            }

            entry* held = nullptr;
            for (entry& item : owner.entries)
            {
                if (item.texture.version == listed.texture.version && !item.abandoned)
                {
                    held = &item;
                }
            }
            if (held != nullptr)
            {
                if (held->state == entry_state::uploading && held->data)
                {
                    upload(listed.handle, owner, *held); // the ring was full last frame
                }
                continue;
            }

            // A new version: one that isn't current yet will never draw
            for (entry& item : owner.entries)
            {
                if (item.state != entry_state::ready && !item.abandoned)
                {
                    item.abandoned = true;
                    item.data.reset();
                }
            }

            const scene::texture_data& data = *listed.texture.data;
            const rhi::format format = to_rhi_format(data.format);
            if (data.width == 0 || data.height == 0 || data.mips.empty())
            {
                owner.failed_version = listed.texture.version;
                ++status_changes_;
                warn(listed.handle, owner, listed.texture.version, "it has no texels");
                continue;
            }
            const bool generate = generates_mips(data, format);
            const std::uint32_t mip_levels =
                generate ? full_mip_count(data.width, data.height) : static_cast<std::uint32_t>(data.mips.size());
            const std::string debug_name = describe(listed.handle, listed.texture.version);
            auto texture = resources_.create_texture(
                { .width = data.width,
                  .height = data.height,
                  .mip_levels = mip_levels,
                  .format = format,
                  .usage = generate ? rhi::texture_usage::shader_resource | rhi::texture_usage::copy_destination |
                                          rhi::texture_usage::copy_source :
                                      rhi::texture_usage::shader_resource | rhi::texture_usage::copy_destination,
                  .initial_layout = rhi::texture_layout::common,
                  .debug_name = debug_name });
            if (!texture)
            {
                // Not retried: the same data would fail again. Materials draw it as missing.
                owner.failed_version = listed.texture.version;
                ++status_changes_;
                warn(listed.handle, owner, listed.texture.version, std::format("{}", texture.error()));
                continue;
            }

            entry item;
            item.texture.version = listed.texture.version;
            item.texture.texture = *texture;
            item.texture.srv = resources_.srv(*texture);
            item.texture.width = data.width;
            item.texture.height = data.height;
            item.texture.mip_levels = mip_levels;
            item.texture.format = format;
            item.texture.last_used_frame = frame_index;
            item.data = listed.texture.data;
            item.generate_mips = generate;
            item.srgb = rhi::traits(format).srgb;
            owner.entries.push_back(std::move(item));
            upload(listed.handle, owner, owner.entries.back());
        }
    }

    bool texture_residency::upload(const scene::texture_handle handle, record& owner, entry& item)
    {
        const scene::texture_data& data = *item.data;
        // Decoded images upload mip 0; their chain is generated
        const std::size_t count = item.generate_mips ? 1 : data.mips.size();
        std::vector<graphics::subresource_data> subresources;
        subresources.reserve(count);
        std::uint64_t bytes = 0;
        for (std::size_t mip = 0; mip < count; ++mip)
        {
            subresources.push_back({ .bytes = data.mips[mip] });
            bytes += data.mips[mip].size();
        }

        result<graphics::upload_ticket> ticket = uploads_.upload_texture(item.texture.texture, subresources);
        if (!ticket)
        {
            if (!graphics::is_error(ticket.error(), graphics::error_kind::upload_queue_full))
            {
                // The queue refused the data itself: retrying won't help
                owner.failed_version = item.texture.version;
                ++status_changes_;
                item.abandoned = true;
                item.data.reset();
                warn(handle, owner, item.texture.version, std::format("{}", ticket.error()));
                return false;
            }
            ++failed_uploads_;
            warn(handle, owner, item.texture.version, std::format("upload failed: {}", ticket.error()));
            return false;
        }
        item.ticket = *ticket;
        item.data.reset(); // the texels are in the staging ring
        item.texture.last_used_frame = frame_index_;
        uploaded_bytes_ += bytes;
        return true;
    }

    void texture_residency::warn(
        const scene::texture_handle handle, record& owner, const std::uint64_t version, const std::string& reason)
    {
        const auto now = std::chrono::steady_clock::now();
        if (owner.last_warning && now - *owner.last_warning < warning_interval)
        {
            return;
        }
        owner.last_warning = now;
        log::get(logger_name)
            ->warn(
                "residency: {} not made resident this frame: {}. Materials draw the previous version or a default.",
                describe(handle, version),
                reason);
    }

    void texture_residency::after_flush()
    {
        const auto resolve = [this](std::optional<graphics::upload_ticket>& ticket, rhi::synchronization_point& ready)
        {
            if (!ticket)
            {
                return;
            }
            const std::optional<rhi::synchronization_point> point = uploads_.point(*ticket);
            if (!AVIS_CHECK(
                    logger_name,
                    point.has_value(),
                    "residency: an upload ticket resolved to nullopt, so its flush hasn't happened (11 §3.6): flush "
                    "the upload queue before after_flush"))
            {
                return; // kept for the next after_flush
            }
            ready = rhi::latest(ready, *point);
            ticket.reset();
        };

        mip_requests_.clear();
        frame_ready_after_ = {};
        for (std::size_t which = 0; which < default_texture_count; ++which)
        {
            resolve(default_tickets_[which], defaults_[which].ready_after);
            frame_ready_after_ = rhi::latest(frame_ready_after_, defaults_[which].ready_after);
        }

        for (auto& [handle, owner] : records_)
        {
            for (entry& item : owner.entries)
            {
                resolve(item.ticket, item.texture.ready_after);
                if (item.abandoned)
                {
                    continue;
                }
                if (item.state == entry_state::uploading && !item.ticket && !item.data)
                {
                    item.state = item.generate_mips ? entry_state::needs_mips : entry_state::ready;
                    ++status_changes_;
                }
                if (item.state == entry_state::needs_mips)
                {
                    item.texture.last_used_frame = frame_index_; // the mip passes of this frame use it
                    mip_requests_.push_back(
                        { .handle = handle,
                          .version = item.texture.version,
                          .texture = item.texture.texture,
                          .width = item.texture.width,
                          .height = item.texture.height,
                          .mip_levels = item.texture.mip_levels,
                          .srgb = item.srgb,
                          .ready_after = item.texture.ready_after });
                }
            }

            if (owner.in_latest_snapshot)
            {
                if (entry* current = newest_ready(owner))
                {
                    current->texture.last_used_frame = frame_index_;
                    frame_ready_after_ = rhi::latest(frame_ready_after_, current->texture.ready_after);
                }
            }
        }

        // The graph's passes in a reproducible order
        std::ranges::sort(
            mip_requests_,
            [](const mip_request& left, const mip_request& right)
            { return std::tie(left.handle, left.version) < std::tie(right.handle, right.version); });
    }

    void texture_residency::mips_recorded()
    {
        for (const mip_request& request : mip_requests_)
        {
            const auto found = records_.find(request.handle);
            if (found == records_.end())
            {
                continue;
            }
            for (entry& item : found->second.entries)
            {
                if (item.texture.version == request.version && item.state == entry_state::needs_mips && !item.abandoned)
                {
                    item.state = entry_state::ready;
                    ++mip_chains_generated_;
                    ++status_changes_;
                }
            }
        }
        mip_requests_.clear();
    }

    const resident_texture* texture_residency::find(const scene::texture_handle texture) const
    {
        const auto found = records_.find(texture);
        if (found == records_.end())
        {
            return nullptr;
        }
        const entry* current = newest_ready(found->second);
        return current == nullptr ? nullptr : &current->texture;
    }

    texture_status texture_residency::status(const scene::texture_handle texture) const
    {
        const auto found = records_.find(texture);
        if (found == records_.end() || !found->second.in_latest_snapshot)
        {
            return texture_status::unknown;
        }
        const record& owner = found->second;
        if (newest_ready(owner) != nullptr)
        {
            return texture_status::resident;
        }
        const bool pending = std::ranges::any_of(
            owner.entries, [](const entry& item) { return item.state != entry_state::ready && !item.abandoned; });
        if (pending)
        {
            return texture_status::pending;
        }
        return owner.failed_version != 0 ? texture_status::failed : texture_status::unknown;
    }

    rhi::bindless_index texture_residency::default_srv(const default_texture which) const noexcept
    {
        return defaults_[static_cast<std::size_t>(which)].srv;
    }

    const resident_texture& texture_residency::default_resident(const default_texture which) const noexcept
    {
        return defaults_[static_cast<std::size_t>(which)];
    }

    void texture_residency::mark_used(
        const scene::texture_handle texture, const std::uint64_t version, const std::uint64_t frame_index) noexcept
    {
        const auto found = records_.find(texture);
        if (found == records_.end())
        {
            return;
        }
        for (entry& item : found->second.entries)
        {
            if (item.texture.version == version)
            {
                item.texture.last_used_frame = std::max(item.texture.last_used_frame, frame_index);
            }
        }
    }

    void texture_residency::retire(const std::uint64_t completed_frame_index)
    {
        for (auto record_it = records_.begin(); record_it != records_.end();)
        {
            record& owner = record_it->second;
            const entry* current = owner.in_latest_snapshot ? newest_ready(owner) : nullptr;
            const std::optional<std::uint64_t> current_version =
                current != nullptr ? std::optional{ current->texture.version } : std::nullopt;

            std::erase_if(
                owner.entries,
                [&](entry& item)
                {
                    if (item.state == entry_state::ready && item.texture.version == current_version)
                    {
                        return false;
                    }
                    if (item.state != entry_state::ready && !item.abandoned)
                    {
                        if (owner.in_latest_snapshot)
                        {
                            return false; // still to become current
                        }
                        item.abandoned = true; // its texture left the snapshot first
                        item.data.reset();
                    }
                    if (item.ticket || item.texture.last_used_frame > completed_frame_index)
                    {
                        return false;
                    }
                    if (item.texture.ready_after.value != 0 && !synchronization_.is_complete(item.texture.ready_after))
                    {
                        return false;
                    }
                    resources_.destroy(item.texture.texture);
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

    texture_residency_statistics texture_residency::statistics() const
    {
        texture_residency_statistics statistics{ .uploaded_bytes = uploaded_bytes_,
                                                 .failed_uploads = failed_uploads_,
                                                 .mip_chains_generated = mip_chains_generated_ };
        for (const auto& [handle, owner] : records_)
        {
            if (newest_ready(owner) != nullptr)
            {
                ++statistics.textures;
            }
            if (owner.failed_version != 0)
            {
                ++statistics.failed;
            }
            for (const entry& item : owner.entries)
            {
                if (item.state == entry_state::ready)
                {
                    ++statistics.resident_versions;
                }
                else if (!item.abandoned)
                {
                    ++statistics.pending;
                }
            }
        }
        return statistics;
    }

    std::vector<std::uint64_t> texture_residency::versions(const scene::texture_handle texture) const
    {
        std::vector<std::uint64_t> held;
        if (const auto found = records_.find(texture); found != records_.end())
        {
            for (const entry& item : found->second.entries)
            {
                held.push_back(item.texture.version);
            }
        }
        return held;
    }

    const texture_residency::entry* texture_residency::newest_ready(const record& owner) noexcept
    {
        for (auto item = owner.entries.rbegin(); item != owner.entries.rend(); ++item)
        {
            if (item->state == entry_state::ready)
            {
                return &*item;
            }
        }
        return nullptr;
    }

    texture_residency::entry* texture_residency::newest_ready(record& owner) noexcept
    {
        return const_cast<entry*>(newest_ready(std::as_const(owner)));
    }
} // namespace avis::visualizer::rendering
