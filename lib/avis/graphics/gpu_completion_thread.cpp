#include "avis/graphics/gpu_completion_thread.h"

#include "avis/log.h"

#include <array>
#include <exception>
#include <stop_token>
#include <system_error>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";

        // Waits until the stop event is set, polling the RHI each time its completion event fires
        void drive(const rhi::completion_driver driver, const HANDLE stop_event, const std::stop_token& stop) noexcept
        {
            // The pool's stop request (or the handle's) ends the body like the destructor does
            const std::stop_callback on_stop{ stop, [stop_event] { SetEvent(stop_event); } };

            // The stop event first: when both are signalled, stopping wins
            const std::array<HANDLE, 2> events{ stop_event, static_cast<HANDLE>(driver.completion_event()) };
            while (true)
            {
                switch (WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE))
                {
                case WAIT_OBJECT_0:
                    return;
                case WAIT_OBJECT_0 + 1:
                    static_cast<void>(rhi::completion_driver{ driver }.poll_completions());
                    break;
                default:
                    // Nothing can wake the thread reliably any more; GPU waits stall until another driver polls
                    log::get(logger_name)
                        ->error(
                            "gpu_completion_thread: WaitForMultipleObjects failed ({}); no longer driving completions",
                            std::system_category().message(static_cast<int>(GetLastError())));
                    return;
                }
            }
        }
    } // namespace

    result<std::unique_ptr<gpu_completion_thread>>
        gpu_completion_thread::create(threading::thread_pool& pool, const rhi::completion_driver driver)
    {
        const HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop_event == nullptr)
        {
            return std::unexpected(
                avis::make_error(
                    std::make_exception_ptr(
                        std::system_error{ static_cast<int>(GetLastError()),
                                           std::system_category(),
                                           "gpu_completion_thread: CreateEvent" })));
        }

        auto thread = pool.start(
            thread_name, [driver, stop_event](const std::stop_token stop) { drive(driver, stop_event, stop); });
        if (!thread)
        {
            CloseHandle(stop_event);
            return std::unexpected(std::move(thread.error()));
        }

        try
        {
            return std::unique_ptr<gpu_completion_thread>{ new gpu_completion_thread{ stop_event,
                                                                                      std::move(*thread) } };
        }
        catch (...)
        {
            // Stop and join the body before closing the event it waits on
            SetEvent(stop_event);
            thread->request_stop();
            thread->join();
            CloseHandle(stop_event);
            return std::unexpected(avis::make_error(std::current_exception()));
        }
    }

    gpu_completion_thread::gpu_completion_thread(void* const stop_event, threading::thread_handle thread) noexcept :
        stop_event_{ stop_event },
        thread_{ std::move(thread) }
    {}

    gpu_completion_thread::~gpu_completion_thread()
    {
        SetEvent(static_cast<HANDLE>(stop_event_));
        thread_.request_stop();
        try
        {
            thread_.join();
        }
        catch (...)
        {
            // The body may still wait on the event, so it's leaked rather than closed
            log::get(logger_name)->error("gpu_completion_thread: joining the thread failed");
            return;
        }
        CloseHandle(static_cast<HANDLE>(stop_event_));
    }
} // namespace avis::graphics
