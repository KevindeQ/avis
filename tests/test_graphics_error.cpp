// Graphics error domain (docs/tasks/07-graphics-frames-and-transfers.md §7.1)

#include "avis/error.h"
#include "avis/graphics/error.h"
#include "avis/rhi/error.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <set>
#include <string_view>

using namespace avis;

TEST_CASE("graphics errors are in their own domain", "[graphics][unit]")
{
    const avis::error error = graphics::make_error(graphics::error_kind::upload_queue_full, "ring is full");

    CHECK(error.domain == error_domain::graphics);
    CHECK(error.code == static_cast<std::int32_t>(graphics::error_kind::upload_queue_full));
    CHECK(error.message == "ring is full");
    CHECK(graphics::is_error(error, graphics::error_kind::upload_queue_full));
    CHECK_FALSE(graphics::is_error(error, graphics::error_kind::invalid_argument));

    // Same code in another domain isn't a graphics error
    const avis::error rhi_error = rhi::make_error(static_cast<rhi::error_kind>(error.code), "same code");
    CHECK_FALSE(graphics::is_error(rhi_error, graphics::error_kind::upload_queue_full));

    CHECK(std::format("{}", graphics::error_kind::upload_queue_full) == "upload_queue_full");
    CHECK(std::format("{}", error).starts_with("[graphics:1] ring is full"));
}

TEST_CASE("every graphics error kind has a distinct name", "[graphics][unit]")
{
    constexpr auto last = graphics::error_kind::shader_bindings_missing;
    std::set<std::string_view> names;
    for (std::int32_t code = 0; code <= static_cast<std::int32_t>(last); ++code)
    {
        const std::string_view name = graphics::to_string(static_cast<graphics::error_kind>(code));
        CHECK(name != "unknown");
        names.insert(name);
    }
    CHECK(names.size() == static_cast<std::size_t>(last) + 1);
    CHECK(graphics::to_string(graphics::error_kind::ok) == "ok");
    CHECK(graphics::to_string(graphics::error_kind::no_compatible_root_signature) == "no_compatible_root_signature");
    CHECK(graphics::to_string(static_cast<graphics::error_kind>(1000)) == "unknown");
}
