#ifndef VISUALIZER_SIMULATOR_DRIVER_H
#define VISUALIZER_SIMULATOR_DRIVER_H

#include "field_reader.h"
#include "avis/scene/semantic_scene_graph.h"
#include "sensor_injector.h"
#include "simulator_clock.h"

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/random_access_file.hpp>
#include <QObject>
#include <QQuickItem>

#include <chrono>
#include <span>
#include <thread>
#include <vector>

using namespace std::literals::chrono_literals;

struct recording_descriptor
{
public:
    std::uint64_t start_file_offset;
    std::uint64_t byte_size;
};

struct recording_header
{
public:
    template <typename test>
    void serialize(test& reader)
    {
        reader.field(signature);
        reader.field(version);
        reader.field(data_offset);

        reader.field(stream_count);
        for (std::uint8_t index = 0; index < stream_count; ++index)
        {
            std::uint64_t start_file_offset = 0;
            reader.field(start_file_offset);

            std::uint64_t byte_size = 0;
            reader.field(byte_size);

            stream_descriptors.push_back({ .start_file_offset = start_file_offset, .byte_size = byte_size });
        }

        reader.field(checksum);
    }

    std::uint32_t signature;
    std::uint8_t version;
    std::uint8_t data_offset;

    std::uint8_t stream_count;
    std::vector<recording_descriptor> stream_descriptors;

    std::uint32_t checksum;
};

class data_recording_stream
{
public:
};

class data_recording
{
public:
    data_recording(const recording_header& header);

private:
    recording_header header_;

    std::vector<data_recording_stream> streams_;
};

class simulator_driver : public QObject
{
    Q_OBJECT
public:
    explicit simulator_driver(QQuickItem* parent = nullptr);
    ~simulator_driver();

    static void declare_qml();

public slots:
    void update();

private:
    void simulate_step(std::chrono::milliseconds delta_time);

    asio::awaitable<recording_header> read_recording_header(asio::random_access_file& file_handle);

    // Wall-clock time tracking
    std::chrono::steady_clock::time_point current_time_ = std::chrono::steady_clock::now();
    std::chrono::milliseconds frame_time_ = 16ms;
    std::chrono::milliseconds max_frame_time = 100ms;
    std::chrono::milliseconds accumulator_ = 0ms;

    // Simulation time tracking
    avis::virtual_clock_state clock_state_;

    std::vector<std::jthread> threads_;

    asio::io_context execution_context_;
    asio::executor_work_guard<asio::io_context::executor_type> execution_context_guard_;

    asio::random_access_file data_recording_file_handle_;

    avis::scene::semantic_scene_graph scene_graph_;

    recording_sensor_injector injector_;
};

#endif
