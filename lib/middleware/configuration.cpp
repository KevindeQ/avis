#include "avis/middleware/configuration.h"

#include "avis/core/common.h"
#include "avis/middleware/configuration/configuration_parser.h"
#include "avis/middleware/configuration/ini_parser.h"

configuration_builder::configuration_builder() :
    // TODO: Think about making number of threads configurable
    threads_{max_thread_count_},
    file_load_service_{threads_, max_thread_count_}
{}

void configuration_builder::use_root_path(const std::filesystem::path& root_path)
{
    root_path_ = root_path;
}

void configuration_builder::configure_from_json(const std::filesystem::path& config_file)
{
    io::file_descriptor new_descriptor = file_context_.create_descriptor(config_file);
    std::future<engine_configuration> file_view = file_load_service_.async_read_file<engine_configuration>(
        new_descriptor, [this](const streams::memory_stream& stream) { return parse_json_configuration(stream); });
    config_files_.push_back(std::move(file_view));
}

void configuration_builder::configure_from_ini(const std::filesystem::path& config_file)
{
    io::file_descriptor new_descriptor = file_context_.create_descriptor(config_file);
    std::future<engine_configuration> file_view = file_load_service_.async_read_file<engine_configuration>(
        new_descriptor, [this](const streams::memory_stream& stream) { return parse_ini_configuration(stream); });
    config_files_.push_back(std::move(file_view));
}

void configuration_builder::configure_from_commandline(const char* argv[], const std::uint32_t argc) {}

engine_configuration configuration_builder::build()
{
    for (auto& config_file : config_files_)
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

    config_files_.clear();

    return engine_configuration{};
}

engine_configuration configuration_builder::parse_json_configuration(const streams::memory_stream& stream)
{
    return engine_configuration();
}

engine_configuration configuration_builder::parse_ini_configuration(const streams::memory_stream& stream)
{
    std::span<unsigned char> data = stream.data();
    std::string text{reinterpret_cast<char*>(data.data()), data.size()};

    configuration::ini_parser parser;
    configuration::configuration config = parser.parse(text);

    return engine_configuration();
}
