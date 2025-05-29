#include "avis/configuration.h"

#include "avis/common.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

engine_configuration engine_configuration::construct_from_yaml(const std::span<unsigned char> file_contents)
{
    return engine_configuration{};
}

configuration_builder::configuration_builder() :
    root_path{},
    config_file_handles{},

    threads{ max_thread_count },
    file_load_service{ threads, max_thread_count }
{}

void configuration_builder::use_root_path(const std::filesystem::path& path)
{
    root_path = path;
}

void configuration_builder::use_exe_as_root()
{
    std::wstring buffer;
    buffer.resize(MAX_PATH);

    // Keep growing the buffer until GetModuleFileNameW has enough space to store the path
    unsigned long bytes_copied = 0;
    do
    {
        bytes_copied = GetModuleFileNameW(NULL, buffer.data(), buffer.size());
        if (bytes_copied >= buffer.size())
        {
            unsigned long error_code = GetLastError();
            if (error_code == ERROR_INSUFFICIENT_BUFFER)
            {
                buffer.resize(buffer.size() + MAX_PATH);
            }
            else
            {
                throw std::system_error{ std::error_code{ static_cast<int>(error_code), std::system_category() } };
            }
        }
    }
    while (bytes_copied >= buffer.size());

    // Truncate buffer to remove excess space
    buffer.resize(bytes_copied);

    root_path = std::filesystem::path{ buffer }.remove_filename();
}

void configuration_builder::configure_from_yaml(const std::filesystem::path& config_file)
{
    std::filesystem::path file_path = config_file;
    if (config_file.is_relative())
    {
        file_path = root_path / config_file;
    }

    io::read_handle<engine_configuration> handle = file_load_service.read_structured_file_async<engine_configuration>(
        file_path,
        [this](std::span<unsigned char> contents) { return engine_configuration::construct_from_yaml(contents); });
    config_file_handles.push_back(std::move(handle));
}

void configuration_builder::configure_from_commandline(const char* argv[], const std::uint32_t argc) {}

engine_configuration configuration_builder::build()
{
    for (auto& config_file : config_file_handles)
    {
        try
        {
            engine_configuration file_contents = config_file.get();
            // TODO: merge the configuration from different files
        }
        catch (...)
        {
            // TODO: Log the error
        }
    }

    config_file_handles.clear();

    return engine_configuration{};
}
