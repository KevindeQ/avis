#ifndef IO_IO_SERVICE_H
#define IO_IO_SERVICE_H

#include "avis/common.h"
#include "avis/parallel/thread_pool.h"

#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/io_context.hpp"
#include "asio/random_access_file.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/use_future.hpp"

namespace io
{
    /*class binary_reader
    {
    public:
        binary_reader(std::span<std::uint8_t> data);

        [[nodiscard]] std::uint8_t read_uint8();
        [[nodiscard]] std::uint16_t read_uint16();
        [[nodiscard]] std::uint32_t read_uint32();
        [[nodiscard]] std::uint64_t read_uint64();

        [[nodiscard]] std::int8_t read_int8();
        [[nodiscard]] std::int16_t read_int16();
        [[nodiscard]] std::int32_t read_int32();
        [[nodiscard]] std::int64_t read_int64();

    private:
    };*/

    template<typename result_type>
    class read_handle
    {
    public:
        read_handle(std::future<result_type> handle);

        result_type get();

        void wait() const;

    private:
        std::future<result_type> result_handle;
    };

    template<typename result_type>
    read_handle<result_type>::read_handle(std::future<result_type> handle) : result_handle{ std::move(handle) }
    {}

    template<typename result_type>
    result_type read_handle<result_type>::get()
    {
        return result_handle.get();
    }

    template<typename result_type>
    void read_handle<result_type>::wait() const
    {
        result_handle.wait();
    }

    class io_service
    {
    public:
        explicit io_service(
            parallel::thread_pool& threads, std::size_t max_thread_count = std::thread::hardware_concurrency());
        io_service(const io_service&) = delete;
        io_service(io_service&&) = default;
        ~io_service();

        io_service& operator=(const io_service&) = delete;
        io_service& operator=(io_service&&) = default;

        // TODO: Allow any container to be used as result type
        read_handle<std::vector<unsigned char>> read_file_async(const std::filesystem::path& file);

        template<typename return_type>
        read_handle<return_type> read_structured_file_async(
            const std::filesystem::path& file, std::function<return_type(std::span<unsigned char>)> type_converter);

    private:
        template<typename return_type>
        asio::awaitable<return_type> read_file_wrapper(
            const std::filesystem::path file, std::function<return_type(std::span<unsigned char>)> type_converter);

        std::vector<unsigned char> identity_type_converter(std::span<unsigned char> file_contents);

        asio::io_context execution_context;
        asio::executor_work_guard<asio::io_context::executor_type> execution_context_guard;
    };

    template<typename return_type>
    read_handle<return_type> io_service::read_structured_file_async(
        const std::filesystem::path& file, std::function<return_type(std::span<unsigned char>)> type_converter)
    {
        asio::io_context::executor_type executor = execution_context.get_executor();
        std::future<return_type> result_future =
            asio::co_spawn(executor, read_file_wrapper(file, type_converter), asio::use_future);
        return read_handle<return_type>{ std::move(result_future) };
    }

    template<typename return_type>
    asio::awaitable<return_type> io_service::read_file_wrapper(
        const std::filesystem::path file, std::function<return_type(std::span<unsigned char>)> type_converter)
    {
        asio::random_access_file file_handle{ execution_context, file.string(), asio::random_access_file::read_only };

        std::vector<std::uint8_t> buffer{};
        buffer.resize(file_handle.size());

        auto [error_code, bytes_read] =
            co_await file_handle.async_read_some_at(0, asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));

        co_return type_converter(buffer);
    }
} // namespace io

#endif
