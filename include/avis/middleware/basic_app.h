#ifndef MIDDLEWARE_BASIC_APP_H
#define MIDDLEWARE_BASIC_APP_H

#include "avis/core/common.h"
#include "avis/runtime/io/io_context.h"
#include "avis/runtime/io/io_service.h"
#include "avis/runtime/parallel/thread_pool.h"

struct basic_app_config
{
public:
    std::chrono::nanoseconds fixed_time_delta;
};

class basic_app
{
public:
    basic_app(const basic_app_config& config);
    virtual ~basic_app() = default;

    virtual void on_launch() {}
    virtual void on_close() {}

    virtual void on_suspend() {}
    virtual void on_resume() {}

    virtual void on_update() {}

    virtual void on_render_frame() {}

protected:
    const basic_app_config& config;

    parallel::thread_pool threads;
    io::io_context file_context;
    io::io_service file_load_service;
};

#endif
