#ifndef AVIS_IO_DETAIL_ASYNC_FILE_STATE_H
#define AVIS_IO_DETAIL_ASYNC_FILE_STATE_H

// Private: the state of one open file (asio file, strand, path), shared by the async_file and its operations.
// Defined here because async_file.cpp creates and closes it and file_sender.cpp runs operations on it; the
// per-operation state (file_operation_core) is private to file_sender.cpp.

#include "avis/io/detail/registered_operation.h"
#include "avis/io/io_context.h"

#include <asio/io_context.hpp>
#include <asio/random_access_file.hpp>
#include <asio/strand.hpp>

#include <filesystem>
#include <memory>
#include <utility>

namespace avis::io
{
    namespace detail
    {
        class async_file_state
        {
        public:
            async_file_state(
                io_context& context,
                asio::random_access_file file,
                std::filesystem::path path,
                operation_registry* const registry) :
                file{ std::move(file) },
                strand{ asio::make_strand(context.native().get_executor()) },
                path{ std::move(path) },
                registry{ registry }
            {}

            async_file_state(const async_file_state&) = delete;
            async_file_state& operator=(const async_file_state&) = delete;

            // The asio file closes its handle when the state is destroyed, after the last operation released it

            asio::random_access_file file;
            asio::strand<asio::io_context::executor_type> strand;
            std::filesystem::path path;
            operation_registry* registry; // nullptr would mean a plain asio context; avis contexts always have one
            bool closed = false; // only read and written on the strand
        };
    } // namespace detail
} // namespace avis::io

#endif
