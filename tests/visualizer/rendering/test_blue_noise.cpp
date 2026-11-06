// The tonemap's blue-noise dither (docs/tasks/11-renderer.md §11.16): the rank map is a permutation, each 8-bit level
// appears equally often, and it has no low frequencies (its blocks and its sparse thresholds are even).

#include "rendering/blue_noise.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "shared_types.hlsli"

using namespace avis::visualizer::rendering;

namespace
{
    constexpr int size = static_cast<int>(gpu::blue_noise_size);
    constexpr int count = size * size;
} // namespace

TEST_CASE("the blue-noise ranks are a permutation, and its texels have every level equally often", "[renderer]")
{
    const std::span<const std::uint16_t> ranks = blue_noise_ranks();
    REQUIRE(ranks.size() == count);
    std::vector<int> seen(count, 0);
    for (const std::uint16_t rank : ranks)
    {
        REQUIRE(rank < count);
        ++seen[rank];
    }
    CHECK(std::ranges::all_of(seen, [](const int times) { return times == 1; }));

    const std::span<const std::uint8_t> texels = blue_noise_texels();
    REQUIRE(texels.size() == count);
    std::array<int, 256> levels{};
    for (std::size_t index = 0; index < texels.size(); ++index)
    {
        CHECK(texels[index] == ranks[index] / 16);
        ++levels[texels[index]];
    }
    CHECK(std::ranges::all_of(levels, [](const int times) { return times == count / 256; }));

    // Generated once
    CHECK(blue_noise_ranks().data() == ranks.data());
}

TEST_CASE("the blue noise has no low frequencies", "[renderer]")
{
    const std::span<const std::uint8_t> texels = blue_noise_texels();

    // Each 8 × 8 block's mean level is close to the mean of all, 127.5. White noise's block means spread by
    // 74 / 8 = 9 levels (RMS), and the largest of its 64 blocks is about 25 off; void-and-cluster gives about a
    // quarter of that.
    double largest = 0.0;
    double squares = 0.0;
    for (int block_y = 0; block_y < size; block_y += 8)
    {
        for (int block_x = 0; block_x < size; block_x += 8)
        {
            double sum = 0.0;
            for (int y = block_y; y < block_y + 8; ++y)
            {
                for (int x = block_x; x < block_x + 8; ++x)
                {
                    sum += texels[static_cast<std::size_t>(y * size + x)];
                }
            }
            const double offset = sum / 64.0 - 127.5;
            largest = std::max(largest, std::abs(offset));
            squares += offset * offset;
        }
    }
    const double rms = std::sqrt(squares / 64.0);
    INFO("block mean offsets: RMS " << rms << ", largest " << largest);
    CHECK(rms < 4.5);
    CHECK(largest < 12.0);

    // The 256 lowest ranks (one texel in 16, 4 texels apart on average) are spread out: no two are neighbours
    const std::span<const std::uint16_t> ranks = blue_noise_ranks();
    std::vector<int> points;
    for (int index = 0; index < count; ++index)
    {
        if (ranks[static_cast<std::size_t>(index)] < count / 16)
        {
            points.push_back(index);
        }
    }
    REQUIRE(points.size() == count / 16);
    int closest = count;
    for (std::size_t first = 0; first < points.size(); ++first)
    {
        for (std::size_t second = first + 1; second < points.size(); ++second)
        {
            int dx = std::abs(points[first] % size - points[second] % size);
            int dy = std::abs(points[first] / size - points[second] / size);
            dx = std::min(dx, size - dx); // the texture tiles
            dy = std::min(dy, size - dy);
            closest = std::min(closest, dx * dx + dy * dy);
        }
    }
    INFO("closest squared distance " << closest);
    CHECK(closest >= 4);
}
