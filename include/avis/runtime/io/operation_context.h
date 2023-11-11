#ifndef RUNTIME_IO_OPERATION_CONTEXT_H
#define RUNTIME_IO_OPERATION_CONTEXT_H

#include "avis/core/common.h"

namespace io
{
    struct operation_context
    {
    public:
        OVERLAPPED kernel_context;
    };

    class operation_context_manager
    {
    public:
        operation_context_manager(const operation_context_manager&) = delete;
        operation_context_manager(operation_context_manager&&) = default;

        operation_context_manager& operator=(const operation_context_manager&) = delete;
        operation_context_manager& operator=(operation_context_manager&&) = default;

        operation_context& allocate_context();
        void deallocate_context(operation_context& context);

    private:
        operation_context* allocate_free_context();
        operation_context* allocate_new_context();

    private:
        std::vector<std::unique_ptr<operation_context>> descriptors_;
        std::vector<operation_context*> free_list_;

        std::mutex descriptors_mutex_;
        std::mutex free_list_mutex_;
    };
} // namespace io

#endif
