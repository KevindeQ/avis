#include "avis/io/io_service.h"

#include "avis/common.h"

namespace io
{
    io_service::io_service(parallel::thread_pool& threads, std::size_t max_thread_count) :
        execution_context{},
        execution_context_guard{ asio::make_work_guard(execution_context) }
    {
        for (std::size_t index = 0; index < max_thread_count; ++index)
        {
            threads.execute([this]() { execution_context.run(); });
        }
    }

    io_service::~io_service()
    {
        // Allow the io_context to finish all work before stopping
        execution_context_guard.reset();

        // Stop execution as soon as possible
        execution_context.stop();
    }

    read_handle<std::vector<unsigned char>> io_service::read_file_async(const std::filesystem::path& file)
    {
        asio::io_context::executor_type executor = execution_context.get_executor();
        std::future<std::vector<unsigned char>> result_future = asio::co_spawn(
            executor,
            read_file_wrapper<std::vector<unsigned char>>(
                file,
                [=](std::span<unsigned char> file_contents) { return this->identity_type_converter(file_contents); }),
            asio::use_future);
        return read_handle<std::vector<unsigned char>>{ std::move(result_future) };
    }

    std::vector<unsigned char> io_service::identity_type_converter(std::span<unsigned char> file_contents)
    {
        return { file_contents.begin(), file_contents.end() };
    }
} // namespace io
