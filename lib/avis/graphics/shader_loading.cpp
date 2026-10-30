#include "avis/graphics/shader_loading.h"

#include "avis/graphics/error.h"
#include "avis/io/file_operations.h"
#include "avis/runtime.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace avis::graphics
{
    namespace detail
    {
        namespace
        {
            std::string utf8(const std::filesystem::path& path)
            {
                const std::u8string text = path.u8string();
                return std::string{ text.begin(), text.end() };
            }

            // A blob path must stay inside the blob directory: relative, without a ".." element
            std::optional<avis::error> check_blob_path(const shader_manifest_entry& entry)
            {
                const auto fail = [&](const std::string_view reason)
                {
                    return make_error(
                        error_kind::invalid_argument,
                        std::format("shader '{}': blob path '{}' {}", entry.id, utf8(entry.blob), reason));
                };
                if (entry.blob.empty())
                {
                    return make_error(
                        error_kind::invalid_argument, std::format("shader '{}': empty blob path", entry.id));
                }
                if (entry.blob.has_root_path())
                {
                    return fail("isn't relative to the shader directory");
                }
                for (const std::filesystem::path& element : entry.blob)
                {
                    if (element == "..")
                    {
                        return fail("leaves the shader directory");
                    }
                }
                return std::nullopt;
            }
        } // namespace

        // Receives one blob read
        struct shader_load_read_receiver
        {
            using receiver_concept = stdexec::receiver_tag;

            void set_value(std::vector<std::byte> bytes) noexcept;
            void set_error(avis::error error) noexcept;
            void set_stopped() noexcept;

            auto get_env() const noexcept
            {
                return stdexec::prop{ stdexec::get_stop_token, token };
            }

            shader_library_load_core* core;
            std::size_t index;
            stdexec::inplace_stop_token token;
        };

        // Receives the hop to the parallel scheduler once every read completed. No stop token: the hop always runs,
        // and the continuation decides how the load completes.
        struct shader_load_finish_receiver
        {
            using receiver_concept = stdexec::receiver_tag;

            void set_value() noexcept;
            template<class Error>
            void set_error(Error&& error) noexcept;
            void set_stopped() noexcept;

            stdexec::env<> get_env() const noexcept
            {
                return {};
            }

            shader_library_load_core* core;
        };

        using shader_load_read_operation =
            decltype(stdexec::connect(std::declval<io::read_file_sender>(), std::declval<shader_load_read_receiver>()));
        using shader_load_finish_operation = decltype(stdexec::connect(
            stdexec::schedule(std::declval<stdexec::parallel_scheduler>()),
            std::declval<shader_load_finish_receiver>()));

        struct shader_library_load_core
        {
            shader_library_load_core(
                shader_library_load_operation& owner,
                io::io_context& context,
                shader_manifest manifest,
                std::filesystem::path directory) :
                owner{ &owner },
                manifest{ std::move(manifest) },
                directory{ std::move(directory) }
            {
                for (const shader_manifest_entry& entry : this->manifest.entries)
                {
                    if (auto error = check_blob_path(entry))
                    {
                        path_error = std::move(error);
                        return;
                    }
                }

                contents.resize(this->manifest.entries.size());
                reads.reserve(this->manifest.entries.size());
                for (std::size_t index = 0; index < this->manifest.entries.size(); ++index)
                {
                    reads.push_back(
                        std::unique_ptr<shader_load_read_operation>{ new shader_load_read_operation{ stdexec::connect(
                            io::read_file(context, this->directory / this->manifest.entries[index].blob),
                            shader_load_read_receiver{
                                .core = this, .index = index, .token = stop_source.get_token() }) } });
                }
                finish =
                    std::unique_ptr<shader_load_finish_operation>{ new shader_load_finish_operation{ stdexec::connect(
                        stdexec::schedule(stdexec::get_parallel_scheduler()),
                        shader_load_finish_receiver{ .core = this }) } };
            }

            void begin() noexcept
            {
                if (path_error)
                {
                    owner->complete(std::unexpected(std::move(*path_error)));
                    return;
                }
                // One reference per read plus this one, so a read completing inline can't finish the load early
                pending.store(reads.size() + 1, std::memory_order_relaxed);
                for (const auto& read : reads)
                {
                    stdexec::start(*read);
                }
                release();
            }

            void request_stop() noexcept
            {
                stop_requested.store(true, std::memory_order_relaxed);
                stop_source.request_stop();
            }

            void fail(avis::error error) noexcept
            {
                {
                    const std::scoped_lock lock{ error_mutex };
                    if (first_error)
                    {
                        return;
                    }
                    first_error = std::move(error);
                }
                stop_source.request_stop(); // the other reads aren't needed any more
            }

            // Drops one reference; the last one hops to the parallel scheduler. Touches nothing after that.
            void release() noexcept
            {
                if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    stdexec::start(*finish);
                }
            }

            // On a parallel worker, after every read completed. The receiver's stop request wins over the errors it
            // may have caused; then the first failure, which stopped the other reads; then a read that stopped on its
            // own (its IO context is stopping), since its blob is missing.
            void complete() noexcept
            {
                if (stop_requested.load(std::memory_order_relaxed))
                {
                    owner->complete_stopped();
                    return;
                }
                if (first_error)
                {
                    owner->complete(std::unexpected(std::move(*first_error)));
                    return;
                }
                if (read_stopped.load(std::memory_order_relaxed))
                {
                    owner->complete_stopped();
                    return;
                }

                owner->complete(build_library_or_error());
            }

            result<shader_library> build_library_or_error() noexcept
            {
                try
                {
                    return build_library();
                }
                catch (...)
                {
                    return std::unexpected(avis::make_error(std::current_exception()));
                }
            }

            result<shader_library> build_library()
            {
                std::vector<shader_blob> blobs;
                blobs.reserve(manifest.entries.size());
                for (std::size_t index = 0; index < manifest.entries.size(); ++index)
                {
                    shader_manifest_entry& entry = manifest.entries[index];
                    std::vector<std::byte>& bytes = contents[index];
                    if (entry.content_hash)
                    {
                        const std::uint64_t hash = XXH3_64bits(bytes.data(), bytes.size());
                        if (hash != *entry.content_hash)
                        {
                            return std::unexpected(make_error(
                                error_kind::shader_hash_mismatch,
                                std::format(
                                    "shader '{}': '{}' has the xxh3 hash {:016x}, the manifest expects {:016x}",
                                    entry.id,
                                    utf8(directory / entry.blob),
                                    hash,
                                    *entry.content_hash)));
                        }
                    }
                    blobs.push_back(
                        {
                            .id = std::move(entry.id),
                            .stage = entry.stage,
                            .bytecode = std::move(bytes),
                            .bindings = std::move(entry.bindings),
                        });
                }
                return shader_library::create(std::move(blobs), utf8(directory));
            }

            shader_library_load_operation* owner;
            shader_manifest manifest;
            std::filesystem::path directory;
            std::optional<avis::error> path_error; // a blob path outside the directory; nothing is read then

            std::vector<std::vector<std::byte>> contents; // one per entry, written by its read
            std::vector<std::unique_ptr<shader_load_read_operation>> reads;
            std::unique_ptr<shader_load_finish_operation> finish;
            std::atomic<std::size_t> pending{ 0 };

            stdexec::inplace_stop_source stop_source; // stops the outstanding reads
            std::atomic<bool> stop_requested{ false }; // the receiver asked to stop
            std::atomic<bool> read_stopped{ false }; // a read completed with set_stopped
            std::mutex error_mutex;
            std::optional<avis::error> first_error; // read by complete() only after every read released
        };

        void shader_load_read_receiver::set_value(std::vector<std::byte> bytes) noexcept
        {
            core->contents[index] = std::move(bytes);
            core->release();
        }

        void shader_load_read_receiver::set_error(avis::error error) noexcept
        {
            core->fail(std::move(error));
            core->release();
        }

        void shader_load_read_receiver::set_stopped() noexcept
        {
            core->read_stopped.store(true, std::memory_order_relaxed);
            core->release();
        }

        void shader_load_finish_receiver::set_value() noexcept
        {
            core->complete();
        }

        template<class Error>
        void shader_load_finish_receiver::set_error(Error&& error) noexcept
        {
            core->fail(threading::detail::to_internal_error(std::forward<Error>(error)));
            core->complete();
        }

        void shader_load_finish_receiver::set_stopped() noexcept
        {
            // The parallel scheduler is stopping: the load can't finish
            if (!core->stop_requested.load(std::memory_order_relaxed))
            {
                core->fail(make_error(
                    generic_error_kind::cancelled, "load_shader_library: the parallel scheduler is stopping"));
            }
            core->complete();
        }

        shader_library_load_operation::shader_library_load_operation(
            io::io_context& context, shader_manifest manifest, std::filesystem::path directory) :
            core_{ std::make_unique<shader_library_load_core>(
                *this, context, std::move(manifest), std::move(directory)) }
        {}

        shader_library_load_operation::~shader_library_load_operation() = default;

        void shader_library_load_operation::begin() noexcept
        {
            core_->begin();
        }

        void shader_library_load_operation::request_stop() noexcept
        {
            core_->request_stop();
        }
    } // namespace detail

    shader_library_load_sender load_shader_library(shader_manifest manifest, std::filesystem::path directory)
    {
        return load_shader_library(avis::runtime::io_context(), std::move(manifest), std::move(directory));
    }

    shader_library_load_sender
        load_shader_library(io::io_context& context, shader_manifest manifest, std::filesystem::path directory)
    {
        return { context, std::move(manifest), std::move(directory) };
    }
} // namespace avis::graphics
