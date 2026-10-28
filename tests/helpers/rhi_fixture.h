#ifndef AVIS_TESTS_HELPERS_RHI_FIXTURE_H
#define AVIS_TESTS_HELPERS_RHI_FIXTURE_H

// The RHI for device tests (docs/tasks/06-rhi.md §6.3.4): software adapters allowed, debug layer on, and a capture
// on the "rhi" logger that fails the test on any error-level message (a D3D12 validation error included).
// AVIS_TEST_ADAPTER=warp selects WARP; unset, the default order applies (hardware first).

#include "log_capture.h"

#include "avis/rhi/configuration.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/render_hardware_interface.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace avis::tests
{
    // The configuration make_test_rhi uses: overrides plus allow_software_adapter, the debug layer, and the WARP
    // LUID when AVIS_TEST_ADAPTER=warp and no adapter is set
    rhi::render_hardware_interface_configuration
        test_rhi_configuration(rhi::render_hardware_interface_configuration overrides = {});

    // WARP's LUID, read from adapter_selector::enumerate()
    std::optional<rhi::adapter_luid> warp_luid();

    // Owns an RHI and the "rhi" log capture around it. The capture is attached before the RHI is created and outlives
    // it, so messages from creation and destruction count. Not movable.
    class test_rhi
    {
    public:
        explicit test_rhi(rhi::render_hardware_interface_configuration overrides = {});
        ~test_rhi();

        test_rhi(const test_rhi&) = delete;
        test_rhi& operator=(const test_rhi&) = delete;

        rhi::render_hardware_interface& get() noexcept
        {
            return *rhi_;
        }

        rhi::render_hardware_interface* operator->() noexcept
        {
            return rhi_.get();
        }

        rhi::render_hardware_interface& operator*() noexcept
        {
            return *rhi_;
        }

        // Deterministic GPU waits (docs/tasks/06-rhi.md, Conventions): waits on the point's queue fence with a private
        // event, then calls completion_driver::poll_completions() and returns what it completed. Fails the test when
        // the point isn't reached within 10 seconds.
        std::size_t wait_for(rhi::synchronization_point point);

        // The captured "[error] ..." and "[critical] ..." lines
        std::vector<std::string> error_lines() const;

        // For tests that provoke errors on purpose and check them themselves: the destructor no longer fails on them
        void accept_errors() noexcept
        {
            errors_accepted_ = true;
        }

        const log_capture& log() const noexcept
        {
            return capture_;
        }

    private:
        log_capture capture_{ "rhi" };
        bool errors_accepted_ = false;
        std::unique_ptr<rhi::render_hardware_interface> rhi_;
    };

    // Same as test_rhi{ overrides }; returned by guaranteed copy elision
    inline test_rhi make_test_rhi(rhi::render_hardware_interface_configuration overrides = {})
    {
        return test_rhi{ std::move(overrides) };
    }
} // namespace avis::tests

#endif
