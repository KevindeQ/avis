// Smoke tests for the vendored external libraries. They only
// check that each library is built, linked and usable, so a broken integration fails here and not in
// the first feature that uses it.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <fastgltf/core.hpp>
#include <memory>
#include <spdlog/logger.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <string>
#include <string_view>
#include <xxhash.h>
#include <yaml-cpp/yaml.h>

TEST_CASE("spdlog formats with std::format", "[external][spdlog]")
{
    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(4);
    spdlog::logger logger{ "external-test", sink };
    logger.set_pattern("[%n] [%l] %v");

    logger.info("io_threads={} parallel_workers={}", 1, 6);

    const auto lines = sink->last_formatted();
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().starts_with("[external-test] [info] io_threads=1 parallel_workers=6"));
}

TEST_CASE("yaml-cpp parses a mapping", "[external][yaml-cpp]")
{
    const YAML::Node root = YAML::Load("runtime: { io_threads: 1, parallel_workers: 0 }");

    REQUIRE(root["runtime"].IsMap());
    CHECK(root["runtime"]["io_threads"].as<int>() == 1);
    CHECK(root["runtime"]["parallel_workers"].as<int>() == 0);
    CHECK_FALSE(root["logging"].IsDefined());
}

TEST_CASE("fastgltf parses a minimal glTF document", "[external][fastgltf]")
{
    constexpr std::string_view json = R"({"asset":{"version":"2.0"}})";

    auto data = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(json.data()), json.size());
    REQUIRE(data.error() == fastgltf::Error::None);

    fastgltf::Parser parser;
    auto asset = parser.loadGltfJson(data.get(), {});
    REQUIRE(asset.error() == fastgltf::Error::None);
    CHECK(asset.get().assetInfo.has_value());
    CHECK(asset.get().assetInfo->gltfVersion == "2.0");
}

// Updating external/xxHash-0.8.4 must update external/README.md too
static_assert(XXH_VERSION_NUMBER == 804);

TEST_CASE("xxHash's XXH3_64bits matches Python's xxhash.xxh3_64", "[external][xxhash]")
{
    constexpr std::string_view input = "avis shader blob";

    // python -c "import xxhash; print(hex(xxhash.xxh3_64_intdigest(b'avis shader blob')))"
    constexpr XXH64_hash_t expected = 0xb8cb7f355f950193ULL;

    CHECK(XXH3_64bits(input.data(), input.size()) == expected);
}
