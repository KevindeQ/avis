#ifndef AVIS_SENDER_OF_H
#define AVIS_SENDER_OF_H

#include "avis/error.h"

#include <stdexec/execution.hpp>

#include <type_traits>

// The completion signatures every Avis sender uses (docs/design/07-io-layer.md §4.1). There's no type-erased sender:
// if a consumer ever needs one, add the any_sender_of alias described in 07 §4.1 here.

namespace avis
{
    // One value channel, avis::error, stopped
    template<typename... Values>
    using completions = stdexec::completion_signatures<
        stdexec::set_value_t(Values...),
        stdexec::set_error_t(avis::error),
        stdexec::set_stopped_t()>;

    namespace detail
    {
        template<typename Signature, typename Signatures>
        struct contains_signature : std::false_type
        {};

        template<typename Signature, typename... Signatures>
        struct contains_signature<Signature, stdexec::completion_signatures<Signatures...>> :
            std::bool_constant<(std::is_same_v<Signature, Signatures> || ...)>
        {};

        // True when both lists hold the same signatures, in any order
        template<typename Left, typename Right>
        struct same_signature_set : std::false_type
        {};

        template<typename... Left, typename... Right>
        struct same_signature_set<stdexec::completion_signatures<Left...>, stdexec::completion_signatures<Right...>> :
            std::bool_constant<
                sizeof...(Left) == sizeof...(Right) &&
                (contains_signature<Left, stdexec::completion_signatures<Right...>>::value && ...) &&
                (contains_signature<Right, stdexec::completion_signatures<Left...>>::value && ...)>
        {};
    } // namespace detail

    // True when Sender completes with exactly completions<Values...>, in any order. sender_of<S> (no values) is the
    // "sender_of<void>" of the design documents.
    template<typename Sender, typename... Values>
    concept sender_of =
        stdexec::sender_in<Sender, stdexec::env<>> && detail::same_signature_set<
                                                          stdexec::completion_signatures_of_t<Sender, stdexec::env<>>,
                                                          completions<Values...>>::value;
} // namespace avis

#endif
