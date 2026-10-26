#ifndef AVIS_IO_FILE_OPERATIONS_H
#define AVIS_IO_FILE_OPERATIONS_H

#include "avis/detail/receiver_stop_callback.h"
#include "avis/error.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>
#include <vector>

// Whole-file reads and writes (docs/design/07-io-layer.md §4.5). No asio and no <windows.h> here: the state machines
// (open → size → read loop; open → write loop → flush → rename) are in lib/avis/io/file_operations.cpp, on top of the
// async_file read/write loop.

namespace avis::io
{
    class io_context;

    enum class write_policy : std::uint8_t
    {
        atomic_replace, // write <path>.<pid>-<counter>.tmp, flush, POSIX-semantics rename over <path>
        truncate, // open with create | truncate and write in place
        create_new // open with create | exclusive; fail with error_kind::already_exists
    };

    class read_file_sender;
    class write_file_sender;

    // set_value(std::vector<std::byte>) with the whole file. The buffer is sized from the file's size at open time.
    // Completes on an IO thread, or inline in start() for an empty file and for errors found before reading.
    read_file_sender read_file(const std::filesystem::path& path); // avis::runtime::io_context()
    read_file_sender read_file(io_context& context, const std::filesystem::path& path);

    // set_value(). data must outlive the operation. atomic_replace completes on a parallel worker, the other policies
    // on an IO thread (or inline in start() for errors found before writing and for empty data).
    write_file_sender write_file(
        const std::filesystem::path& path,
        std::span<const std::byte> data,
        write_policy policy = write_policy::atomic_replace);
    write_file_sender write_file(
        io_context& context,
        const std::filesystem::path& path,
        std::span<const std::byte> data,
        write_policy policy = write_policy::atomic_replace);

    // Owning variants for fire-and-forget writers (screenshots, autosave): the sender keeps data alive. An lvalue
    // vector is copied; move it in to avoid the copy.
    write_file_sender write_file(
        const std::filesystem::path& path,
        std::vector<std::byte> data,
        write_policy policy = write_policy::atomic_replace);
    write_file_sender write_file(
        io_context& context,
        const std::filesystem::path& path,
        std::vector<std::byte> data,
        write_policy policy = write_policy::atomic_replace);

    // Synchronous metadata query (GetFileAttributesExW); no handle kept open
    result<std::uint64_t> file_size(const std::filesystem::path& path);

    namespace detail
    {
        // Test hook: while enabled, atomic_replace fails after flushing and closing the temporary file and before the
        // rename, so tests can check that the target is untouched and the temporary file was removed (07 §7)
        void inject_atomic_replace_failure(bool enabled) noexcept;

        // The state of one read_file or write_file. Defined in lib/avis/io/file_operations.cpp.
        struct read_file_core;
        struct write_file_core;

        // Non-template part of a read_file operation
        class read_file_operation
        {
        public:
            // Called once: inline from prepare(), or on an IO thread
            virtual void complete(result<std::vector<std::byte>> outcome) noexcept = 0;
            virtual void complete_stopped() noexcept = 0;

        protected:
            read_file_operation() noexcept;
            ~read_file_operation();
            read_file_operation(const read_file_operation&) = delete;
            read_file_operation& operator=(const read_file_operation&) = delete;

            // Opens the file, sizes the buffer and registers the read. Returns false when it has already completed
            // (an error, or an empty file). Then the caller installs its stop callback and calls submit().
            bool prepare(io_context& context, const std::filesystem::path& path) noexcept;

            // Starts the read loop. The operation may complete (and be destroyed) before submit() returns.
            void submit() noexcept;

            // From the stop callback, on any thread
            void request_cancel() noexcept;

        private:
            std::unique_ptr<read_file_core> core_;
        };

        // Non-template part of a write_file operation, one state machine for the three policies
        class write_file_operation
        {
        public:
            // Called once: inline from prepare() or submit(), on an IO thread, or on a parallel worker
            virtual void complete(result<void> outcome) noexcept = 0;
            virtual void complete_stopped() noexcept = 0;

        protected:
            write_file_operation() noexcept;
            ~write_file_operation();
            write_file_operation(const write_file_operation&) = delete;
            write_file_operation& operator=(const write_file_operation&) = delete;

            // Opens the target (or the temporary file) and registers the write. Returns false when it has already
            // completed. Then the caller installs its stop callback and calls submit().
            bool prepare(
                io_context& context,
                const std::filesystem::path& path,
                std::span<const std::byte> data,
                write_policy policy) noexcept;

            // Starts the write loop. The operation may complete (and be destroyed) before submit() returns.
            void submit() noexcept;

            // From the stop callback, on any thread
            void request_cancel() noexcept;

        private:
            std::unique_ptr<write_file_core> core_;
        };

        template<class Receiver>
        class read_file_operation_state final : public read_file_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            read_file_operation_state(Receiver receiver, io_context& context, std::filesystem::path path) :
                receiver_{ std::move(receiver) },
                context_{ &context },
                path_{ std::move(path) }
            {}

            void start() & noexcept
            {
                if (stop_callback::stop_requested(receiver_))
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                if (!prepare(*context_, path_))
                {
                    return;
                }
                stop_callback_.install(receiver_, stop_function{ this });
                submit();
            }

            void complete(result<std::vector<std::byte>> outcome) noexcept override
            {
                stop_callback_.reset();
                if (outcome)
                {
                    stdexec::set_value(std::move(receiver_), std::move(*outcome));
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
                read_file_operation_state* self;

                void operator()() const noexcept
                {
                    self->request_cancel();
                }
            };

            using stop_callback = avis::detail::receiver_stop_callback<Receiver, stop_function>;

            Receiver receiver_;
            io_context* context_;
            std::filesystem::path path_;
            stop_callback stop_callback_;
        };

        template<class Receiver>
        class write_file_operation_state final : public write_file_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            write_file_operation_state(
                Receiver receiver,
                io_context& context,
                std::filesystem::path path,
                std::vector<std::byte> storage,
                const std::span<const std::byte> data,
                const write_policy policy) :
                receiver_{ std::move(receiver) },
                context_{ &context },
                path_{ std::move(path) },
                storage_{ std::move(storage) }, // moving a vector keeps its buffer, so data stays valid
                data_{ data },
                policy_{ policy }
            {}

            void start() & noexcept
            {
                if (stop_callback::stop_requested(receiver_))
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                if (!prepare(*context_, path_, data_, policy_))
                {
                    return;
                }
                stop_callback_.install(receiver_, stop_function{ this });
                submit();
            }

            void complete(result<void> outcome) noexcept override
            {
                stop_callback_.reset();
                if (outcome)
                {
                    stdexec::set_value(std::move(receiver_));
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
                write_file_operation_state* self;

                void operator()() const noexcept
                {
                    self->request_cancel();
                }
            };

            using stop_callback = avis::detail::receiver_stop_callback<Receiver, stop_function>;

            Receiver receiver_;
            io_context* context_;
            std::filesystem::path path_;
            std::vector<std::byte> storage_; // the owning overloads' data; empty otherwise
            std::span<const std::byte> data_;
            write_policy policy_;
            stop_callback stop_callback_;
        };
    } // namespace detail

    class read_file_sender
    {
    public:
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = avis::completions<std::vector<std::byte>>;

        template<stdexec::receiver Receiver>
        auto connect(Receiver receiver) && -> detail::read_file_operation_state<Receiver>
        {
            return { std::move(receiver), *context_, std::move(path_) };
        }

    private:
        friend read_file_sender read_file(io_context& context, const std::filesystem::path& path);

        read_file_sender(io_context& context, std::filesystem::path path) :
            context_{ &context },
            path_{ std::move(path) }
        {}

        io_context* context_;
        std::filesystem::path path_;
    };

    class write_file_sender
    {
    public:
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = avis::completions<>;

        // Move-only: data may point into storage, which a move keeps and a copy wouldn't
        write_file_sender(write_file_sender&&) noexcept = default;
        write_file_sender& operator=(write_file_sender&&) noexcept = default;
        write_file_sender(const write_file_sender&) = delete;
        write_file_sender& operator=(const write_file_sender&) = delete;

        template<stdexec::receiver Receiver>
        auto connect(Receiver receiver) && -> detail::write_file_operation_state<Receiver>
        {
            return { std::move(receiver), *context_, std::move(path_), std::move(storage_), data_, policy_ };
        }

    private:
        friend write_file_sender write_file(
            io_context& context,
            const std::filesystem::path& path,
            std::span<const std::byte> data,
            write_policy policy);
        friend write_file_sender write_file(
            io_context& context, const std::filesystem::path& path, std::vector<std::byte> data, write_policy policy);

        write_file_sender(
            io_context& context,
            std::filesystem::path path,
            std::vector<std::byte> storage,
            const std::span<const std::byte> data,
            const write_policy policy) :
            context_{ &context },
            path_{ std::move(path) },
            storage_{ std::move(storage) },
            data_{ data },
            policy_{ policy }
        {}

        io_context* context_;
        std::filesystem::path path_;
        std::vector<std::byte> storage_; // the owning overloads' data; empty otherwise
        std::span<const std::byte> data_;
        write_policy policy_;
    };
} // namespace avis::io

#endif
