#ifndef AVIS_IO_DETAIL_OPERATION_REGISTRY_H
#define AVIS_IO_DETAIL_OPERATION_REGISTRY_H

// Private: the operation registry service behind include/avis/io/detail/registered_operation.h

#include "avis/io/detail/registered_operation.h"

#include <asio/execution_context.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace avis::io::detail
{
    // Intrusive list of the live operations of one asio context, so the IO context can cancel them at shutdown and
    // wait for them to complete. An asio service, so anything holding an executor of the context can find it.
    class operation_registry : public asio::execution_context::service
    {
    public:
        static asio::execution_context::id id;

        explicit operation_registry(asio::execution_context& context);

        bool add(registered_operation& operation);
        void remove(registered_operation& operation);

        // Closes the registry (add() fails from now on) and calls cancel() on every operation, under the lock
        void close_and_cancel_all() noexcept;

        // Waits until no operation is registered; false on timeout
        bool wait_empty(std::chrono::milliseconds timeout);

        std::size_t size() const;

    private:
        void shutdown() override {}

        mutable std::mutex mutex_;
        std::condition_variable empty_;
        registered_operation* head_ = nullptr;
        std::size_t count_ = 0;
        bool closed_ = false;
    };
} // namespace avis::io::detail

#endif
