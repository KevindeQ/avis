#include "sensor_injector.h"

#include "avis/utils/overload.h"

#include <variant>

recording_sensor_injector::recording_sensor_injector() : execution_state_{ state_reading_wait{}, injector_context{} } {}

void recording_sensor_injector::tick()
{
    std::visit([this](auto&& state) { update_state(state); }, execution_state_.state());
}

void recording_sensor_injector::update_state(state_reading_header& state) {}

void recording_sensor_injector::update_state(state_reading_stream& state) {}

void recording_sensor_injector::update_state(state_reading_wait& state) {}

void recording_sensor_injector::update_state(state_reading_error& state) {}
