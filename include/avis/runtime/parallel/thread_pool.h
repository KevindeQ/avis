#ifndef RUNTIME_PARALLEL_THREAD_POOL_H
#define RUNTIME_PARALLEL_THREAD_POOL_H

#include "avis/core/common.h"

namespace parallel
{
    class thread_pool
    {
    private:
        class basic_callable_container
        {
        public:
            virtual ~basic_callable_container() = default;

            virtual void operator()() = 0;
        };

        template<typename callable_t>
        class callable_container : public basic_callable_container
        {
        public:
            callable_container(callable_t&& callable) : callable_{std::forward<callable_t>(callable)} {}

            void operator()() override
            {
                callable_();
            }

        private:
            callable_t callable_;
        };

    public:
        ENGINE_API explicit thread_pool(std::size_t thread_count = std::thread::hardware_concurrency());
        thread_pool(const thread_pool&) = delete;
        thread_pool(thread_pool&&) = default;
        ENGINE_API ~thread_pool();

        thread_pool& operator=(const thread_pool&) = delete;
        thread_pool& operator=(thread_pool&&) = default;

        template<typename callable_t, typename... arguments_t>
        std::enable_if_t<std::is_same_v<std::invoke_result_t<callable_t, arguments_t...>, void>>
            execute(callable_t callable, arguments_t&&... arguments);

        template<typename callable_t, typename... arguments_t>
        std::enable_if_t<std::is_same_v<
            std::invoke_result_t<callable_t, arguments_t...>,
            std::future<std::invoke_result_t<callable_t, arguments_t...>>>>
            execute(callable_t callable, arguments_t&&... arguments);

    private:
        void thread_main();

        template<typename callable_t>
        void enqueue_callable(callable_t&& callable);

    private:
        std::vector<std::thread> threads_;
        std::queue<std::unique_ptr<basic_callable_container>> callables_;
        std::mutex callables_mutex_;
        std::condition_variable callables_cv_;
        bool stop_threads_;
    };

    template<typename callable_t, typename... arguments_t>
    std::enable_if_t<std::is_same_v<std::invoke_result_t<callable_t, arguments_t...>, void>>
        thread_pool::execute(callable_t callable, arguments_t&&... arguments)
    {
        auto callable_wrapper = [lambda_callable = std::move(callable),
                                 lambda_arguments = std::make_tuple(std::forward<arguments>(arguments)...)]() mutable
        { std::apply(std::move(lambda_callable), std::move(lambda_arguments)); };

        enqueue_callable(std::move(callable_wrapper));
    }

    template<typename callable_t, typename... arguments_t>
    std::enable_if_t<std::is_same_v<
        std::invoke_result_t<callable_t, arguments_t...>,
        std::future<std::invoke_result_t<callable_t, arguments_t...>>>>
        thread_pool::execute(callable_t callable, arguments_t&&... arguments)
    {
        std::packaged_task<std::invoke_result_t<callable_t, arguments_t>> callable_wrapper{
            [lambda_callable = std::move(callable),
             lambda_arguments = std::make_tuple(std::forward<arguments>(arguments)...)]() mutable
            { std::apply(std::move(lambda_callable), std::move(lambda_arguments)); }};
        std::future<std::invoke_result_t<callable_t, arguments_t>> callable_result = callable_wrapper.get_future();

        enqueue_callable(std::move(callable_wrapper));

        return callable_result;
    }

    template<typename callable_t>
    inline void thread_pool::enqueue_callable(callable_t&& callable)
    {
        std::unique_ptr<basic_callable_container> new_callable =
            std::make_unique<callable_container<callable_t>>(std::forward<callable_t>(callable));
        {
            std::unique_lock<std::mutex> lock{callables_mutex_};
            callables_.emplace(std::move(new_callable));
        }

        /*{
          std::unique_lock<std::mutex> lock{ callables_mutex_ };
          callables_.emplace(std::forward<callable_t>(callable));
        }*/
        callables_cv_.notify_one();
    }
} // namespace parallel

#endif
