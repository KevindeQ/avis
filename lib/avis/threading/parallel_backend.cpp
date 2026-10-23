#include "avis/threading/parallel_backend.h"

#include "avis/threading/parallel_helpers.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <utility>

namespace avis::threading
{
    namespace
    {
        using receiver_proxy = parallel_backend::receiver_proxy;
        using bulk_item_receiver_proxy = parallel_backend::bulk_item_receiver_proxy;

        // Environment of the operations on the pool: the stop token of the front end's receiver
        struct proxy_environment
        {
            receiver_proxy* proxy = nullptr;

            stdexec::inplace_stop_token query(stdexec::get_stop_token_t) const noexcept
            {
                const auto token = proxy->try_query<stdexec::inplace_stop_token>(stdexec::get_stop_token);
                return token ? *token : stdexec::inplace_stop_token{};
            }
        };

        // Forwards the completion of an operation on the pool to the front end's receiver proxy. It destroys the
        // operation (and so itself) first, because completing the proxy may end the lifetime of the scratch
        // storage the operation lives in.
        struct proxy_receiver
        {
            using receiver_concept = stdexec::receiver_tag;
            using destroy_function = void(void*) noexcept;

            receiver_proxy* proxy = nullptr;
            void* operation = nullptr;
            destroy_function* destroy = nullptr;

            void set_value() noexcept
            {
                receiver_proxy& target = *proxy;
                destroy(operation);
                const detail::parallel_work_scope scope;
                target.set_value();
            }

            void set_error(std::exception_ptr error) noexcept
            {
                receiver_proxy& target = *proxy;
                destroy(operation);
                const detail::parallel_work_scope scope;
                target.set_error(std::move(error));
            }

            void set_stopped() noexcept
            {
                receiver_proxy& target = *proxy;
                destroy(operation);
                const detail::parallel_work_scope scope;
                target.set_stopped();
            }

            proxy_environment get_env() const noexcept
            {
                return proxy_environment{ proxy };
            }
        };

        // A sender connected to a proxy_receiver, built in the front end's scratch storage when it fits and on the
        // heap otherwise. It destroys itself when it completes.
        template<typename Sender>
        class operation
        {
        public:
            static operation* create(std::span<std::byte> scratch, receiver_proxy& proxy, Sender sender)
            {
                void* storage = scratch.data();
                std::size_t space = scratch.size();
                if (storage != nullptr && std::align(alignof(operation), sizeof(operation), storage, space) != nullptr)
                {
                    return ::new (storage) operation(std::move(sender), proxy, &destroy<false>);
                }
                return new operation(std::move(sender), proxy, &destroy<true>);
            }

            operation(const operation&) = delete;
            operation& operator=(const operation&) = delete;

            void start() noexcept
            {
                stdexec::start(inner_);
            }

        private:
            operation(Sender sender, receiver_proxy& proxy, proxy_receiver::destroy_function* destroy_self) :
                inner_{ stdexec::connect(std::move(sender), proxy_receiver{ &proxy, this, destroy_self }) }
            {}

            template<bool on_heap>
            static void destroy(void* self) noexcept
            {
                if constexpr (on_heap)
                {
                    delete static_cast<operation*>(self);
                }
                else
                {
                    std::destroy_at(static_cast<operation*>(self));
                }
            }

            stdexec::connect_result_t<Sender, proxy_receiver> inner_;
        };

        template<typename Sender>
        void start_operation(std::span<std::byte> scratch, receiver_proxy& proxy, Sender sender) noexcept
        {
            try
            {
                operation<Sender>::create(scratch, proxy, std::move(sender))->start();
            }
            catch (...)
            {
                proxy.set_error(std::current_exception());
            }
        }

        // Bulk item functions: each call runs one chunk (or one index) through the front end
        struct chunked_items
        {
            bulk_item_receiver_proxy* proxy = nullptr;
            std::size_t chunk_size = 1;
            std::size_t count = 0;

            void operator()(const std::size_t chunk) const noexcept
            {
                const detail::parallel_work_scope scope;
                const std::size_t begin = chunk * chunk_size;
                proxy->execute(begin, std::min(begin + chunk_size, count));
            }
        };

        struct unchunked_items
        {
            bulk_item_receiver_proxy* proxy = nullptr;

            void operator()(const std::size_t index) const noexcept
            {
                const detail::parallel_work_scope scope;
                proxy->execute(index, index + 1);
            }
        };
    } // namespace

    parallel_backend::parallel_backend(exec::static_thread_pool& pool) :
        scheduler_{ pool.get_scheduler() },
        thread_count_{ pool.available_parallelism() }
    {}

    void parallel_backend::schedule(receiver_proxy& receiver, const std::span<std::byte> scratch) noexcept
    {
        if (detached_.load(std::memory_order_acquire))
        {
            receiver.set_stopped();
            return;
        }

        start_operation(scratch, receiver, stdexec::schedule(scheduler_));
    }

    void parallel_backend::schedule_bulk_chunked(
        const std::size_t count, bulk_item_receiver_proxy& receiver, const std::span<std::byte> scratch) noexcept
    {
        if (detached_.load(std::memory_order_acquire))
        {
            receiver.set_stopped();
            return;
        }
        if (count == 0)
        {
            receiver.set_value();
            return;
        }

        // stdexec's default chunking: about two chunks per worker once there's enough work, else one item each
        const std::size_t chunk_size = thread_count_ > 0 && count > 3 * thread_count_ ? count / thread_count_ / 2 : 1;
        const std::size_t chunk_count = (count + chunk_size - 1) / chunk_size;

        start_operation(
            scratch,
            receiver,
            stdexec::bulk(
                stdexec::schedule(scheduler_),
                stdexec::par,
                chunk_count,
                chunked_items{ .proxy = &receiver, .chunk_size = chunk_size, .count = count }));
    }

    void parallel_backend::schedule_bulk_unchunked(
        const std::size_t count, bulk_item_receiver_proxy& receiver, const std::span<std::byte> scratch) noexcept
    {
        if (detached_.load(std::memory_order_acquire))
        {
            receiver.set_stopped();
            return;
        }
        if (count == 0)
        {
            receiver.set_value();
            return;
        }

        start_operation(
            scratch,
            receiver,
            stdexec::bulk(stdexec::schedule(scheduler_), stdexec::par, count, unchunked_items{ .proxy = &receiver }));
    }

    void parallel_backend::detach() noexcept
    {
        detached_.store(true, std::memory_order_release);
    }
} // namespace avis::threading
