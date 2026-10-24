// The whole-file operations (include/avis/io/file_operations.h): state machines on top of the async_file read/write
// loop (detail::file_operation, lib/avis/io/detail/file_sender.cpp)

#include "avis/io/file_operations.h"

#include "detail/async_file_state.h"
#include "detail/safe_error.h"

#include "avis/io/async_file.h"
#include "avis/io/detail/file_sender.h"
#include "avis/io/detail/registered_operation.h"
#include "avis/io/error.h"
#include "avis/io/io_context.h"
#include "avis/log.h"
#include "avis/runtime.h"

#include <stdexec/execution.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <atomic>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <windows.h>

namespace avis::io
{
    namespace
    {
        std::atomic<bool> atomic_replace_failure{ false };

        // Makes the temporary file names of concurrent atomic_replace writes in this process unique
        std::atomic<std::uint64_t> temporary_counter{ 0 };

        std::error_code last_error() noexcept
        {
            return std::error_code{ static_cast<int>(::GetLastError()), std::system_category() };
        }

        // Closes the file's handle now instead of when the state is released. Only while no operation runs on the
        // file: on its strand from the last completion, or after the loop has completed.
        void close_now(detail::async_file_state& state) noexcept
        {
            std::error_code ignored;
            state.file.close(ignored);
        }

        std::filesystem::path temporary_path_for(const std::filesystem::path& target)
        {
            std::wstring name = target.native();
            name += std::format(L".{}-{}.tmp", ::GetCurrentProcessId(), temporary_counter.fetch_add(1));
            return name;
        }

        // Renames from over to with POSIX semantics: replaces to even while others have it open (as long as they share
        // delete). MoveFileExW fails with access denied in that case (05 As implemented, 5.5).
        std::error_code posix_rename(const std::filesystem::path& from, const std::filesystem::path& to)
        {
            const HANDLE handle = ::CreateFileW(
                from.c_str(),
                DELETE | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return last_error();
            }

            const std::wstring& target = to.native();
            const std::size_t name_bytes = target.size() * sizeof(wchar_t);
            std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes);
            auto* const information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            information->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            information->RootDirectory = nullptr;
            information->FileNameLength = static_cast<DWORD>(name_bytes);
            std::memcpy(information->FileName, target.data(), name_bytes);

            std::error_code code;
            if (!::SetFileInformationByHandle(
                    handle, FileRenameInfoEx, information, static_cast<DWORD>(storage.size())))
            {
                code = last_error();
            }
            ::CloseHandle(handle);
            return code;
        }

        // Best effort; a failure is logged
        void delete_temporary(const std::filesystem::path& path) noexcept
        {
            if (::DeleteFileW(path.c_str()))
            {
                return;
            }
            const std::error_code code = last_error();
            try
            {
                log::get("io")->warn(
                    "write_file: couldn't delete the temporary file '{}': {}", path.string(), code.message());
            }
            catch (...)
            {
                // Logging is best effort
            }
        }
    } // namespace

    namespace detail
    {
        void inject_atomic_replace_failure(const bool enabled) noexcept
        {
            atomic_replace_failure.store(enabled);
        }

        // Declared as async_file's friend in async_file.h
        struct async_file_access
        {
            // Takes the state over: the async_file is closed afterwards without posting a cancel, and the caller
            // decides when the handle is closed
            static std::shared_ptr<async_file_state> release(async_file& file) noexcept
            {
                return std::move(file.state_);
            }
        };

        // Receives the completion of a whole_file_transfer
        class transfer_owner
        {
        public:
            virtual void transfer_done(result<std::size_t> outcome) noexcept = 0;
            virtual void transfer_stopped() noexcept = 0;

        protected:
            transfer_owner() = default;
            ~transfer_owner() = default;
            transfer_owner(const transfer_owner&) = delete;
            transfer_owner& operator=(const transfer_owner&) = delete;
        };

        // The read or write loop of a whole-file operation: the async_file operation core (registry, strand,
        // cancellation, chunking), with its completion forwarded to the owner
        class whole_file_transfer final : public file_operation
        {
        public:
            explicit whole_file_transfer(transfer_owner& owner) noexcept : owner_{ &owner } {}

            using file_operation::prepare;
            using file_operation::request_cancel;
            using file_operation::submit;

            void complete(result<std::size_t> outcome) noexcept override
            {
                owner_->transfer_done(std::move(outcome)); // may destroy this; nothing may follow
            }

            void complete_stopped() noexcept override
            {
                owner_->transfer_stopped();
            }

        private:
            transfer_owner* owner_;
        };

        // ---------- read_file ----------

        struct read_file_core final : transfer_owner
        {
            explicit read_file_core(read_file_operation& owner) noexcept : owner{ &owner }, transfer{ *this } {}

            // On the strand; the read loop is over
            void transfer_done(result<std::size_t> outcome) noexcept override
            {
                close_now(*state);
                read_file_operation* const target = owner;
                if (!outcome)
                {
                    target->complete(std::unexpected(std::move(outcome.error())));
                    return;
                }
                buffer.resize(*outcome); // a file that shrank while being read; shrinking doesn't allocate
                target->complete(std::move(buffer));
            }

            void transfer_stopped() noexcept override
            {
                close_now(*state);
                owner->complete_stopped();
            }

            read_file_operation* owner;
            std::shared_ptr<async_file_state> state;
            std::vector<std::byte> buffer;
            whole_file_transfer transfer;
        };

        read_file_operation::read_file_operation() noexcept = default;
        read_file_operation::~read_file_operation() = default;

        bool read_file_operation::prepare(io_context& context, const std::filesystem::path& path) noexcept
        {
            std::shared_ptr<async_file_state> state;
            try
            {
                result<async_file> file = async_file::open(context, path, open_mode::read);
                if (!file)
                {
                    complete(std::unexpected(std::move(file.error())));
                    return false;
                }
                state = async_file_access::release(*file);
            }
            catch (...)
            {
                complete(std::unexpected(current_exception_error()));
                return false;
            }

            std::error_code code;
            const std::uint64_t size = state->file.size(code);
            if (code)
            {
                close_now(*state);
                complete(std::unexpected(safe_error(code, "size", path)));
                return false;
            }
            if (size > std::vector<std::byte>{}.max_size())
            {
                close_now(*state);
                complete(std::unexpected(safe_error(error_kind::file_too_large, "read_file: file too large")));
                return false;
            }
            if (size == 0)
            {
                close_now(*state);
                complete(std::vector<std::byte>{});
                return false;
            }

            try
            {
                core_ = std::make_unique<read_file_core>(*this);
                core_->buffer.resize(static_cast<std::size_t>(size));
            }
            catch (...)
            {
                core_.reset();
                close_now(*state);
                complete(std::unexpected(current_exception_error()));
                return false;
            }
            core_->state = state;

            // On failure the transfer has completed this operation already (transfer_done, transfer_stopped)
            return core_->transfer.prepare(
                file_operation_kind::read, std::move(state), 0, core_->buffer.data(), core_->buffer.size());
        }

        void read_file_operation::submit() noexcept
        {
            core_->transfer.submit();
        }

        void read_file_operation::request_cancel() noexcept
        {
            core_->transfer.request_cancel();
        }

        // ---------- write_file ----------

        // Receives the hop to the parallel scheduler for the flush and rename of atomic_replace (07 §3.2)
        struct parallel_receiver
        {
            using receiver_concept = stdexec::receiver_t;

            write_file_core* core;

            void set_value() && noexcept;
            template<class Error>
            void set_error(Error&& error) && noexcept;
            void set_stopped() && noexcept;

            stdexec::env<> get_env() const noexcept
            {
                return {};
            }
        };

        using parallel_operation = decltype(stdexec::connect(
            stdexec::schedule(std::declval<stdexec::parallel_scheduler>()), std::declval<parallel_receiver>()));

        // Registered with the operation registry for atomic_replace only, which runs outside the transfer (flush and
        // rename on a parallel worker); the other policies are covered by the transfer's own registration
        struct write_file_core final : transfer_owner, registered_operation
        {
            write_file_core(write_file_operation& owner, const write_policy policy, std::filesystem::path target) :
                owner{ &owner },
                policy{ policy },
                target{ std::move(target) },
                transfer{ *this }
            {}

            // On the strand, or inline from submit() when there is nothing to write
            void transfer_done(result<std::size_t> outcome) noexcept override
            {
                if (!outcome)
                {
                    fail(std::move(outcome.error()));
                    return;
                }
                if (policy != write_policy::atomic_replace)
                {
                    close_now(*state);
                    succeed();
                    return;
                }
                continue_on_parallel();
            }

            void transfer_stopped() noexcept override
            {
                fail_stopped();
            }

            // Registry, at shutdown: only posts (through the transfer), never completes inline
            void cancel() noexcept override
            {
                request_cancel();
            }

            // Stop callback or registry, any thread. The mutex orders reading the transfer's state with prepare().
            void request_cancel() noexcept
            {
                cancel_requested.store(true);
                const std::lock_guard lock{ mutex };
                if (transfer_started)
                {
                    transfer.request_cancel();
                }
            }

            // After the transfer's prepare() succeeded; forwards a cancel that came earlier
            void mark_transfer_started() noexcept
            {
                const std::lock_guard lock{ mutex };
                transfer_started = true;
                if (cancel_requested.load())
                {
                    transfer.request_cancel();
                }
            }

            void continue_on_parallel() noexcept
            {
                try
                {
                    parallel = std::unique_ptr<parallel_operation>{ new parallel_operation{ stdexec::connect(
                        stdexec::schedule(stdexec::get_parallel_scheduler()), parallel_receiver{ this }) } };
                }
                catch (...)
                {
                    fail(current_exception_error());
                    return;
                }
                stdexec::start(*parallel);
            }

            // On a parallel worker: flush, close, rename over the target
            void on_parallel() noexcept
            {
                if (cancel_requested.load())
                {
                    fail_stopped();
                    return;
                }

                std::error_code code;
                state->file.sync_all(code);
                if (code)
                {
                    fail(safe_error(code, "flush", temporary));
                    return;
                }
                close_now(*state); // the rename needs a delete-sharing handle, and nothing may keep the target open

                if (atomic_replace_failure.load())
                {
                    fail(safe_error(error_kind::system_error, "injected failure"));
                    return;
                }

                try
                {
                    code = posix_rename(temporary, target);
                }
                catch (...)
                {
                    fail(current_exception_error());
                    return;
                }
                if (code)
                {
                    fail(safe_error(code, "replace", target));
                    return;
                }
                succeed();
            }

            void unregister() noexcept
            {
                if (registered)
                {
                    remove_operation(*registry, *this);
                    registered = false;
                }
            }

            // Closes and removes the temporary file (atomic_replace), then completes
            void clean_up() noexcept
            {
                if (state)
                {
                    close_now(*state);
                }
                if (!temporary.empty() && state)
                {
                    delete_temporary(temporary);
                }
                unregister();
            }

            void succeed() noexcept
            {
                unregister();
                owner->complete({});
            }

            void fail(avis::error error) noexcept
            {
                clean_up();
                owner->complete(std::unexpected(std::move(error)));
            }

            void fail_stopped() noexcept
            {
                clean_up();
                owner->complete_stopped();
            }

            write_file_operation* owner;
            write_policy policy;
            std::filesystem::path target;
            std::filesystem::path temporary; // atomic_replace only
            std::shared_ptr<async_file_state> state; // set once the target or temporary file is open
            operation_registry* registry = nullptr;
            bool registered = false;
            bool nothing_to_write = false;

            std::mutex mutex;
            bool transfer_started = false; // guarded by mutex
            std::atomic<bool> cancel_requested{ false };

            whole_file_transfer transfer;
            std::unique_ptr<parallel_operation> parallel;
        };

        void parallel_receiver::set_value() && noexcept
        {
            core->on_parallel();
        }

        template<class Error>
        void parallel_receiver::set_error(Error&& error) && noexcept
        {
            if constexpr (std::is_same_v<std::remove_cvref_t<Error>, std::exception_ptr>)
            {
                try
                {
                    std::rethrow_exception(error);
                }
                catch (...)
                {
                    core->fail(current_exception_error());
                }
            }
            else
            {
                core->fail(safe_error(error_kind::system_error, "write_file: the parallel scheduler failed"));
            }
        }

        void parallel_receiver::set_stopped() && noexcept
        {
            core->fail_stopped();
        }

        write_file_operation::write_file_operation() noexcept = default;
        write_file_operation::~write_file_operation() = default;

        bool write_file_operation::prepare(
            io_context& context,
            const std::filesystem::path& path,
            const std::span<const std::byte> data,
            const write_policy policy) noexcept
        {
            const bool atomic = policy == write_policy::atomic_replace;
            const open_flags mode = policy == write_policy::truncate ?
                                        open_mode::write | open_mode::create | open_mode::truncate :
                                        open_mode::write | open_mode::create | open_mode::exclusive;
            try
            {
                core_ = std::make_unique<write_file_core>(*this, policy, path);
                if (atomic)
                {
                    core_->temporary = temporary_path_for(path);
                }

                result<async_file> file = async_file::open(context, atomic ? core_->temporary : path, mode);
                if (!file)
                {
                    core_.reset(); // nothing of ours to clean up: the file wasn't created
                    complete(std::unexpected(std::move(file.error())));
                    return false;
                }
                core_->state = async_file_access::release(*file);
            }
            catch (...)
            {
                core_.reset();
                complete(std::unexpected(current_exception_error()));
                return false;
            }
            core_->registry = core_->state->registry;

            if (atomic && core_->registry != nullptr)
            {
                if (!add_operation(*core_->registry, *core_))
                {
                    core_->fail_stopped(); // the context is stopping
                    return false;
                }
                core_->registered = true;
            }

            if (data.empty())
            {
                core_->nothing_to_write = true;
                return true;
            }

            // The write path only reads through the pointer. On failure the transfer has completed this operation
            // already (transfer_done, transfer_stopped).
            if (!core_->transfer.prepare(
                    file_operation_kind::write, core_->state, 0, const_cast<std::byte*>(data.data()), data.size()))
            {
                return false;
            }
            core_->mark_transfer_started();
            return true;
        }

        void write_file_operation::submit() noexcept
        {
            if (core_->nothing_to_write)
            {
                core_->transfer_done(std::size_t{ 0 });
                return;
            }
            core_->transfer.submit();
        }

        void write_file_operation::request_cancel() noexcept
        {
            core_->request_cancel();
        }
    } // namespace detail

    read_file_sender read_file(const std::filesystem::path& path)
    {
        return read_file(runtime::io_context(), path);
    }

    read_file_sender read_file(io_context& context, const std::filesystem::path& path)
    {
        return read_file_sender{ context, path };
    }

    write_file_sender
        write_file(const std::filesystem::path& path, const std::span<const std::byte> data, const write_policy policy)
    {
        return write_file(runtime::io_context(), path, data, policy);
    }

    write_file_sender write_file(
        io_context& context,
        const std::filesystem::path& path,
        const std::span<const std::byte> data,
        const write_policy policy)
    {
        return write_file_sender{ context, path, {}, data, policy };
    }

    write_file_sender
        write_file(const std::filesystem::path& path, std::vector<std::byte> data, const write_policy policy)
    {
        return write_file(runtime::io_context(), path, std::move(data), policy);
    }

    write_file_sender write_file(
        io_context& context, const std::filesystem::path& path, std::vector<std::byte> data, const write_policy policy)
    {
        const std::span<const std::byte> view{ data };
        return write_file_sender{ context, path, std::move(data), view, policy };
    }

    result<std::uint64_t> file_size(const std::filesystem::path& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        {
            return std::unexpected(make_error(last_error(), "file_size", path));
        }
        return (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    }
} // namespace avis::io
