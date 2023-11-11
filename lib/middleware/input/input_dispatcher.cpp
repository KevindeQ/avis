#include "avis/middleware/input/input_dispatcher.h"

#include "avis/core/common.h"

namespace input
{
    void input_dispatcher::handle_raw_input(const WPARAM wparam, const LPARAM lparam) {}

    void input_dispatcher::dispatch_input() {}

    void input_dispatcher::push_context(const input_context& context) {}

    void input_dispatcher::pop_context(const input_context& context) {}
} // namespace input
