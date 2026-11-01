// Scene error domain and simulation time types (docs/tasks/10-scene-management.md §10.1)

#include "avis/error.h"
#include "avis/graphics/error.h"
#include "avis/scene/error.h"
#include "avis/simulation/time.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <set>
#include <source_location>
#include <string_view>

using namespace avis;
using namespace std::chrono_literals;

TEST_CASE("scene errors are in their own domain", "[scene][unit]")
{
    const std::source_location location = std::source_location::current();
    const avis::error error = scene::make_error(scene::error_kind::duplicate_id, "id 42 exists", location);

    CHECK(error.domain == error_domain::scene);
    CHECK(error.code == static_cast<std::int32_t>(scene::error_kind::duplicate_id));
    CHECK(error.message == "id 42 exists");
    CHECK(error.location.line() == location.line());
    CHECK(std::string_view{ error.location.file_name() } == location.file_name());

    CHECK(scene::is_error(error, scene::error_kind::duplicate_id));
    CHECK_FALSE(scene::is_error(error, scene::error_kind::invalid_parent));

    // The same code in another domain isn't a scene error
    const avis::error graphics_error =
        graphics::make_error(static_cast<graphics::error_kind>(error.code), "same code");
    CHECK_FALSE(scene::is_error(graphics_error, scene::error_kind::duplicate_id));

    CHECK(std::format("{}", scene::error_kind::duplicate_id) == "duplicate_id");
    CHECK(std::format("{}", error).starts_with("[scene:1] id 42 exists"));
}

TEST_CASE("make_error records the caller's source location by default", "[scene][unit]")
{
    const std::uint_least32_t line = std::source_location::current().line() + 1;
    const avis::error error = scene::make_error(scene::error_kind::cycle, "loop");
    CHECK(error.location.line() == line);
}

TEST_CASE("every scene error kind has a distinct name", "[scene][unit]")
{
    constexpr auto last = scene::error_kind::image_decode_failed;
    std::set<std::string_view> names;
    for (std::int32_t code = 0; code <= static_cast<std::int32_t>(last); ++code)
    {
        const std::string_view name = scene::to_string(static_cast<scene::error_kind>(code));
        CHECK(name != "unknown");
        names.insert(name);
    }
    CHECK(names.size() == static_cast<std::size_t>(last) + 1);
    CHECK(scene::to_string(scene::error_kind::ok) == "ok");
    CHECK(scene::to_string(scene::error_kind::gltf_unsupported) == "gltf_unsupported");
    CHECK(scene::to_string(static_cast<scene::error_kind>(1000)) == "unknown");
}

TEST_CASE("simulation time is derived from the step index", "[scene][unit]")
{
    static_assert(simulation::time_of_step(10, 100ms) == 1s);
    static_assert(simulation::time_of_step(0, 20ms) == simulation::simulation_duration::zero());

    CHECK(simulation::time_of_step(10, 100ms) == 1s);
    // 50 Hz for an hour, exact in integer nanoseconds
    CHECK(simulation::time_of_step(180'000, 20ms) == 1h);

    const simulation::time_anchor anchor{};
    CHECK(anchor.t_base == simulation::simulation_duration::zero());
    CHECK(anchor.rate == 0.0);
}
