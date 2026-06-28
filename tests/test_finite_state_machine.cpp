#include "avis/finite_state_machine.h"

#include <catch2/catch_test_macros.hpp>

#include <variant>
#include <type_traits>

// Test model
struct context
{
    int energy = 100;
    int score = 0;
};

struct idle
{
    bool entered = false;
    bool exited = false;

    void on_enter(context&)
    {
        entered = true;
    }
    void on_exit(context&)
    {
        exited = true;
    }
};

struct running
{
    void on_enter(context& ctx)
    {
        ctx.energy -= 10;
    }
    void on_exit(context& ctx)
    {
        ctx.score += 5;
    }
};

struct paused
{};

struct start
{};
struct stop
{};
struct pause
{};
struct resume
{};

// Guard
struct has_energy
{
    bool operator()(context& ctx, const start&, idle&) const
    {
        return ctx.energy > 0;
    }
};

// FSM type
using test_fsm = avis::state_machine<
    context,
    avis::transition<idle, start, running, has_energy>,
    avis::transition<running, stop, idle>,
    avis::transition<running, pause, paused>,
    avis::transition<paused, resume, running>>;

// --------------------------------------------------
// Tests
// --------------------------------------------------

TEST_CASE("fsm performs basic transitions", "[fsm]")
{
    test_fsm fsm{ idle{}, context{} };

    REQUIRE(std::holds_alternative<idle>(fsm.state()));

    bool transitioned = fsm.dispatch(start{});
    REQUIRE(transitioned);
    REQUIRE(std::holds_alternative<running>(fsm.state()));

    transitioned = fsm.dispatch(stop{});
    REQUIRE(transitioned);
    REQUIRE(std::holds_alternative<idle>(fsm.state()));
}

TEST_CASE("fsm ignores invalid transitions", "[fsm]")
{
    test_fsm fsm{ idle{}, context{} };

    bool transitioned = fsm.dispatch(stop{});

    REQUIRE_FALSE(transitioned);
    REQUIRE(std::holds_alternative<idle>(fsm.state()));
}

TEST_CASE("fsm respects guard conditions", "[fsm]")
{
    context ctx;
    ctx.energy = 0; // guard should fail

    test_fsm fsm{ idle{}, ctx };

    bool transitioned = fsm.dispatch(start{});

    REQUIRE_FALSE(transitioned);
    REQUIRE(std::holds_alternative<idle>(fsm.state()));
}

TEST_CASE("fsm modifies context during transitions", "[fsm]")
{
    test_fsm fsm{ idle{}, context{ 100, 0 } };

    fsm.dispatch(start{}); // energy -10
    REQUIRE(fsm.context().energy == 90);

    fsm.dispatch(stop{}); // score +5
    REQUIRE(fsm.context().score == 5);
}

TEST_CASE("fsm handles multi-step transitions", "[fsm]")
{
    test_fsm fsm{ idle{}, context{} };

    fsm.dispatch(start{});
    REQUIRE(std::holds_alternative<running>(fsm.state()));

    fsm.dispatch(pause{});
    REQUIRE(std::holds_alternative<paused>(fsm.state()));

    fsm.dispatch(resume{});
    REQUIRE(std::holds_alternative<running>(fsm.state()));
}

TEST_CASE("entry and exit hooks are called", "[fsm]")
{
    idle i;
    context ctx{};

    test_fsm fsm{ i, ctx };

    REQUIRE(std::get<idle>(fsm.state()).entered);

    fsm.dispatch(start{});

    // ensure transition happened
    REQUIRE(std::holds_alternative<running>(fsm.state()));
}

TEST_CASE("state variant contains all expected states", "[fsm]")
{
    using state = typename test_fsm::state_type;

    STATIC_REQUIRE(std::variant_size_v<state> == 3);

    STATIC_REQUIRE(std::is_same_v<state, std::variant<idle, running, paused>>);
}
