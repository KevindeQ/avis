#ifndef VISUALIZER_SENSOR_INJECTOR_H
#define VISUALIZER_SENSOR_INJECTOR_H

#include "avis/finite_state_machine.h"
#include "avis/io/async_file.h"

struct injector_context
{
public:
    injector_context() = default; // no recording opened yet

    // Takes the opened recording, e.g. from avis::io::async_file::open(path, avis::io::open_mode::read), which reports
    // a missing or unreadable file as a result before the context exists
    explicit injector_context(avis::io::async_file data_file) noexcept;

    // Throws std::runtime_error when no recording was opened
    const avis::io::async_file& file() const;

private:
    avis::io::async_file data_file_;
};

// States
struct state_reading_header;
struct state_reading_stream;
struct state_reading_wait;
struct state_reading_error;

// Events
struct read_start
{};
struct read_completed
{};
struct read_error
{};

using fsm_reader_state = avis::state_machine<
    injector_context,
    avis::transition<state_reading_header, read_completed, state_reading_stream>,
    avis::transition<state_reading_header, read_error, state_reading_error>,
    avis::transition<state_reading_stream, read_completed, state_reading_wait>,
    avis::transition<state_reading_stream, read_error, state_reading_error>,
    avis::transition<state_reading_wait, read_start, state_reading_stream>>;

struct state_reading_header
{
public:
    void on_enter(injector_context& context);
};

struct state_reading_stream
{
public:
    void on_enter(injector_context& context);
};

struct state_reading_wait
{
public:
    void on_enter(injector_context& context);
};

struct state_reading_error
{
public:
    void on_enter(injector_context& context);
};

class recording_sensor_injector
{
public:
    recording_sensor_injector();

    void tick();

private:
    void update_state(state_reading_header& state);
    void update_state(state_reading_stream& state);
    void update_state(state_reading_wait& state);
    void update_state(state_reading_error& state);

    // Async IO runs on the runtime's pool-hosted IO context (avis::runtime::io_context())
    fsm_reader_state execution_state_;
};

#endif
