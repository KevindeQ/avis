#ifndef AVIS_GRAPHICS_DETAIL_SHADER_LIBRARY_LOAD_SENDER_H
#define AVIS_GRAPHICS_DETAIL_SHADER_LIBRARY_LOAD_SENDER_H

// The sender behind load_shader_library (docs/design/18-graphics-shaders-and-pipelines.md §4). The operation reads
// every blob at once and counts the outstanding reads with an atomic: the number of reads is only known at runtime and
// D-66 rules out a type-erased sender. Its state, the reads included, is in lib/avis/graphics/shader_loading.cpp. Free
// of asio, <windows.h> and xxhash.h.

#include "avis/detail/receiver_stop_callback.h"
#include "avis/error.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/shader_manifest.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <filesystem>
#include <memory>
#include <utility>

namespace avis::io
{
    class io_context;
} // namespace avis::io

namespace avis::graphics
{
    namespace detail
    {
        class shader_library_load_sender;
    } // namespace detail

    detail::shader_library_load_sender
        load_shader_library(io::io_context& context, shader_manifest manifest, std::filesystem::path directory);

    namespace detail
    {
        struct shader_library_load_core;

        // Non-template part of a load_shader_library operation
        class shader_library_load_operation
        {
        public:
            // Called once: inline from begin() for a bad blob path, otherwise on a parallel worker
            virtual void complete(result<shader_library> outcome) noexcept = 0;
            virtual void complete_stopped() noexcept = 0;

        protected:
            // Checks the blob paths and connects the reads; nothing is read before begin()
            shader_library_load_operation(
                io::io_context& context, shader_manifest manifest, std::filesystem::path directory);
            ~shader_library_load_operation();
            shader_library_load_operation(const shader_library_load_operation&) = delete;
            shader_library_load_operation& operator=(const shader_library_load_operation&) = delete;

            // Starts every read. The operation may complete (and be destroyed) before begin() returns.
            void begin() noexcept;

            // From the stop callback, on any thread: stops the outstanding reads, then completes with set_stopped
            void request_stop() noexcept;

        private:
            std::unique_ptr<shader_library_load_core> core_;
        };

        template<class Receiver>
        class shader_library_load_operation_state final : public shader_library_load_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            shader_library_load_operation_state(
                Receiver receiver, io::io_context& context, shader_manifest manifest, std::filesystem::path directory) :
                shader_library_load_operation{ context, std::move(manifest), std::move(directory) },
                receiver_{ std::move(receiver) }
            {}

            void start() & noexcept
            {
                if (stop_callback::stop_requested(receiver_))
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                stop_callback_.install(receiver_, stop_function{ this });
                begin();
            }

            void complete(result<shader_library> outcome) noexcept override
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
                shader_library_load_operation_state* self;

                void operator()() const noexcept
                {
                    self->request_stop();
                }
            };

            using stop_callback = avis::detail::receiver_stop_callback<Receiver, stop_function>;

            Receiver receiver_;
            stop_callback stop_callback_;
        };

        // Completes with set_value(shader_library), set_error(avis::error) or set_stopped(). Single-shot.
        class shader_library_load_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = avis::completions<shader_library>;

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) && -> shader_library_load_operation_state<Receiver>
            {
                return { std::move(receiver), *context_, std::move(manifest_), std::move(directory_) };
            }

        private:
            friend shader_library_load_sender graphics::load_shader_library(
                io::io_context& context, shader_manifest manifest, std::filesystem::path directory);

            shader_library_load_sender(
                io::io_context& context, shader_manifest manifest, std::filesystem::path directory) :
                context_{ &context },
                manifest_{ std::move(manifest) },
                directory_{ std::move(directory) }
            {}

            io::io_context* context_;
            shader_manifest manifest_;
            std::filesystem::path directory_;
        };

        static_assert(avis::sender_of<shader_library_load_sender, shader_library>);
    } // namespace detail

    using detail::shader_library_load_sender;
} // namespace avis::graphics

#endif
