#ifndef RUNTIME_GRAPHICS_COMMAND_QUEUE_H
#define RUNTIME_GRAPHICS_COMMAND_QUEUE_H

#include "avis/core/common.h"

namespace graphics
{
    class command_queue
    {
    private:
        class fence_event_pool
        {
        public:
            fence_event_pool();
            ~fence_event_pool();

            HANDLE acquire_handle();
            void release_handle(HANDLE handle);

        private:
            std::array<HANDLE, 8> event_handles_;
            std::uint8_t free_list_;
            std::mutex event_handle_access_;
        };

    public:
        command_queue(D3D12_COMMAND_LIST_TYPE type, ID3D12Device* const device);
        ~command_queue();

        std::uint64_t execute_command_list(ID3D12CommandList* command_list);

        void wait_for_fence(std::uint64_t fence_Value);

        std::uint64_t get_completed_fence_value();
        std::uint64_t get_next_fence_value() const;

        bool is_fence_completed(std::uint64_t fence_value);

    private:
        ID3D12CommandQueue* command_queue_;

        // The command list type is encoded into the fence
        // values in the 4 most significant bits.
        ID3D12Fence* fence_;
        std::atomic<std::uint64_t> next_fence_value_;
        std::atomic<std::uint64_t> completed_fence_value_;
        fence_event_pool fence_event_pool_;

        D3D12_COMMAND_LIST_TYPE command_list_type_;
    };
} // namespace graphics

#endif
