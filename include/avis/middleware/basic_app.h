#ifndef MIDDLEWARE_BASIC_APP_H
#define MIDDLEWARE_BASIC_APP_H

#include "avis/core/common.h"
#include "avis/middleware/step_timer.h"

struct basic_app_config
{
public:
    std::chrono::nanoseconds fixed_time_delta;
};

class basic_app
{
public:
    ENGINE_API basic_app(basic_app_config& config) : config{config} {}
    ENGINE_API virtual ~basic_app() = default;

    virtual void on_launch() {}
    virtual void on_close() {}

    virtual void on_suspend() {}
    virtual void on_resume() {}

    virtual void on_update(const step_timer& timer) {}

    virtual void on_render() {}

protected:
    basic_app_config& config;
};

#endif
