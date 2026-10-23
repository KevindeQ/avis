#ifndef AVIS_RUNTIME_H
#define AVIS_RUNTIME_H

#include "avis/runtime_configuration.h"

#include <atomic>
#include <memory>
#include <string_view>

namespace avis::io
{
    class io_context;
} // namespace avis::io

namespace avis
{
    // Initialises and shuts down the Avis library's process-wide services and is their static access point:
    // logging, the default thread pool, the default IO context and the parallel pool behind
    // stdexec::get_parallel_scheduler(). Exactly one may be alive at a time; sequential lifetimes are allowed.
    class runtime
    {
    public:
        // Throws avis::runtime_error on failure (unrecoverable), after rolling back what was started
        explicit runtime(const runtime_configuration& configuration);
        ~runtime(); // ordered shutdown
        runtime(const runtime&) = delete;
        runtime& operator=(const runtime&) = delete;
        runtime(runtime&&) = delete;
        runtime& operator=(runtime&&) = delete;

        // Static access to the default services. With no runtime alive they assert in debug builds and throw
        // std::logic_error otherwise. The parallel scheduler is reached through stdexec::get_parallel_scheduler().
        static threading::thread_pool& thread_pool();
        static io::io_context& io_context();

        static bool alive() noexcept;
        static const runtime_configuration& configuration(); // the resolved configuration

    private:
        struct state;

        // The live runtime for the static accessor named `accessor`. With no runtime alive it asserts in debug builds
        // (unless detail::make_runtime_accessors_throw is enabled) and throws std::logic_error.
        static runtime* live_instance(std::string_view accessor);

        void shut_down() noexcept; // ordered shutdown, also rolls back a failed construction

        // Runs one shutdown step; a failure is logged and the shutdown goes on. Defined in runtime.cpp, the only place
        // it's used.
        template<typename Step>
        static void shutdown_step(std::string_view name, Step&& step) noexcept
        {
            try
            {
                std::forward<Step>(step)();
            }
            catch (...)
            {
                try
                {
                    log::get("core")->error(
                        "runtime: shutdown step '{}' failed: {}", name, make_error(std::current_exception()));
                }
                catch (...)
                {
                    // Logging is best effort
                }
            }
        }

        // The published runtime: set at the end of construction, cleared after the parallel pool and IO stopped
        static std::atomic<runtime*> instance_;

        // Claimed for a runtime's whole lifetime, construction and destruction included
        static std::atomic<bool> constructing_;

        std::unique_ptr<state> state_;
    };

    namespace detail
    {
        // Test hook: while enabled, the static accessors throw std::logic_error in debug builds too, instead of
        // asserting, so the "no runtime alive" cases are testable in Debug
        void make_runtime_accessors_throw(bool enabled) noexcept;
    } // namespace detail
} // namespace avis

#endif
