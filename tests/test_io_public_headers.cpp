// Compile-time check that the asio-free public IO headers stay free of asio and <windows.h>
// (docs/tasks/05-io-layer.md §5.8, 07 Code placement). These includes must come first in this file: anything included
// before them could pull in asio or <windows.h> and hide a leak.

#include "avis/io/async_file.h"
#include "avis/io/error.h"
#include "avis/io/file_operations.h"
#include "avis/io/mapped_file.h"
#include "avis/sender_of.h"

// ASIO_VERSION alone isn't enough: only asio/version.hpp (and asio.hpp) define it, while single headers such as
// asio/io_context.hpp don't. Every asio header includes asio/detail/config.hpp, so its include guard catches them all.
#if defined(ASIO_VERSION) || defined(ASIO_DETAIL_CONFIG_HPP)
#    error "a public IO header includes asio"
#endif

#ifdef _WINDOWS_
#    error "a public IO header includes <windows.h>"
#endif

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the asio-free public IO headers include neither asio nor windows.h", "[io][headers]")
{
    // The check is the #error directives above; reaching this test means the file compiled
    SUCCEED();
}
