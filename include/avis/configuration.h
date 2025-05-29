#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "avis/common.h"
#include "avis/io/io_service.h"
#include "avis/parallel/thread_pool.h"

class engine_configuration
{
public:
    static engine_configuration construct_from_yaml(const std::span<unsigned char> file_contents);
};

class configuration_builder
{
public:
    configuration_builder();

    void use_root_path(const std::filesystem::path& root_path);
    void use_exe_as_root();

    void configure_from_yaml(const std::filesystem::path& config_file);

    void configure_from_commandline(const char* argv[], const std::uint32_t argc);

    engine_configuration build();

private:
    std::filesystem::path root_path;
    std::vector<io::read_handle<engine_configuration>> config_file_handles;

    parallel::thread_pool threads;
    io::io_service file_load_service;

    static constexpr std::size_t max_thread_count = 2;
};

#endif
