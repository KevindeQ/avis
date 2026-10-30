#ifndef AVIS_GRAPHICS_GPU_COMPLETION_THREAD_H
#define AVIS_GRAPHICS_GPU_COMPLETION_THREAD_H

// Drives an RHI's completions from one long-running thread of the Avis thread pool (docs/design/09-rhi.md §5.2), for
// headless applications, tools and the Driver. Applications with an event loop drive the RHI from it instead. Free of
// <windows.h> and D3D12.

#include "avis/error.h"
#include "avis/rhi/completion_driver.h"
#include "avis/threading/thread_handle.h"
#include "avis/threading/thread_pool.h"

#include <memory>
#include <string_view>

namespace avis::graphics
{
    class gpu_completion_thread
    {
    public:
        // The name of the thread it starts
        static constexpr std::string_view thread_name = "gpu-completions";

        // Starts one long-running body on `pool` that waits on the driver's completion_event() and a private stop
        // event, and calls poll_completions() whenever the completion event fires. Fails with the pool's errors
        // (pool_exhausted, pool_stopping), or the system error when the stop event can't be created.
        static result<std::unique_ptr<gpu_completion_thread>>
            create(threading::thread_pool& pool, rhi::completion_driver driver);

        // Signals the stop event and joins the thread; GPU waits still pending then stay pending. Destroy before the
        // RHI. The pool stopping first ends the body too.
        ~gpu_completion_thread();

        gpu_completion_thread(const gpu_completion_thread&) = delete;
        gpu_completion_thread& operator=(const gpu_completion_thread&) = delete;

    private:
        gpu_completion_thread(void* stop_event, threading::thread_handle thread) noexcept;

        void* stop_event_; // manual-reset Win32 event (a HANDLE), owned
        threading::thread_handle thread_;
    };
} // namespace avis::graphics

#endif
