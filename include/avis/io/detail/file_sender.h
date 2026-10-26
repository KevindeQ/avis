#ifndef AVIS_IO_DETAIL_FILE_SENDER_H
#define AVIS_IO_DETAIL_FILE_SENDER_H

// The sender and operation state behind async_file::read_at and write_at (docs/design/07-io-layer.md §4.4).
// Only the templates that depend on the receiver live here. file_operation and the read/write loop are
// implemented in lib/avis/io/detail/file_sender.cpp; the file's state (asio file, strand) is in the private
// lib/avis/io/detail/async_file_state.h. Free of asio and <windows.h>.

#include "avis/detail/receiver_stop_callback.h"
#include "avis/error.h"
#include "avis/io/detail/registered_operation.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace avis::io
{
    class async_file;

    namespace detail
    {
        // Owns the asio::random_access_file and the file's strand; closes the handle in its destructor, once the
        // async_file and every operation have released it (07 §3.1). Defined in lib/avis/io/detail/async_file_state.h.
        class async_file_state;

        // Access to async_file's state for the whole-file operations. Defined in lib/avis/io/file_operations.cpp.
        struct async_file_access;

        // The shared part of one read or write: progress, cancellation signal, flags. Defined in
        // lib/avis/io/detail/file_sender.cpp.
        struct file_operation_core;

        enum class file_operation_kind : std::uint8_t
        {
            read,
            write
        };

        // Non-template part of a read or write operation; the asio calls live in lib/avis/io/detail/file_sender.cpp
        class file_operation : public registered_operation
        {
        public:
            // Called once, on the file's strand or synchronously from prepare()
            virtual void complete(result<std::size_t> outcome) noexcept = 0;
            virtual void complete_stopped() noexcept = 0;

        protected:
            file_operation() noexcept;
            ~file_operation();
            file_operation(const file_operation&) = delete;
            file_operation& operator=(const file_operation&) = delete;

            // Validates and registers the operation. On failure it has already completed (synchronously) and returns
            // false. Then the caller installs its stop callback and calls submit().
            bool prepare(
                file_operation_kind kind,
                std::shared_ptr<async_file_state> state,
                std::uint64_t offset,
                std::byte* data,
                std::size_t size) noexcept;

            // Posts the first chunk to the file's strand. The operation may complete (and be destroyed) before
            // submit() returns, so the caller must not touch it afterwards.
            void submit() noexcept;

            // From the stop callback, on any thread: posts a cancellation of this operation to the file's strand
            void request_cancel() noexcept;

        private:
            void cancel() noexcept override; // registry cancellation at shutdown

            std::shared_ptr<file_operation_core> core_;
        };

        template<class Receiver>
        class file_operation_state final : public file_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            file_operation_state(
                Receiver receiver,
                const file_operation_kind kind,
                std::shared_ptr<async_file_state> state,
                const std::uint64_t offset,
                std::byte* const data,
                const std::size_t size) :
                receiver_{ std::move(receiver) },
                kind_{ kind },
                state_{ std::move(state) },
                offset_{ offset },
                data_{ data },
                size_{ size }
            {}

            void start() & noexcept
            {
                if (stop_callback::stop_requested(receiver_))
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }

                if (!prepare(kind_, std::move(state_), offset_, data_, size_))
                {
                    return;
                }
                stop_callback_.install(receiver_, stop_function{ this });
                submit();
            }

            void complete(result<std::size_t> outcome) noexcept override
            {
                stop_callback_.reset();
                if (outcome)
                {
                    stdexec::set_value(std::move(receiver_), *outcome);
                }
                else
                {
                    stdexec::set_error(std::move(receiver_), std::move(outcome.error()));
                }
            }

            void complete_stopped() noexcept override
            {
                stop_callback_.reset();
                stdexec::set_stopped(std::move(receiver_));
            }

        private:
            struct stop_function
            {
                file_operation_state* self;

                void operator()() const noexcept
                {
                    self->request_cancel();
                }
            };

            using stop_callback = avis::detail::receiver_stop_callback<Receiver, stop_function>;

            Receiver receiver_;
            file_operation_kind kind_;
            std::shared_ptr<async_file_state> state_;
            std::uint64_t offset_;
            std::byte* data_;
            std::size_t size_;
            stop_callback stop_callback_;
        };

        template<file_operation_kind Kind>
        class file_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = avis::completions<std::size_t>;

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) && -> file_operation_state<Receiver>
            {
                return { std::move(receiver), Kind, std::move(state_), offset_, data_, size_ };
            }

        private:
            friend class io::async_file;

            file_sender(
                std::shared_ptr<async_file_state> state,
                const std::uint64_t offset,
                std::byte* const data,
                const std::size_t size) noexcept :
                state_{ std::move(state) },
                offset_{ offset },
                data_{ data },
                size_{ size }
            {}

            std::shared_ptr<async_file_state> state_;
            std::uint64_t offset_;
            std::byte* data_;
            std::size_t size_;
        };
    } // namespace detail
} // namespace avis::io

#endif
