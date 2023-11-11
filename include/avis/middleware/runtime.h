#ifndef MIDDLEWARE_RUNTIME_H
#define MIDDLEWARE_RUNTIME_H

#include "avis/core/common.h"
#include "avis/middleware/basic_app.h"
#include "avis/middleware/configuration.h"

class runtime_builder;

class logger
{
public:
};

class logging_builder
{
public:
    void add_log(const std::filesystem::path& log_file);
    logger build();
};

class runtime
{
private:
    struct constructor_tag
    {};

public:
    // The additional constructor_tag effectively makes this constructor private.
    ENGINE_API runtime(const engine_configuration& config, [[maybe_unused]] constructor_tag /*unused*/);

    template<typename application_t>
    std::int32_t execute(basic_app_config& config);

    friend class runtime_builder;

private:
    ENGINE_API bool dispatch_system_messages() const;

private:
    engine_configuration configuration_;
};

template<typename application_t>
std::int32_t runtime::execute(basic_app_config& config)
{
    try
    {
        application_t app{config};

        app.on_launch();

        while (dispatch_system_messages())
        {
            app.on_update();
            app.on_render();
        }

        app.on_close();
    }
    catch ([[maybe_unused]] const std::runtime_error& exception)
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

class runtime_builder
{
public:
    ENGINE_API void configure_engine(std::function<void(configuration_builder&)> on_configure_engine);

    ENGINE_API void configure_logging(std::function<void(logging_builder&)> on_configure_logging);

    ENGINE_API runtime build();

private:
    configuration_builder config_builder_;
    logging_builder logging_builder_;
};

#endif
