#ifndef AVIS_RHI_COMMAND_LIST_H
#define AVIS_RHI_COMMAND_LIST_H

// Command lists and recorders (docs/design/09-rhi.md §7, D-25, D-92). A command_recorder<Queue> records the operations
// of queue type Queue into an open list; a command_list<Queue> owns that list and is what queue<Queue>::submit takes.
// Free of D3D12 and <windows.h>.

#include "avis/error.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace avis::rhi
{
    class render_hardware_interface;
    class timestamp_query_set;
    template<queue_type>
    class queue;

    namespace d3d12
    {
        class device_state;
        struct command_list_record;
        struct recorder_access; // native_access.h: the record behind a recorder, for RHI tests
    } // namespace d3d12

    enum class index_type : std::uint8_t
    {
        uint16,
        uint32
    };

    // D3D12_DRAW_ARGUMENTS, D3D12_DRAW_INDEXED_ARGUMENTS and D3D12_DISPATCH_ARGUMENTS, for indirect argument buffers
    // written by shaders
    struct draw_arguments
    {
        std::uint32_t vertex_count;
        std::uint32_t instance_count;
        std::uint32_t first_vertex;
        std::uint32_t first_instance;
    };

    struct draw_indexed_arguments
    {
        std::uint32_t index_count;
        std::uint32_t instance_count;
        std::uint32_t first_index;
        std::int32_t vertex_offset;
        std::uint32_t first_instance;
    };

    struct dispatch_arguments
    {
        std::uint32_t group_count_x;
        std::uint32_t group_count_y;
        std::uint32_t group_count_z;
    };

    template<queue_type Queue>
    class command_list;

    // Records the operations of queue type Queue into an open command list. Non-owning and copyable; valid until the
    // owning command_list is submitted or destroyed (a stale recorder is caught by the generation check). Recording
    // calls return void: misuse is asserted in debug builds and ignored with an error log in release builds (§7).
    template<queue_type Queue>
    class command_recorder
    {
    public:
        // ---- every queue type ----
        void copy_buffer(
            buffer_handle destination,
            std::uint64_t destination_offset,
            buffer_handle source,
            std::uint64_t source_offset,
            std::uint64_t size);
        void copy_texture(
            texture_handle destination,
            subresource destination_subresource,
            texture_handle source,
            subresource source_subresource);
        // The footprint is computed (copyable_footprint); the buffer offset must be 512-byte aligned
        void copy_buffer_to_texture(
            texture_handle destination,
            subresource destination_subresource,
            buffer_handle source,
            std::uint64_t source_offset);
        void copy_texture_to_buffer(
            buffer_handle destination,
            std::uint64_t destination_offset,
            texture_handle source,
            subresource source_subresource);
        // One Barrier() call with up to three groups
        void barrier(
            std::span<const global_barrier> global_barriers,
            std::span<const buffer_barrier> buffer_barriers,
            std::span<const texture_barrier> texture_barriers);
        void begin_event(std::string_view name); // PIX-compatible
        void end_event();
        // set.queue() must be submission_queue() (the owning list's queue for a narrowed recorder)
        void write_timestamp(timestamp_query_set& set, std::uint32_t index);

        // ---- compute and graphics ----
        void set_pipeline(compute_pipeline_handle pipeline)
            requires(Queue != queue_type::copy);
        // Root arguments of the bound pipeline's root signature (§12.1); undefined after a signature switch
        void set_root_constants(std::uint32_t parameter, std::span<const std::uint32_t> values, std::uint32_t offset = 0)
            requires(Queue != queue_type::copy);
        template<class Constants>
            requires(Queue != queue_type::copy && std::is_trivially_copyable_v<Constants> && sizeof(Constants) % 4 == 0)
        void set_root_constants(std::uint32_t parameter, const Constants& constants)
        {
            set_root_constants(
                parameter,
                std::span<const std::uint32_t>{ reinterpret_cast<const std::uint32_t*>(&constants), sizeof(Constants) / 4 });
        }
        void set_root_constant_buffer(std::uint32_t parameter, gpu_address address)
            requires(Queue != queue_type::copy);
        void set_root_shader_resource(std::uint32_t parameter, gpu_address address)
            requires(Queue != queue_type::copy);
        void set_root_unordered_access(std::uint32_t parameter, gpu_address address)
            requires(Queue != queue_type::copy);
        void dispatch(std::uint32_t group_count_x, std::uint32_t group_count_y, std::uint32_t group_count_z)
            requires(Queue != queue_type::copy);
        void dispatch_indirect(buffer_handle arguments, std::uint64_t offset)
            requires(Queue != queue_type::copy);
        void clear_unordered_access_float(texture_handle texture, std::uint32_t mip, std::span<const float, 4> values)
            requires(Queue != queue_type::copy);
        void clear_unordered_access_uint(texture_handle texture, std::uint32_t mip, std::span<const std::uint32_t, 4> values)
            requires(Queue != queue_type::copy);
        // Placed (transient) textures: `view` is the UAV index a descriptor_range returned for that texture and mip
        void clear_unordered_access_float(texture_handle texture, bindless_index view, std::span<const float, 4> values)
            requires(Queue != queue_type::copy);
        void clear_unordered_access_uint(texture_handle texture, bindless_index view, std::span<const std::uint32_t, 4> values)
            requires(Queue != queue_type::copy);

        // ---- graphics only ----
        void set_pipeline(graphics_pipeline_handle pipeline)
            requires(Queue == queue_type::graphics);
        void set_render_targets(std::span<const texture_view> colors, std::optional<texture_view> depth = {})
            requires(Queue == queue_type::graphics);
        void set_viewport(const viewport& value)
            requires(Queue == queue_type::graphics);
        void set_scissor(const rectangle& value)
            requires(Queue == queue_type::graphics);
        void set_index_buffer(buffer_handle buffer, index_type type, std::uint64_t offset = 0)
            requires(Queue == queue_type::graphics);
        void draw(
            std::uint32_t vertex_count,
            std::uint32_t instance_count = 1,
            std::uint32_t first_vertex = 0,
            std::uint32_t first_instance = 0)
            requires(Queue == queue_type::graphics);
        void draw_indexed(
            std::uint32_t index_count,
            std::uint32_t instance_count = 1,
            std::uint32_t first_index = 0,
            std::int32_t vertex_offset = 0,
            std::uint32_t first_instance = 0)
            requires(Queue == queue_type::graphics);
        void draw_indirect(
            buffer_handle arguments,
            std::uint64_t offset,
            std::uint32_t maximum_count,
            std::optional<buffer_handle> count = {},
            std::uint64_t count_offset = 0)
            requires(Queue == queue_type::graphics);
        void draw_indexed_indirect(
            buffer_handle arguments,
            std::uint64_t offset,
            std::uint32_t maximum_count,
            std::optional<buffer_handle> count = {},
            std::uint64_t count_offset = 0)
            requires(Queue == queue_type::graphics);
        void clear_render_target(const texture_view& view, std::span<const float, 4> color)
            requires(Queue == queue_type::graphics);
        void clear_depth(const texture_view& view, float depth)
            requires(Queue == queue_type::graphics);

        // The queue the native list executes on: Queue for a list's own recorder, the owning list's queue for a narrowed
        // one. Timestamps follow this queue.
        queue_type submission_queue() const noexcept;

    protected:
        command_recorder() noexcept = default;
        command_recorder(d3d12::command_list_record* record, std::uint32_t generation) noexcept :
            record_{ record },
            generation_{ generation }
        {}

        template<queue_type>
        friend class command_list;
        friend struct d3d12::recorder_access;

        d3d12::command_list_record* record_ = nullptr;
        std::uint32_t generation_ = 0;
    };

    // Owns an open command list for queue type Queue: what command_lists::acquire returns and queue<Queue>::submit
    // takes. Move-only. Records like a command_recorder<Queue> (public base).
    template<queue_type Queue>
    class command_list : public command_recorder<Queue>
    {
    public:
        command_list(command_list&& other) noexcept;
        command_list& operator=(command_list&& other) noexcept;
        command_list(const command_list&) = delete;
        command_list& operator=(const command_list&) = delete;
        // An unsubmitted list is discarded: closed, and its allocator reused at once
        ~command_list();

        // A recorder of a smaller operation set, recording into this list (§7, A-39). Capabilities nest
        // (graphics ⊃ compute ⊃ copy).
        template<queue_type Narrower>
            requires((Queue == queue_type::graphics && Narrower != queue_type::graphics) ||
                     (Queue == queue_type::compute && Narrower == queue_type::copy))
        command_recorder<Narrower> narrow() const noexcept
        {
            return command_recorder<Narrower>{ this->record_, this->generation_ };
        }

    private:
        friend class command_lists;
        template<queue_type>
        friend class queue;

        command_list(d3d12::command_list_record* record, std::uint32_t generation) noexcept;
        void discard() noexcept;
    };

    using graphics_command_list = command_list<queue_type::graphics>;
    using compute_command_list = command_list<queue_type::compute>;
    using copy_command_list = command_list<queue_type::copy>;

    using graphics_recorder = command_recorder<queue_type::graphics>;
    using compute_recorder = command_recorder<queue_type::compute>;
    using copy_recorder = command_recorder<queue_type::copy>;

    // A view (§4.3): non-owning, copyable, built from the RHI and never outliving it
    class command_lists
    {
    public:
        explicit command_lists(render_hardware_interface& hardware_interface) noexcept;

        // Any thread, lock-free after a thread's first call (D-64). Fails with device_lost after device removal, or
        // out_of_memory (D-86).
        template<queue_type Queue>
        result<command_list<Queue>> acquire(std::string_view debug_name = {});

    private:
        d3d12::device_state* state_;
    };

    extern template class command_recorder<queue_type::graphics>;
    extern template class command_recorder<queue_type::compute>;
    extern template class command_recorder<queue_type::copy>;
    extern template class command_list<queue_type::graphics>;
    extern template class command_list<queue_type::compute>;
    extern template class command_list<queue_type::copy>;
    extern template result<command_list<queue_type::graphics>> command_lists::acquire(std::string_view);
    extern template result<command_list<queue_type::compute>> command_lists::acquire(std::string_view);
    extern template result<command_list<queue_type::copy>> command_lists::acquire(std::string_view);
} // namespace avis::rhi

#endif
