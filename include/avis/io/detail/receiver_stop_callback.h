#ifndef AVIS_IO_DETAIL_RECEIVER_STOP_CALLBACK_H
#define AVIS_IO_DETAIL_RECEIVER_STOP_CALLBACK_H

// The optional stop callback that the IO operation states install on their receiver's stop token. Holds nothing when
// the token can never stop (unstoppable_token), so those receivers pay for no callback.

#include <stdexec/execution.hpp>

#include <optional>
#include <utility>

namespace avis::io::detail
{
    template<class Receiver, class Callback>
    class receiver_stop_callback
    {
    public:
        using stop_token_type = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
        static constexpr bool stoppable = !stdexec::unstoppable_token<stop_token_type>;

        static bool stop_requested(const Receiver& receiver) noexcept
        {
            if constexpr (stoppable)
            {
                return stdexec::get_stop_token(stdexec::get_env(receiver)).stop_requested();
            }
            else
            {
                return false;
            }
        }

        // Runs callback inline when a stop was already requested
        void install(const Receiver& receiver, Callback callback) noexcept
        {
            if constexpr (stoppable)
            {
                callback_.emplace(stdexec::get_stop_token(stdexec::get_env(receiver)), std::move(callback));
            }
        }

        // Waits for a callback that is running on another thread
        void reset() noexcept
        {
            if constexpr (stoppable)
            {
                callback_.reset();
            }
        }

    private:
        // stop_callback_for_t is only named for tokens that can stop
        template<bool Stoppable, class = void>
        struct storage
        {
            using type = std::optional<int>;
        };

        template<class Unused>
        struct storage<true, Unused>
        {
            using type = std::optional<stdexec::stop_callback_for_t<stop_token_type, Callback>>;
        };

        typename storage<stoppable>::type callback_;
    };
} // namespace avis::io::detail

#endif
