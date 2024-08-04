#include "avis/runtime/io/io_service.h"

#include "avis/core/common.h"
#include "avis/runtime/io/io_context.h"
#include "avis/runtime/io/io_write_request.h"

namespace io
{
    io_service::io_service(parallel::thread_pool& threads, std::size_t max_thread_count) : stop_service_{false}
    {
        for (std::size_t thread_index = 0; thread_index < max_thread_count; ++thread_index)
        {
            threads.execute([this]() { operation_servicing_main(); });
        }
    }

    io_service::~io_service()
    {
        stop_service_ = true;
        ops_queue_cv_.notify_all();
    }

    std::future<streams::memory_stream> io_service::async_read_file(file_descriptor descriptor)
    {
        std::unique_ptr<io_read_request<streams::memory_stream>> new_request =
            std::make_unique<io_read_request<streams::memory_stream>>(descriptor);
        std::future<streams::memory_stream> response = new_request->get_future();

        enqueue_request(std::move(new_request));

        return response;
    }

    std::future<void> io_service::async_write_file(file_descriptor descriptor)
    {
        std::unique_ptr<io_write_request<void>> new_request = std::make_unique<io_write_request<void>>(descriptor);
        std::future<void> response = new_request->get_future();

        enqueue_request(std::move(new_request));

        return response;
    }

    void io_service::operation_servicing_main()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(ops_queue_mutex_);
            ops_queue_cv_.wait(lock, [this]() { return stop_service_ || !ops_queue_.empty(); });

            if (stop_service_ && ops_queue_.empty())
            {
                lock.unlock();
                break;
            }

            std::unique_ptr<basic_io_request> request = std::move(ops_queue_.front());
            ops_queue_.pop();

            lock.unlock();

            process_request(request);
        }
    }

    void io_service::enqueue_request(std::unique_ptr<basic_io_request>&& request)
    {
        std::scoped_lock<std::mutex> lock{ops_queue_mutex_};

        ops_queue_.push(std::forward<std::unique_ptr<basic_io_request>>(request));
        ops_queue_cv_.notify_one();
    }

    void io_service::process_request(const std::unique_ptr<basic_io_request>& request)
    {
        request->process();
    }
} // namespace io
