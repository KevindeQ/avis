#include "operation_registry.h"

#include <asio/execution_context.hpp>

namespace avis::io::detail
{
    asio::execution_context::id operation_registry::id;

    operation_registry::operation_registry(asio::execution_context& context) :
        asio::execution_context::service{ context }
    {}

    bool operation_registry::add(registered_operation& operation)
    {
        const std::scoped_lock lock{ mutex_ };
        if (closed_)
        {
            return false;
        }

        operation.previous_ = nullptr;
        operation.next_ = head_;
        if (head_ != nullptr)
        {
            head_->previous_ = &operation;
        }
        head_ = &operation;
        ++count_;
        return true;
    }

    void operation_registry::remove(registered_operation& operation)
    {
        bool now_empty = false;
        {
            const std::scoped_lock lock{ mutex_ };
            if (operation.previous_ != nullptr)
            {
                operation.previous_->next_ = operation.next_;
            }
            else
            {
                head_ = operation.next_;
            }
            if (operation.next_ != nullptr)
            {
                operation.next_->previous_ = operation.previous_;
            }
            operation.previous_ = nullptr;
            operation.next_ = nullptr;
            --count_;
            now_empty = count_ == 0;
        }
        if (now_empty)
        {
            empty_.notify_all();
        }
    }

    void operation_registry::close_and_cancel_all() noexcept
    {
        const std::scoped_lock lock{ mutex_ };
        closed_ = true;
        for (registered_operation* operation = head_; operation != nullptr; operation = operation->next_)
        {
            operation->cancel();
        }
    }

    bool operation_registry::wait_empty(const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{ mutex_ };
        return empty_.wait_for(lock, timeout, [this] { return count_ == 0; });
    }

    std::size_t operation_registry::size() const
    {
        const std::scoped_lock lock{ mutex_ };
        return count_;
    }

    operation_registry* find_operation_registry(asio::execution_context& context) noexcept
    {
        if (!asio::has_service<operation_registry>(context))
        {
            return nullptr;
        }
        return &asio::use_service<operation_registry>(context);
    }

    bool add_operation(operation_registry& registry, registered_operation& operation) noexcept
    {
        try
        {
            return registry.add(operation);
        }
        catch (...)
        {
            return false; // the mutex failed to lock: treat it like a stopping context
        }
    }

    void remove_operation(operation_registry& registry, registered_operation& operation) noexcept
    {
        try
        {
            registry.remove(operation);
        }
        catch (...)
        {
            // std::mutex::lock only throws on a system failure; nothing sensible is left to do
        }
    }
} // namespace avis::io::detail
