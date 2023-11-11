#ifndef RUNTIME_IO_IO_SERVICE_H
#define RUNTIME_IO_IO_SERVICE_H

#include "avis/core/common.h"
#include "avis/core/streams/memory_stream.h"
#include "avis/runtime/io/file_descriptor.h"
#include "avis/runtime/io/io_read_request.h"
#include "avis/runtime/io/io_request.h"

namespace io
{
    class io_service
    {
    public:
        ENGINE_API explicit io_service(
            parallel::thread_pool& threads, std::size_t max_thread_count = std::thread::hardware_concurrency());
        io_service(const io_service&) = delete;
        io_service(io_service&&) = default;
        ENGINE_API ~io_service();

        io_service& operator=(const io_service&) = delete;
        io_service& operator=(io_service&&) = default;

        ENGINE_API std::future<streams::memory_stream> async_read_file(file_descriptor descriptor);

        template<typename result_t>
        std::future<result_t> async_read_file(
            file_descriptor descriptor, std::function<result_t(const streams::memory_stream& stream)> converter);

        ENGINE_API std::future<void> async_write_file(file_descriptor descriptor);

    private:
        void operation_servicing_main();

        ENGINE_API void enqueue_request(std::unique_ptr<basic_io_request>&& request);
        void process_request(const std::unique_ptr<basic_io_request>& request);

    private:
        std::queue<std::unique_ptr<basic_io_request>> ops_queue_;
        std::mutex ops_queue_mutex_;
        std::condition_variable ops_queue_cv_;
        bool stop_service_;
    };

    template<typename result_t>
    std::future<result_t> io_service::async_read_file(
        file_descriptor descriptor, std::function<result_t(const streams::memory_stream& stream)> converter)
    {
        std::unique_ptr<io_read_request<result_t>> new_request =
            std::make_unique<io_read_request<result_t>>(descriptor, converter);
        std::future<result_t> response = new_request->get_future();

        enqueue_request(std::move(new_request));

        return response;
    }
} // namespace io

#endif
