#ifndef AVIS_FINITE_STATE_MACHINE_H
#define AVIS_FINITE_STATE_MACHINE_H

#include <concepts>
#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace avis
{
    namespace details
    {
        // Optional state hooks
        //
        // A state may define either, both, or neither of these:
        //
        //   void on_enter(Context&);
        //   void on_exit(Context&);
        //
        // The FSM detects them at compile time and calls them only when present.
        template<typename State, typename Context>
        concept has_on_enter = requires(State& state, Context& context) {
            { state.on_enter(context) } -> std::same_as<void>;
        };

        template<typename State, typename Context>
        concept has_on_exit = requires(State& state, Context& context) {
            { state.on_exit(context) } -> std::same_as<void>;
        };

        // Type-list utilities
        template<typename... Ts>
        struct type_list
        {};

        template<typename List, typename T>
        struct push_unique;

        template<typename... Ts, typename T>
        struct push_unique<type_list<Ts...>, T>
        {
            using type = std::conditional_t<(std::same_as<T, Ts> || ...), type_list<Ts...>, type_list<Ts..., T>>;
        };

        template<typename List, typename Transition>
        struct append_transition_states
        {
        private:
            using with_source = typename push_unique<List, typename Transition::source>::type;

        public:
            using type = typename push_unique<with_source, typename Transition::target>::type;
        };

        template<typename List, typename... Transitions>
        struct collect_states_impl;

        template<typename List>
        struct collect_states_impl<List>
        {
            using type = List;
        };

        template<typename List, typename First, typename... Rest>
        struct collect_states_impl<List, First, Rest...>
        {
        private:
            using next = typename append_transition_states<List, First>::type;

        public:
            using type = typename collect_states_impl<next, Rest...>::type;
        };

        template<typename... Transitions>
        using state_list_t = typename collect_states_impl<type_list<>, Transitions...>::type;

        template<typename List>
        struct variant_from_type_list;

        template<typename... States>
        struct variant_from_type_list<type_list<States...>>
        {
            using type = std::variant<States...>;
        };

        template<typename... Transitions>
        using state_variant_t = typename variant_from_type_list<state_list_t<Transitions...>>::type;
    }

    // Default guard, always lets state transition happen
    struct always
    {
        template<typename Context, typename Event, typename State>
        constexpr bool operator()(Context&, const Event&, State&) const noexcept
        {
            return true;
        }
    };

    template<typename SourceState, typename EventType, typename TargetState, typename Guard = always>
    struct transition
    {
        using source = SourceState;
        using event = EventType;
        using target = TargetState;
        using guard = Guard;

        [[no_unique_address]] Guard guard_fn{};

        constexpr transition() = default;

        constexpr explicit transition(Guard guard) : guard_fn(std::move(guard)) {}
    };

    template<typename Context, typename... Transitions>
    class state_machine
    {
    public:
        using context_type = Context;
        using state_type = details::state_variant_t<Transitions...>;
        using transition_tuple_type = std::tuple<Transitions...>;

        explicit state_machine(state_type initial, Context context)
            requires((std::default_initializable<Transitions> && ...))
            : state_(std::move(initial)), context_(std::move(context)), transitions_{}
        {
            enter_current();
        }

        explicit state_machine(state_type initial, Context context, Transitions... transitions) :
            state_(std::move(initial)),
            context_(std::move(context)),
            transitions_(std::move(transitions)...)
        {
            enter_current();
        }

        template<typename Event>
        [[nodiscard]] bool dispatch(const Event& event)
        {
            std::optional<state_type> next;

            std::visit(
                [&](auto& current_state)
                {
                    std::apply(
                        [&](auto&... transition) { (try_transition(transition, current_state, event, next), ...); },
                        transitions_);
                },
                state_);

            if (!next)
            {
                return false;
            }

            state_ = std::move(*next);
            enter_current();
            return true;
        }

        [[nodiscard]] const state_type& state() const noexcept
        {
            return state_;
        }

        [[nodiscard]] state_type& state() noexcept
        {
            return state_;
        }

        [[nodiscard]] Context& context() noexcept
        {
            return context_;
        }

        [[nodiscard]] const Context& context() const noexcept
        {
            return context_;
        }

    private:
        state_type state_;
        Context context_;
        transition_tuple_type transitions_;

        void enter_current()
        {
            std::visit([&](auto& state) { call_on_enter(state); }, state_);
        }

        template<typename State>
        void call_on_enter(State& state)
        {
            if constexpr (details::has_on_enter<State, Context>)
            {
                state.on_enter(context_);
            }
        }

        template<typename State>
        void call_on_exit(State& state)
        {
            if constexpr (details::has_on_exit<State, Context>)
            {
                state.on_exit(context_);
            }
        }

        template<typename Transition, typename CurrentState, typename Event>
        void try_transition(
            Transition& transition, CurrentState& current_state, const Event& event, std::optional<state_type>& next)
        {
            if (next)
            {
                return;
            }

            using transition_type = std::remove_cvref_t<Transition>;
            using current_state_type = std::remove_cvref_t<CurrentState>;
            using event_type = std::remove_cvref_t<Event>;

            if constexpr (
                std::same_as<current_state_type, typename transition_type::source> &&
                std::same_as<event_type, typename transition_type::event>)
            {
                if (std::invoke(transition.guard_fn, context_, event, current_state))
                {
                    call_on_exit(current_state);
                    next.emplace(typename transition_type::target{});
                }
            }
        }
    };
} // namespace avis

#endif
