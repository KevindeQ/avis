#ifndef EVENTS_EVENT_DISPATCHER_H
#    define EVENTS_EVENT_DISPATCHER_H

#include "avis/common.h"
#include "avis/containers/circular_buffer.h"

namespace events
{
    template<typename event_base_t>
    class event_dispatcher
    {
    public:
        template <typename event_derived_t>
        void on_event(std::string_view ident, std::function<void(const event_derived_t&)> callback);

        template<typename event_derived_t, typename... event_args_t>
        void dispatch_event(event_args_t&&... args);

        void process_events();

    private:
        containers::circular_buffer<std::unique_ptr<event_base_t>> events;
    };

    template<typename event_base_t>
    template<typename event_derived_t>
    void event_dispatcher<event_base_t>::on_event(
        std::string_view ident, std::function<void(const event_derived_t&)> callback)
    {

    }

    template<typename event_base_t>
    template<typename event_derived_t, typename... event_args_t>
    void event_dispatcher<event_base_t>::dispatch_event(event_args_t&&... args)
    {

    }


    template<typename event_base_t>
    void event_dispatcher<event_base_t>::process_events()
    {}
}

#endif
