#ifndef AVIS_TESTS_HELPERS_RECORDED_ASSERTIONS_H
#define AVIS_TESTS_HELPERS_RECORDED_ASSERTIONS_H

// Installs an assertion handler that records AVIS_CHECK failures instead of aborting, for tests that check a debug
// assertion fires (docs/tasks/06-rhi.md, Conventions). Release builds call no handler, so tests check the recorded
// messages under #ifndef NDEBUG. A failed check also logs (critical in Debug, error in Release): tests on the RHI
// fixture call accept_errors().

#include "avis/detail/check.h"

#include <cstddef>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace avis::tests
{
    class recorded_assertions
    {
    public:
        recorded_assertions() : previous_{ avis::detail::set_assertion_handler(&record) }
        {
            const std::lock_guard lock{ mutex() };
            messages().clear();
        }

        ~recorded_assertions()
        {
            avis::detail::set_assertion_handler(previous_);
            const std::lock_guard lock{ mutex() };
            messages().clear();
        }

        recorded_assertions(const recorded_assertions&) = delete;
        recorded_assertions& operator=(const recorded_assertions&) = delete;

        std::size_t count() const
        {
            const std::lock_guard lock{ mutex() };
            return messages().size();
        }

        // True when a recorded message contains `text`
        bool contains(const std::string_view text) const
        {
            const std::lock_guard lock{ mutex() };
            for (const std::string& message : messages())
            {
                if (message.contains(text))
                {
                    return true;
                }
            }
            return false;
        }

        void clear()
        {
            const std::lock_guard lock{ mutex() };
            messages().clear();
        }

    private:
        static std::mutex& mutex() noexcept
        {
            static std::mutex instance;
            return instance;
        }

        static std::vector<std::string>& messages() noexcept
        {
            static std::vector<std::string> instance;
            return instance;
        }

        static void record(const std::string_view message, std::source_location) noexcept
        {
            try
            {
                const std::lock_guard lock{ mutex() };
                messages().emplace_back(message);
            }
            catch (...)
            {}
        }

        avis::detail::assertion_handler previous_;
    };
} // namespace avis::tests

#endif
