#ifndef MIDDLEWARE_INPUT_INPUT_DISPATCHER_H
#define MIDDLEWARE_INPUT_INPUT_DISPATCHER_H

#include "avis/core/common.h"
#include "avis/middleware/input/input_context.h"

namespace input
{
    class input_dispatcher
    {
    public:
        ENGINE_API void handle_raw_input(const WPARAM wparam, const LPARAM lparam);

        ENGINE_API void dispatch_input();

        ENGINE_API void push_context(const input_context& context);
        ENGINE_API void pop_context(const input_context& context);

    private:
        std::vector<input_context> active_contexts_;
    };
} // namespace input

#endif
