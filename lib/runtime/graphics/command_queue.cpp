#include "avis/runtime/graphics/command_queue.h"

namespace graphics
{
    command_queue::fence_event_pool::fence_event_pool() : free_list_{std::uint8_t(-1)}
    {
        for (auto& EventHandle : event_handles_)
        {
            ::CreateEventW(NULL, FALSE, FALSE, NULL);
        }
    }

    command_queue::fence_event_pool::~fence_event_pool()
    {
        for (auto& event_handle : event_handles_)
        {
            ::CloseHandle(event_handle);
        }
    }

    HANDLE command_queue::fence_event_pool::acquire_handle()
    {
        std::lock_guard<std::mutex> lock(event_handle_access_);

        auto event_handle_id = 0UL;
        const auto contains_free_handle = _BitScanForward(&event_handle_id, free_list_);
        Verify(contains_free_handle == TRUE, "Could not acquire a event handle.");

        free_list_ &= ~(1UL << event_handle_id);

        return event_handles_[event_handle_id];
    }

    void command_queue::fence_event_pool::release_handle(HANDLE handle)
    {
        std::lock_guard<std::mutex> lock(event_handle_access_);

        auto result = std::find(event_handles_.begin(), event_handles_.end(), handle);
        Verify(result != event_handles_.end(), "event_handle is tracked by another fence_event_pool.");

        auto event_handle_id = std::distance(event_handles_.begin(), result);
        free_list_ |= 1UL << event_handle_id;
    }

    command_queue::command_queue(D3D12_COMMAND_LIST_TYPE type, ID3D12Device* const device) :
        command_queue_{nullptr},

        fence_{nullptr},
        next_fence_value_{std::uint64_t(type) << 60 | 1ULL},
        completed_fence_value_{std::uint64_t(type) << 60},

        command_list_type_{type}
    {
        D3D12_COMMAND_QUEUE_DESC Descriptor = {};
        Descriptor.Type = command_list_type_;
        Descriptor.NodeMask = 0;
        Descriptor.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        Descriptor.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        throw_if_failed(device->CreateCommandQueue(&Descriptor, IID_PPV_ARGS(&command_queue_)));

        throw_if_failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    }

    command_queue::~command_queue()
    {
        safe_release(fence_);
        safe_release(command_queue_);
    }

    std::uint64_t command_queue::execute_command_list(ID3D12CommandList* const command_list)
    {
        Verify(command_list != nullptr, "Invalid command list.");

        throw_if_failed(static_cast<ID3D12GraphicsCommandList*>(command_list)->Close());

        auto FenceValue = next_fence_value_.fetch_add(1ULL);
        command_queue_->ExecuteCommandLists(1U, &command_list);
        command_queue_->Signal(fence_, FenceValue);

        return FenceValue;
    }

    void command_queue::wait_for_fence(std::uint64_t FenceValue)
    {
        if (is_fence_completed(FenceValue))
        {
            return;
        }

        auto FenceEventHandle = fence_event_pool_.acquire_handle();
        fence_->SetEventOnCompletion(FenceValue, FenceEventHandle);
        ::WaitForSingleObject(FenceEventHandle, INFINITE);
        fence_event_pool_.release_handle(FenceEventHandle);

        auto CompletedFenceValue = completed_fence_value_.load();
        while (!completed_fence_value_.compare_exchange_weak(
            CompletedFenceValue, std::max(CompletedFenceValue, FenceValue)))
        {
        }
    }

    std::uint64_t command_queue::get_completed_fence_value()
    {
        auto completed_fence_value = completed_fence_value_.load();
        while (!completed_fence_value_.compare_exchange_weak(
            completed_fence_value, std::max(completed_fence_value, fence_->GetCompletedValue())))
        {
        }

        return completed_fence_value;
    }

    std::uint64_t command_queue::get_next_fence_value() const
    {
        return next_fence_value_;
    }

    bool command_queue::is_fence_completed(std::uint64_t fence_value)
    {
        if (fence_value > completed_fence_value_)
        {
            auto completed_fence_value = completed_fence_value_.load();
            while (!completed_fence_value_.compare_exchange_weak(
                completed_fence_value, std::max(completed_fence_value, fence_->GetCompletedValue())))
            {
            }
        }

        return fence_value <= completed_fence_value_;
    }
} // namespace graphics
