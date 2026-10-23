#include "avis/threading/parallel_helpers.h"

namespace avis::threading::detail
{
    namespace
    {
        // Depth of parallel work on the calling thread
        thread_local int parallel_work_depth = 0;

        // Depth of forbid_blocking_waits scopes on the calling thread (IO bodies)
        thread_local int forbidden_wait_depth = 0;
    } // namespace

    parallel_work_scope::parallel_work_scope() noexcept
    {
        ++parallel_work_depth;
    }

    parallel_work_scope::~parallel_work_scope()
    {
        --parallel_work_depth;
    }

    bool in_parallel_work() noexcept
    {
        return parallel_work_depth > 0;
    }

    forbid_blocking_waits::forbid_blocking_waits() noexcept
    {
        ++forbidden_wait_depth;
    }

    forbid_blocking_waits::~forbid_blocking_waits()
    {
        --forbidden_wait_depth;
    }

    bool blocking_wait_forbidden() noexcept
    {
        return parallel_work_depth > 0 || forbidden_wait_depth > 0;
    }
} // namespace avis::threading::detail
