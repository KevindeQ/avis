// The read/write operations behind async_file::read_at and write_at (include/avis/io/detail/file_sender.h)

#include "avis/io/detail/file_sender.h"

#include "async_file_state.h"
#include "safe_error.h"

#include "avis/io/error.h"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace avis::io
{
    namespace detail
    {
        // Declared in avis/io/detail/file_sender.h; only the operations below use it
        struct file_operation_core
        {
            file_operation_core(
                file_operation* const owner,
                const file_operation_kind kind,
                std::shared_ptr<async_file_state> state,
                const std::uint64_t offset,
                std::byte* const data,
                const std::size_t size) :
                owner{ owner },
                kind{ kind },
                state{ std::move(state) },
                offset{ offset },
                data{ data },
                size{ size }
            {}

            file_operation* owner; // valid until the operation completes; nullptr afterwards
            file_operation_kind kind;
            std::shared_ptr<async_file_state> state;
            std::uint64_t offset;
            std::byte* data;
            std::size_t size;
            std::size_t done = 0;
            asio::cancellation_signal signal;
            bool registered = false;
            bool completed = false; // only read and written on the strand
            std::atomic<bool> requested{ false };
        };
    } // namespace detail

    namespace
    {
        using detail::current_exception_error;
        using detail::safe_error;

        // asio passes each buffer size to ReadFile/WriteFile as a DWORD; larger requests are split by the loop
        constexpr std::size_t maximum_chunk = std::size_t{ 1 } << 30;

        std::string_view operation_name(const detail::file_operation_kind kind) noexcept
        {
            return kind == detail::file_operation_kind::read ? "read" : "write";
        }

        // Completes the operation once: unregisters it, then hands the outcome to its receiver. On the strand.
        void finish(const std::shared_ptr<detail::file_operation_core>& core, result<std::size_t> outcome) noexcept
        {
            core->completed = true;
            if (core->registered)
            {
                detail::remove_operation(*core->state->registry, *core->owner);
                core->registered = false;
            }
            detail::file_operation* const owner = std::exchange(core->owner, nullptr);
            owner->complete(std::move(outcome));
        }

        void finish_stopped(const std::shared_ptr<detail::file_operation_core>& core) noexcept
        {
            core->completed = true;
            if (core->registered)
            {
                detail::remove_operation(*core->state->registry, *core->owner);
                core->registered = false;
            }
            detail::file_operation* const owner = std::exchange(core->owner, nullptr);
            owner->complete_stopped();
        }

        void issue_next(const std::shared_ptr<detail::file_operation_core>& core) noexcept;

        // Completion of one chunk, on the strand
        void on_chunk(
            const std::shared_ptr<detail::file_operation_core>& core,
            const std::error_code& code,
            const std::size_t transferred) noexcept
        {
            const bool reading = core->kind == detail::file_operation_kind::read;
            if (code == asio::error::operation_aborted)
            {
                finish_stopped(core);
                return;
            }
            if (reading && code == asio::error::eof)
            {
                finish(core, core->done);
                return;
            }
            if (code)
            {
                finish(core, std::unexpected(safe_error(code, operation_name(core->kind), core->state->path)));
                return;
            }

            core->done += transferred;
            if (core->done == core->size)
            {
                finish(core, core->done);
                return;
            }
            if (transferred == 0)
            {
                if (reading)
                {
                    finish(core, core->done); // end of file without an eof error
                }
                else
                {
                    finish(core, std::unexpected(safe_error(error_kind::short_write, "write transferred no bytes")));
                }
                return;
            }
            issue_next(core);
        }

        // Starts the next chunk, on the strand
        void issue_next(const std::shared_ptr<detail::file_operation_core>& core) noexcept
        {
            if (core->state->closed || core->requested.load())
            {
                finish_stopped(core);
                return;
            }

            const std::size_t chunk = std::min(core->size - core->done, maximum_chunk);
            const std::uint64_t position = core->offset + core->done;
            auto handler = asio::bind_cancellation_slot(
                core->signal.slot(),
                asio::bind_executor(
                    core->state->strand,
                    [core](const std::error_code& code, const std::size_t transferred)
                    { on_chunk(core, code, transferred); }));

            try
            {
                if (core->kind == detail::file_operation_kind::read)
                {
                    core->state->file.async_read_some_at(
                        position, asio::buffer(core->data + core->done, chunk), std::move(handler));
                }
                else
                {
                    core->state->file.async_write_some_at(
                        position,
                        asio::buffer(static_cast<const void*>(core->data + core->done), chunk),
                        std::move(handler));
                }
            }
            catch (...)
            {
                finish(core, std::unexpected(current_exception_error()));
            }
        }
    } // namespace

    namespace detail
    {
        file_operation::file_operation() noexcept = default;
        file_operation::~file_operation() = default;

        bool file_operation::prepare(
            const file_operation_kind kind,
            std::shared_ptr<async_file_state> state,
            const std::uint64_t offset,
            std::byte* const data,
            const std::size_t size) noexcept
        {
            if (!state)
            {
                complete(std::unexpected(safe_error(error_kind::not_open, "operation on a closed file")));
                return false;
            }
            if (size > std::numeric_limits<std::uint64_t>::max() - offset)
            {
                complete(std::unexpected(safe_error(error_kind::invalid_argument, "offset + size overflows")));
                return false;
            }
            if (size == 0)
            {
                complete(std::size_t{ 0 });
                return false;
            }

            operation_registry* const registry = state->registry;
            try
            {
                core_ = std::make_shared<file_operation_core>(this, kind, std::move(state), offset, data, size);
            }
            catch (...)
            {
                complete(std::unexpected(current_exception_error()));
                return false;
            }

            // core_ is set before the registry can call cancel()
            if (registry != nullptr)
            {
                if (!add_operation(*registry, *this))
                {
                    core_->owner = nullptr;
                    complete_stopped(); // the context is stopping
                    return false;
                }
                core_->registered = true;
            }
            return true;
        }

        void file_operation::submit() noexcept
        {
            std::shared_ptr<file_operation_core> core = core_;
            try
            {
                asio::post(core->state->strand, [core]() { issue_next(core); });
            }
            catch (...)
            {
                finish(core, std::unexpected(current_exception_error()));
            }
        }

        void file_operation::request_cancel() noexcept
        {
            // core_ is set before the stop callback and the registry can call this, and never reset afterwards
            std::shared_ptr<file_operation_core> core = core_;
            if (!core)
            {
                return;
            }

            core->requested.store(true);
            try
            {
                asio::post(
                    core->state->strand,
                    [core]()
                    {
                        if (!core->completed)
                        {
                            core->signal.emit(asio::cancellation_type::total);
                        }
                    });
            }
            catch (...)
            {
                // Allocation failure: a chunk not yet started still sees `requested`
            }
        }

        void file_operation::cancel() noexcept
        {
            request_cancel();
        }
    } // namespace detail
} // namespace avis::io
