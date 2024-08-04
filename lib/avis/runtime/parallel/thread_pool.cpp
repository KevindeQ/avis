#include "avis/runtime/parallel/thread_pool.h"

#include "avis/core/common.h"

namespace parallel
{
    thread_pool::thread_pool(std::size_t thread_count) :
        threads_{},
        callables_{},
        callables_mutex_{},
        callables_cv_{},
        stop_threads_{false}
    {
        for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index)
        {
            threads_.push_back(std::move(std::thread{&thread_pool::thread_main, this}));
        }
    }

    thread_pool::~thread_pool()
    {
        stop_threads_ = true;
        callables_cv_.notify_all();

        for (auto& thread : threads_)
        {
            thread.join();
        }
    }

    void thread_pool::thread_main()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(callables_mutex_);
            callables_cv_.wait(lock, [this]() -> bool { return stop_threads_ || !callables_.empty(); });

            if (stop_threads_ && callables_.empty())
            {
                lock.unlock();
                return;
            }

            auto callable = std::move(callables_.front());
            callables_.pop();

            lock.unlock();

            (*callable)();
        }
    }
} // namespace parallel
