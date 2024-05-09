#include "avis/middleware/basic_app.h"

basic_app::basic_app(const basic_app_config& config) :
    config{ config },

    threads{ 3 },
    file_context{},
    file_load_service{ threads, 3 }
{}
