#include "avis/graphics/render_pass_builder.h"

#include "render_graph_tables.h"

#include "avis/detail/check.h"
#include "avis/graphics/render_graph.h"

#include <cstdint>
#include <string_view>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";

        using detail::access_direction;
        using detail::declared_access;
        using detail::no_index;
        using detail::render_graph_tables;
        using detail::resource_kind;
        using detail::version_entry;

        // The version-table index of a handle, or no_index after a failed debug check
        std::uint32_t find_version(
            const render_graph_tables& tables,
            const resource& handle,
            const resource_kind kind,
            [[maybe_unused]] const std::uint32_t frame_serial)
        {
            if (!AVIS_CHECK(logger_name, handle.valid(), "render_pass_builder: invalid resource handle"))
            {
                return no_index;
            }
#ifndef NDEBUG
            if (!AVIS_CHECK(
                    logger_name,
                    handle.frame_serial() == frame_serial,
                    "render_pass_builder: resource handle of another frame"))
            {
                return no_index;
            }
#endif
            if (!AVIS_CHECK(
                    logger_name,
                    handle.index() < tables.resources.size() && tables.resources[handle.index()].kind == kind &&
                        handle.version() < tables.resources[handle.index()].versions.size(),
                    "render_pass_builder: resource handle isn't in this graph"))
            {
                return no_index;
            }
            return tables.resources[handle.index()].versions[handle.version()];
        }
    } // namespace

    texture render_pass_builder::create_texture(const std::string_view name, const texture_description& description)
    {
        render_graph_tables& tables = *graph_.tables_;
        const auto index = static_cast<std::uint32_t>(tables.resources.size());
        detail::resource_entry& entry = tables.resources.emplace_back(graph_.arena_);
        entry.name = graph_.qualify(name);
        entry.kind = resource_kind::texture;
        entry.texture = description;
        entry.extent = description.size.resolve(graph_.view_size().width, graph_.view_size().height);
        entry.creator = pass_index_;
        entry.versions.push_back(static_cast<std::uint32_t>(tables.versions.size()));
        tables.versions.push_back(version_entry{ .resource = index, .version = 0 });
        return detail::resource_handles::make<texture>(index, 0, graph_.frame_serial_);
    }

    buffer render_pass_builder::create_buffer(const std::string_view name, const buffer_description& description)
    {
        render_graph_tables& tables = *graph_.tables_;
        const auto index = static_cast<std::uint32_t>(tables.resources.size());
        detail::resource_entry& entry = tables.resources.emplace_back(graph_.arena_);
        entry.name = graph_.qualify(name);
        entry.kind = resource_kind::buffer;
        entry.buffer = description;
        entry.creator = pass_index_;
        entry.versions.push_back(static_cast<std::uint32_t>(tables.versions.size()));
        tables.versions.push_back(version_entry{ .resource = index, .version = 0 });
        return detail::resource_handles::make<buffer>(index, 0, graph_.frame_serial_);
    }

    texture render_pass_builder::read(const texture input, const texture_read usage, const subresource_range range)
    {
        record_read(input, resource_kind::texture, static_cast<std::uint8_t>(usage), range);
        return input;
    }

    buffer render_pass_builder::read(const buffer input, const buffer_read usage)
    {
        record_read(input, resource_kind::buffer, static_cast<std::uint8_t>(usage), all_subresources);
        return input;
    }

    texture render_pass_builder::write(
        const texture output, const texture_write usage, const subresource_range range, const write_flags flags)
    {
        const std::uint32_t version = record_write(
            output, resource_kind::texture, static_cast<std::uint8_t>(usage), range, flags == write_flags::needs_clear);
        return version == no_index ?
                   output :
                   detail::resource_handles::make<texture>(
                       output.index(), graph_.tables_->versions[version].version, graph_.frame_serial_);
    }

    buffer render_pass_builder::write(const buffer output, const buffer_write usage)
    {
        const std::uint32_t version =
            record_write(output, resource_kind::buffer, static_cast<std::uint8_t>(usage), all_subresources);
        return version == no_index ?
                   output :
                   detail::resource_handles::make<buffer>(
                       output.index(), graph_.tables_->versions[version].version, graph_.frame_serial_);
    }

    void render_pass_builder::set_side_effects()
    {
        pass_.side_effects = true;
    }

    void render_pass_builder::set_render_area(const std::uint32_t width, const std::uint32_t height)
    {
        pass_.render_width = width;
        pass_.render_height = height;
    }

    void render_pass_builder::record_read(
        const resource handle, const resource_kind kind, const std::uint8_t usage, const subresource_range range)
    {
        render_graph_tables& tables = *graph_.tables_;
        const std::uint32_t version = find_version(tables, handle, kind, graph_.frame_serial_);
        if (version == no_index)
        {
            return;
        }

        consume(version);
        pass_.accesses.push_back(
            declared_access{ .resource = handle.index(),
                             .input = version,
                             .kind = kind,
                             .direction = access_direction::read,
                             .usage = usage,
                             .range = range });
    }

    std::uint32_t render_pass_builder::record_write(
        const resource handle,
        const resource_kind kind,
        const std::uint8_t usage,
        const subresource_range range,
        const bool needs_clear)
    {
        render_graph_tables& tables = *graph_.tables_;
        const std::uint32_t input = find_version(tables, handle, kind, graph_.frame_serial_);
        if (input == no_index)
        {
            return no_index;
        }
        if (!AVIS_CHECK(
                logger_name,
                tables.versions[input].successor == no_index,
                "render_pass_builder::write: this version was already written; use the handle the earlier write "
                "returned"))
        {
            return no_index;
        }

        // The write overwrites the version passed in, so its producer stays a dependency (a load of the earlier
        // content)
        consume(input);

        const auto output = static_cast<std::uint32_t>(tables.versions.size());
        const std::uint32_t version_number = tables.versions[input].version + 1;
        tables.versions.push_back(
            version_entry{ .resource = handle.index(), .version = version_number, .producer = pass_index_ });
        tables.versions[input].successor = output;
        tables.resources[handle.index()].versions.push_back(output);

        pass_.accesses.push_back(
            declared_access{ .resource = handle.index(),
                             .input = input,
                             .output = output,
                             .kind = kind,
                             .direction = access_direction::write,
                             .usage = usage,
                             .range = range,
                             .needs_clear = needs_clear });
        return output;
    }

    void render_pass_builder::consume(const std::uint32_t version)
    {
        render_graph_tables& tables = *graph_.tables_;
        version_entry& entry = tables.versions[version];
        // A pass using its own output depends on nothing, and doesn't keep itself alive (render_graph_compiler.cpp)
        if (entry.producer == pass_index_)
        {
            return;
        }
        ++entry.readers;
        if (entry.producer != no_index)
        {
            tables.edges.push_back(
                detail::dependency_edge{ .producer = entry.producer, .consumer = pass_index_, .version = version });
        }
    }
} // namespace avis::graphics
