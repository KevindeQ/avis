#include "avis/runtime/io/operation_context.h"

#include "avis/core/common.h"

namespace io
{
    operation_context& operation_context_manager::allocate_context()
    {
        operation_context* context = allocate_free_context();
        if (context == nullptr)
        {
            context = allocate_new_context();
        }

        return *context;
    }

    void operation_context_manager::deallocate_context(operation_context& context)
    {
        std::scoped_lock<std::mutex> lock{free_list_mutex_};
        free_list_.push_back(&context);
    }

    operation_context* operation_context_manager::allocate_free_context()
    {
        std::scoped_lock<std::mutex> lock{free_list_mutex_};
        if (!free_list_.empty())
        {
            operation_context* free_context = free_list_.back();
            free_list_.pop_back();
            return free_context;
        }

        return nullptr;
    }

    operation_context* operation_context_manager::allocate_new_context()
    {
        std::scoped_lock<std::mutex> lock{descriptors_mutex_};
        std::unique_ptr<operation_context>& new_context = descriptors_.emplace_back();
        return new_context.get();
    }
} // namespace io
