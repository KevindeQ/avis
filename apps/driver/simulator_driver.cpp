#include "simulator_driver.h"

#include "serializer.h"

#include "avis/io/asio_use_sender.h"

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <QtDebug>
#include <stdexec/execution.hpp>

#include <filesystem>

struct transform
{
public:
    Eigen::Vector3f translation;
    Eigen::Quaternionf rotation;
};

struct read_context
{
public:
    read_context(std::size_t buffer_size);

    std::vector<unsigned char>& buffer();

private:
    std::vector<unsigned char> buffer_;
};

read_context::read_context(const std::size_t buffer_size) : buffer_{}
{
    buffer_.resize(512);
}

std::vector<unsigned char>& read_context::buffer()
{
    return buffer_;
}

simulator_driver::simulator_driver(QQuickItem* parent) :
    QObject(parent)
    
    /*data_recording_file_handle_{ execution_context_.get_executor() }*/
{
    // Initialize the clock tracking the simulation time
    simulator_clock::bind_state(clock_state_);

    std::filesystem::path file_path = "";
    /*data_recording_file_handle_.open(file_path.string(), asio::random_access_file::read_only);
    if (!data_recording_file_handle_.is_open())
    {
        throw std::runtime_error{ "An error occurred while opening the data recording" };
    }*/
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

// Completes with set_value(std::size_t bytes) | set_error(avis::error) | set_stopped(): use_sender peels the asio error
// code off (operation_aborted becomes set_stopped, other errors set_error)
stdexec::sender auto read_block(asio::random_access_file& file_handle, read_context& context)
{
    auto sndr = file_handle.async_read_some_at(0, asio::buffer(context.buffer()), avis::io::use_sender);
    return std::move(sndr);
}

recording_header read_header(read_context& context)
{
    recording_header header{};

    field_reader reader{ std::span<unsigned char>{ context.buffer() } };
    serializer s{ reader };
    s.serialize(header);

    return header;
}

void simulator_driver::simulate_step(const std::chrono::milliseconds delta_time)
{
    clock_state_.advance(delta_time);

    injector_.tick();

    /*std::future<recording_header> future_header = asio::co_spawn(
        execution_context_.get_executor(), read_recording_header(data_recording_file_handle_), asio::use_future);
    auto header = future_header.get();*/

    /*read_context context{ std::min(data_recording_file_handle_.size(), 512ull) };
    stdexec::sender auto snd = read_block(data_recording_file_handle_, context) |
                               stdexec::then([&context](std::size_t bytes) { return read_header(context); }) |
                               stdexec::then([](recording_header header) {});*/
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
