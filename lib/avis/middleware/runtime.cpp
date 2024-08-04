#include "avis/middleware/runtime.h"

runtime::runtime(const engine_configuration& config, [[maybe_unused]] constructor_tag /*unused*/) :
    configuration_{config}
{}

bool runtime::dispatch_system_messages() const
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0)
    {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);

        if (message.message == WM_QUIT)
        {
            return false;
        }
    }

    return true;
}

// ================================================================================================

void runtime_builder::configure_engine(std::function<void(configuration_builder&)> on_configure_engine)
{
    if (on_configure_engine)
    {
        on_configure_engine(config_builder_);
    }
}

void runtime_builder::configure_logging(std::function<void(logging_builder&)> on_configure_logging)
{
    if (on_configure_logging)
    {
        on_configure_logging(logging_builder_);
    }
}

runtime runtime_builder::build()
{
    return runtime{config_builder_.build(), runtime::constructor_tag{}};
}
