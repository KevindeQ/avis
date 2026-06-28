#ifndef VISUALIZER_SENSOR_INJECTOR_H
#define VISUALIZER_SENSOR_INJECTOR_H

#include "avis/finite_state_machine.h"

struct injector_context
{};

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

using fsm_recording_reader = avis::state_machine<
    injector_context,
    avis::transition<state_reading_header, read_completed, state_reading_stream>,
    avis::transition<state_reading_header, read_error, state_reading_error>,
    avis::transition<state_reading_stream, read_completed, state_reading_wait>,
    avis::transition<state_reading_stream, read_error, state_reading_error>,
    avis::transition<state_reading_wait, read_start, state_reading_stream>>;

struct state_reading_header
{
public:
    
};

struct state_reading_stream
{
public:
    
};

struct state_reading_wait
{
public:
    
};

struct state_reading_error
{
public:

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

    fsm_recording_reader execution_state_;
};

#endif
