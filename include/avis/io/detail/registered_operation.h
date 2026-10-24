#ifndef AVIS_IO_DETAIL_REGISTERED_OPERATION_H
#define AVIS_IO_DETAIL_REGISTERED_OPERATION_H

// The part of the IO context's operation registry that IO operations use to register for cancellation at shutdown
// (docs/design/07-io-layer.md §3.3). The registry itself is an asio service of the context's asio::io_context,
// defined in lib/avis/io/detail/operation_registry.h. Free of asio, so headers that only start operations don't need
// it.

namespace asio
{
    class execution_context;
} // namespace asio

namespace avis::io::detail
{
    class operation_registry;

    // An operation that io_context::request_stop() can cancel. cancel() runs under the registry's lock on the thread
    // that stops the context: it must only post work (to the operation's strand), never complete the operation inline.
    class registered_operation
    {
    public:
        virtual void cancel() noexcept = 0;

    protected:
        registered_operation() = default;
        ~registered_operation() = default;
        registered_operation(const registered_operation&) = delete;
        registered_operation& operator=(const registered_operation&) = delete;

    private:
        friend class operation_registry;

        registered_operation* previous_ = nullptr;
        registered_operation* next_ = nullptr;
    };

    // The registry of an asio context created by avis::io::io_context, nullptr for any other context (a plain
    // asio::io_context in a test)
    operation_registry* find_operation_registry(asio::execution_context& context) noexcept;

    // Adds the operation; false once the registry is closed (the context is stopping), and the caller then completes
    // with set_stopped without starting
    bool add_operation(operation_registry& registry, registered_operation& operation) noexcept;

    // Called by the operation before it completes its receiver; after it returns, cancel() is never called again
    void remove_operation(operation_registry& registry, registered_operation& operation) noexcept;
} // namespace avis::io::detail

#endif
