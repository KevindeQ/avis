// White-box tests for the IO context's operation registry (lib/avis/io/detail/operation_registry.h, a private
// header)

#include "avis/io/detail/operation_registry.h" // private, via lib/
#include "avis/io/detail/registered_operation.h"

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace
{
    class counting_operation final : public avis::io::detail::registered_operation
    {
    public:
        void cancel() noexcept override
        {
            ++cancel_calls;
        }

        std::atomic<int> cancel_calls = 0;
    };

    // A plain asio context with the registry service, as avis::io::io_context sets it up
    struct registry_fixture
    {
        asio::io_context context;
        avis::io::detail::operation_registry& registry =
            asio::make_service<avis::io::detail::operation_registry>(context);
    };
} // namespace

TEST_CASE("the registry unlinks the head, a middle and the tail operation", "[io][operation_registry]")
{
    registry_fixture fixture;
    auto& registry = fixture.registry;

    // Added a, b, c: c is at the head of the list, a at the tail
    std::array<counting_operation, 3> operations;
    for (auto& operation : operations)
    {
        REQUIRE(registry.add(operation));
    }
    REQUIRE(registry.size() == 3);

    counting_operation& tail = operations[0];
    counting_operation& middle = operations[1];
    counting_operation& head = operations[2];

    // Which operations are still linked shows in which ones close_and_cancel_all reaches
    SECTION("head")
    {
        registry.remove(head);
        CHECK(registry.size() == 2);
        registry.close_and_cancel_all();
        CHECK(head.cancel_calls == 0);
        CHECK(middle.cancel_calls == 1);
        CHECK(tail.cancel_calls == 1);
        registry.remove(middle);
        registry.remove(tail);
    }

    SECTION("middle")
    {
        registry.remove(middle);
        CHECK(registry.size() == 2);
        registry.close_and_cancel_all();
        CHECK(head.cancel_calls == 1);
        CHECK(middle.cancel_calls == 0);
        CHECK(tail.cancel_calls == 1);
        registry.remove(head);
        registry.remove(tail);
    }

    SECTION("tail")
    {
        registry.remove(tail);
        CHECK(registry.size() == 2);
        registry.close_and_cancel_all();
        CHECK(head.cancel_calls == 1);
        CHECK(middle.cancel_calls == 1);
        CHECK(tail.cancel_calls == 0);
        registry.remove(head);
        registry.remove(middle);
    }

    CHECK(registry.size() == 0);
}

TEST_CASE("the registry empties in any removal order and accepts an operation again", "[io][operation_registry]")
{
    registry_fixture fixture;
    auto& registry = fixture.registry;

    std::array<counting_operation, 3> operations;
    for (auto& operation : operations)
    {
        REQUIRE(registry.add(operation));
    }

    registry.remove(operations[1]);
    registry.remove(operations[2]);
    registry.remove(operations[0]); // the last one
    CHECK(registry.size() == 0);

    // Removal reset the links, so the same object can be added again
    REQUIRE(registry.add(operations[1]));
    CHECK(registry.size() == 1);
    registry.close_and_cancel_all();
    CHECK(operations[0].cancel_calls == 0);
    CHECK(operations[1].cancel_calls == 1);
    CHECK(operations[2].cancel_calls == 0);
    registry.remove(operations[1]);
}

TEST_CASE("close_and_cancel_all cancels every operation once and closes the registry", "[io][operation_registry]")
{
    registry_fixture fixture;
    auto& registry = fixture.registry;

    std::array<counting_operation, 4> operations;
    for (auto& operation : operations)
    {
        REQUIRE(registry.add(operation));
    }

    registry.close_and_cancel_all();
    for (const auto& operation : operations)
    {
        CHECK(operation.cancel_calls == 1);
    }

    // Closed: new operations are refused, registered ones still remove themselves
    counting_operation late;
    CHECK_FALSE(registry.add(late));
    CHECK(registry.size() == 4);
    for (auto& operation : operations)
    {
        registry.remove(operation);
    }
    CHECK(registry.size() == 0);

    // A second close finds nothing to cancel
    registry.close_and_cancel_all();
    for (const auto& operation : operations)
    {
        CHECK(operation.cancel_calls == 1);
    }
    CHECK(late.cancel_calls == 0);
}

TEST_CASE("wait_empty returns when the last operation is removed, or on timeout", "[io][operation_registry]")
{
    registry_fixture fixture;
    auto& registry = fixture.registry;

    SECTION("an empty registry")
    {
        CHECK(registry.wait_empty(0ms));
    }

    SECTION("timeout")
    {
        counting_operation operation;
        REQUIRE(registry.add(operation));

        const auto begin = std::chrono::steady_clock::now();
        CHECK_FALSE(registry.wait_empty(30ms));
        CHECK(std::chrono::steady_clock::now() - begin >= 25ms);

        registry.remove(operation);
    }

    SECTION("removed by another thread")
    {
        std::array<counting_operation, 2> operations;
        for (auto& operation : operations)
        {
            REQUIRE(registry.add(operation));
        }

        std::thread remover{ [&registry, &operations]
                             {
                                 std::this_thread::sleep_for(20ms);
                                 registry.remove(operations[0]);
                                 std::this_thread::sleep_for(20ms);
                                 registry.remove(operations[1]);
                             } };

        const auto begin = std::chrono::steady_clock::now();
        CHECK(registry.wait_empty(5s));
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        remover.join();

        CHECK(elapsed >= 30ms); // not woken by the first removal
        CHECK(elapsed < 2s);
        CHECK(registry.size() == 0);
    }
}

TEST_CASE("the public registry functions reach the service", "[io][operation_registry]")
{
    registry_fixture fixture;

    CHECK(avis::io::detail::find_operation_registry(fixture.context) == &fixture.registry);

    counting_operation operation;
    CHECK(avis::io::detail::add_operation(fixture.registry, operation));
    CHECK(fixture.registry.size() == 1);
    avis::io::detail::remove_operation(fixture.registry, operation);
    CHECK(fixture.registry.size() == 0);

    fixture.registry.close_and_cancel_all();
    CHECK_FALSE(avis::io::detail::add_operation(fixture.registry, operation));
}
