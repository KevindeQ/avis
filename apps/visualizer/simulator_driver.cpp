#include "simulator_driver.h"

#include "serializer.h"

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <QtDebug>

#include <filesystem>

struct transform
{
public:
    Eigen::Vector3f translation;
    Eigen::Quaternionf rotation;
};

simulator_driver::simulator_driver(QQuickItem* parent) :
    QObject(parent),
    execution_context_guard_{ asio::make_work_guard(execution_context_) },

    data_recording_file_handle_{ execution_context_.get_executor() }
{
    // Initialize the clock tracking the simulation time
    simulator_clock::bind_state(clock_state_);

    // Spin up worker threads for asynchronously handling IO requests
    static std::size_t max_thread_count = 2;
    for (std::size_t index = 0; index < max_thread_count; ++index)
    {
        threads_.emplace_back([this]() { execution_context_.run(); });
    }

    std::filesystem::path file_path = "";
    /*data_recording_file_handle_.open(file_path.string(), asio::random_access_file::read_only);
    if (!data_recording_file_handle_.is_open())
    {
        throw std::runtime_error{ "An error occurred while opening the data recording" };
    }*/
}

simulator_driver::~simulator_driver()
{
    // Stop asio execution context from picking up more work
    execution_context_guard_.reset();
    execution_context_.stop();

    // Wait until all threads have stopped
    for (auto& thread : threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

void simulator_driver::declare_qml()
{
    qmlRegisterType<simulator_driver>("Avis", 1, 0, "SimulatorDriver");
}

void simulator_driver::update()
{
    // Update the simulation using a fixed timestep

    const auto new_time = std::chrono::steady_clock::now();
    auto delta_time = std::chrono::duration_cast<std::chrono::milliseconds>(new_time - current_time_);

    // Clamp the delta_time to don't do too many simulation steps when the time was frozen due to debugging
    if (delta_time > max_frame_time)
    {
        delta_time = max_frame_time;
    }

    accumulator_ += delta_time;
    while (accumulator_ >= frame_time_)
    {
        simulate_step(frame_time_);

        accumulator_ -= frame_time_;
    }

    current_time_ = new_time;
}

void simulator_driver::simulate_step(const std::chrono::milliseconds delta_time)
{
    clock_state_.advance(delta_time);

    injector_.tick();

    /*std::future<recording_header> future_header = asio::co_spawn(
        execution_context_.get_executor(), read_recording_header(data_recording_file_handle_), asio::use_future);
    auto header = future_header.get();*/
}

asio::awaitable<recording_header> simulator_driver::read_recording_header(asio::random_access_file& file_handle)
{
    recording_header header{};

    try
    {
        const std::size_t buffer_size = std::min(file_handle.size(), 512ull);

        std::vector<unsigned char> buffer;
        buffer.resize(512);

        auto [error, bytes_read] =
            co_await file_handle.async_read_some_at(0, asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
        if (error)
        {
            // TODO: Properly handle error
        }

        field_reader reader{ std::span<unsigned char>{ buffer } };
        serializer s{ reader };
        s.serialize(header);
        // TODO: Properly handle serialization not completing without error
    }
    catch (const std::system_error& error)
    {
        // TODO: Properly handle error
    }
    catch (const std::runtime_error& error)
    {
        // TODO: Properly handle error
    }

    co_return header;
}

data_recording::data_recording(const recording_header& header) : header_{ header }
{
    for (std::size_t index = 0; index < header_.stream_descriptors.size(); ++index)
    {
        streams_.push_back({});
    }
}
