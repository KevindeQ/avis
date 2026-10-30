#ifndef AVIS_GRAPHICS_RESOURCE_H
#define AVIS_GRAPHICS_RESOURCE_H

// Virtual resources of the render graph (docs/design/10-render-graph.md §2, §3.1): typed handles into the graph's
// resource table, valid for one frame. Not RHI resources; the resource_bag maps them during execution (§3.4).

#include "avis/rhi/types.h"

#include <cstdint>
#include <limits>

namespace avis::graphics
{
    // The RHI's ranges, so passes name them without the rhi:: prefix
    using rhi::all_subresources;
    using rhi::subresource_range;

    // The add function a pass came from. Fixes its context type and, with the queue it lands on, the access states of
    // its usages (usage_access_state in texture.h and buffer.h).
    enum class pass_type : std::uint8_t
    {
        graphics,
        compute,
        copy
    };

    namespace detail
    {
        struct resource_handles;
    } // namespace detail

    // Index into the graph's resource table plus a version, bumped by every write (§5). A default handle is invalid.
    // Debug builds also carry the serial of the frame that made the handle, so the graph catches handles kept from an
    // earlier frame.
    class resource
    {
    public:
        static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

        constexpr resource() noexcept = default;

        constexpr std::uint32_t index() const noexcept
        {
            return index_;
        }

        constexpr std::uint32_t version() const noexcept
        {
            return version_;
        }

        constexpr bool valid() const noexcept
        {
            return index_ != invalid_index;
        }

#ifndef NDEBUG
        // The graph's frame serial when the handle was made (debug builds only)
        constexpr std::uint32_t frame_serial() const noexcept
        {
            return frame_serial_;
        }
#endif

        friend constexpr bool operator==(const resource&, const resource&) noexcept = default;

    protected:
        constexpr resource(
            const std::uint32_t index,
            const std::uint32_t version,
            [[maybe_unused]] const std::uint32_t frame_serial) noexcept :
            index_{ index },
            version_{ version }
#ifndef NDEBUG
            ,
            frame_serial_{ frame_serial }
#endif
        {}

    private:
        friend struct detail::resource_handles;

        std::uint32_t index_ = invalid_index;
        std::uint32_t version_ = 0;
#ifndef NDEBUG
        std::uint32_t frame_serial_ = 0;
#endif
    };

    class texture : public resource
    {
    public:
        constexpr texture() noexcept = default;

    private:
        friend struct detail::resource_handles;
        using resource::resource;
    };

    class buffer : public resource
    {
    public:
        constexpr buffer() noexcept = default;

    private:
        friend struct detail::resource_handles;
        using resource::resource;
    };

    namespace detail
    {
        // Makes handles: the graph and its builder, and tests of the compiler on hand-built tables. The frame serial
        // is dropped in release builds.
        struct resource_handles
        {
            template<typename Handle>
            static constexpr Handle make(
                const std::uint32_t index, const std::uint32_t version, const std::uint32_t frame_serial = 0) noexcept
            {
                return Handle{ index, version, frame_serial };
            }
        };
    } // namespace detail
} // namespace avis::graphics

#endif
