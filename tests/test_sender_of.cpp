#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>

namespace
{
    // A sender that only declares its completion signatures; avis::sender_of looks at nothing else
    template<typename Signatures>
    struct declared_sender
    {
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = Signatures;
    };

    using reordered = stdexec::
        completion_signatures<stdexec::set_stopped_t(), stdexec::set_error_t(avis::error), stdexec::set_value_t(int)>;

    using without_stopped =
        stdexec::completion_signatures<stdexec::set_value_t(int), stdexec::set_error_t(avis::error)>;

    using with_exception_ptr = stdexec::completion_signatures<
        stdexec::set_value_t(int),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    using two_value_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(int),
        stdexec::set_value_t(int, int),
        stdexec::set_error_t(avis::error),
        stdexec::set_stopped_t()>;
} // namespace

// Senders with exactly avis::completions<Values...>
static_assert(avis::sender_of<declared_sender<avis::completions<int>>, int>);
static_assert(avis::sender_of<declared_sender<avis::completions<>>>); // the "sender_of<void>" of the designs
static_assert(avis::sender_of<declared_sender<avis::completions<int, double>>, int, double>);
static_assert(avis::sender_of<declared_sender<reordered>, int>); // order doesn't matter

// Wrong value types
static_assert(!avis::sender_of<declared_sender<avis::completions<int>>, long>);
static_assert(!avis::sender_of<declared_sender<avis::completions<int>>>);
static_assert(!avis::sender_of<declared_sender<avis::completions<>>, int>);
static_assert(!avis::sender_of<declared_sender<two_value_signatures>, int>); // an extra value signature

// Missing or foreign channels
static_assert(!avis::sender_of<declared_sender<without_stopped>, int>);
static_assert(!avis::sender_of<declared_sender<with_exception_ptr>, int>);

// Not a sender at all
static_assert(!avis::sender_of<int, int>);

TEST_CASE("sender_of rejects stdexec senders that lack the Avis channels", "[sender_of]")
{
    // just has no error or stopped channel; then adds set_error(std::exception_ptr)
    using just_sender = decltype(stdexec::just(1));
    using then_sender = decltype(stdexec::just(1) | stdexec::then([](int value) { return value; }));

    STATIC_CHECK_FALSE(avis::sender_of<just_sender, int>);
    STATIC_CHECK_FALSE(avis::sender_of<then_sender, int>);
}
