#include "rendering/blue_noise.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr int size = static_cast<int>(gpu::blue_noise_size);
        constexpr int count = size * size;
        constexpr int initial_points = count / 10;
        constexpr float sigma = 1.5f; // Ulichney's
        // The Gaussian is cut at this distance, where it has fallen below 1e-3 of its peak
        constexpr int radius = 6;
        constexpr int kernel_size = 2 * radius + 1;

        // A binary pattern and its energy: the sum over its points of a Gaussian of the toroidal distance
        struct pattern
        {
            std::vector<unsigned char> points = std::vector<unsigned char>(count, 0);
            std::vector<float> energy = std::vector<float>(count, 0.0f);

            void toggle(const int index, const std::array<float, kernel_size * kernel_size>& kernel)
            {
                const float sign = points[index] != 0 ? -1.0f : 1.0f;
                points[index] = points[index] != 0 ? 0 : 1;
                const int x = index % size;
                const int y = index / size;
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    const int row = ((y + dy + size) % size) * size;
                    for (int dx = -radius; dx <= radius; ++dx)
                    {
                        energy[row + (x + dx + size) % size] +=
                            sign * kernel[(dy + radius) * kernel_size + dx + radius];
                    }
                }
            }

            // The point in the tightest cluster: the highest energy among the points
            int tightest_cluster() const
            {
                int found = -1;
                for (int index = 0; index < count; ++index)
                {
                    if (points[index] != 0 && (found < 0 || energy[index] > energy[found]))
                    {
                        found = index;
                    }
                }
                return found;
            }

            // The empty texel in the largest void: the lowest energy among the empty texels
            int largest_void() const
            {
                int found = -1;
                for (int index = 0; index < count; ++index)
                {
                    if (points[index] == 0 && (found < 0 || energy[index] < energy[found]))
                    {
                        found = index;
                    }
                }
                return found;
            }
        };

        std::vector<std::uint16_t> generate()
        {
            std::array<float, kernel_size * kernel_size> kernel{};
            for (int dy = -radius; dy <= radius; ++dy)
            {
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    kernel[(dy + radius) * kernel_size + dx + radius] =
                        std::exp(-static_cast<float>(dx * dx + dy * dy) / (2.0f * sigma * sigma));
                }
            }

            // The initial pattern: random points (mt19937's sequence is fixed by the standard), then relaxed by moving
            // the point of the tightest cluster into the largest void until that point is the void
            pattern initial;
            std::mt19937 random{ 0x5EED };
            for (int placed = 0; placed < initial_points;)
            {
                const int index = static_cast<int>(random() % static_cast<std::uint32_t>(count));
                if (initial.points[index] == 0)
                {
                    initial.toggle(index, kernel);
                    ++placed;
                }
            }
            for (int iteration = 0; iteration < count; ++iteration)
            {
                const int cluster = initial.tightest_cluster();
                initial.toggle(cluster, kernel);
                const int empty = initial.largest_void();
                initial.toggle(empty, kernel);
                if (empty == cluster)
                {
                    break;
                }
            }

            std::vector<std::uint16_t> ranks(count, 0);
            // Phase 1: the initial points, ranked down by removing the tightest cluster
            pattern removing = initial;
            for (int rank = initial_points - 1; rank >= 0; --rank)
            {
                const int cluster = removing.tightest_cluster();
                removing.toggle(cluster, kernel);
                ranks[cluster] = static_cast<std::uint16_t>(rank);
            }
            // Phases 2 and 3: the rest, ranked up by filling the largest void
            pattern filling = initial;
            for (int rank = initial_points; rank < count; ++rank)
            {
                const int empty = filling.largest_void();
                filling.toggle(empty, kernel);
                ranks[empty] = static_cast<std::uint16_t>(rank);
            }
            return ranks;
        }

        const std::vector<std::uint16_t>& ranks()
        {
            static const std::vector<std::uint16_t> generated = generate();
            return generated;
        }
    } // namespace

    std::span<const std::uint16_t> blue_noise_ranks()
    {
        return ranks();
    }

    std::span<const std::uint8_t> blue_noise_texels()
    {
        static const std::vector<std::uint8_t> texels = []
        {
            std::vector<std::uint8_t> made;
            made.reserve(count);
            for (const std::uint16_t rank : ranks())
            {
                made.push_back(static_cast<std::uint8_t>(rank / (count / 256)));
            }
            return made;
        }();
        return texels;
    }
} // namespace avis::visualizer::rendering
