#include <catch2/catch_test_macros.hpp>

#include <coroutine>

class observable
{
public:
    struct promise_type
    {
    public:
        observable get_return_object()
        {
            return { };
        }

        std::suspend_never initial_suspend() noexcept
        {
            return {};
        }

        std::suspend_never final_suspend() noexcept
        {
            return {};
        }

        void return_void() {}

        void unhandled_exception() {}
    };

    void subscribe();
};

template<typename type>
class observer
{
public:
    using value_type = type;

    void on_next(const value_type& v) const noexcept;
    void on_next(value_type&& v) const noexcept;

    void on_error(const std::exception_ptr& err) const noexcept;

    void on_completed() const noexcept;
};

observable test_coro()
{
    co_return;
}

TEST_CASE("observables poc", "[observables]")
{
    auto obs = test_coro();
}
