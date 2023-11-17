#ifndef MIDDLEWARE_CONFIGURATION_H
#define MIDDLEWARE_CONFIGURATION_H

#include "avis/core/common.h"
#include "avis/runtime/io/io_context.h"
#include "avis/runtime/io/io_service.h"
#include "avis/runtime/parallel/thread_pool.h"

class engine_configuration
{
public:
};

class configuration_builder
{
public:
    ENGINE_API configuration_builder();

    ENGINE_API void use_root_path(const std::filesystem::path& root_path);
    ENGINE_API void configure_from_json(const std::filesystem::path& config_file);
    ENGINE_API void configure_from_ini(const std::filesystem::path& config_file);

    ENGINE_API void configure_from_commandline(const char* argv[], const std::uint32_t argc);

    ENGINE_API engine_configuration build();

private:
    engine_configuration parse_json_configuration(const streams::memory_stream& stream);
    engine_configuration parse_ini_configuration(const streams::memory_stream& stream);

private:
    std::filesystem::path root_path_;
    std::vector<std::future<engine_configuration>> config_files_;

    parallel::thread_pool threads;
    io::io_context file_context;
    io::io_service file_load_service;

    static constexpr std::size_t max_thread_count = 2;
};

#endif
