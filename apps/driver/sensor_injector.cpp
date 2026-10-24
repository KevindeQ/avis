#include "sensor_injector.h"

#include "avis/utils/overload.h"

#include <stdexec/execution.hpp>

#include <stdexcept>
#include <utility>
#include <variant>

injector_context::injector_context(avis::io::async_file data_file) noexcept : data_file_{ std::move(data_file) } {}

const avis::io::async_file& injector_context::file() const
{
    if (!data_file_.is_open())
    {
        throw std::runtime_error{ "File must be opened before it can be used" };
    }

    return data_file_;
}

void state_reading_header::on_enter(injector_context& context)
{
    [[maybe_unused]] const avis::io::async_file& file = context.file();

    // TODO: finish pipeline for parsing the header, along these lines (the buffer must outlive the read):
    //   stdexec::sender auto sender =
    //       file.read_at(0, header_buffer) // set_value(std::size_t bytes) | set_error(avis::error) | set_stopped()
    //       | stdexec::continues_on(stdexec::get_parallel_scheduler())
    //       | stdexec::then([&header_buffer](std::size_t bytes) { /* parse header_buffer[0, bytes) */ });

    //stdexec::spawn(sender);
}

void state_reading_stream::on_enter(injector_context& context)
{
    // TODO: Start any necessary stream read actions
}

void state_reading_wait::on_enter(injector_context& context)
{
    // TODO: Cleanup all outstanding state and buffers
}

void state_reading_error::on_enter(injector_context& context)
{
    // TODO: Cleanup all outstanding state and buffers
}

recording_sensor_injector::recording_sensor_injector() : execution_state_{ state_reading_wait{}, injector_context{} } {}

void recording_sensor_injector::tick()
{
    std::visit([this](auto&& state) { update_state(state); }, execution_state_.state());

    // TODO: Emit all sensor data for the current elapsed timestap
}

void recording_sensor_injector::update_state(state_reading_header& state)
{
    // TODO: Wait until the read operation is complete
    execution_state_.dispatch(read_completed{});
}

void recording_sensor_injector::update_state(state_reading_stream& state)
{
    // TODO: Wait until all outstanding read operations are complete
    execution_state_.dispatch(read_completed{});
}

void recording_sensor_injector::update_state(state_reading_wait& state)
{
    // TODO: Check if amount of buffered data is still sufficient
}

void recording_sensor_injector::update_state(state_reading_error& state) {}
