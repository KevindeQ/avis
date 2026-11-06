#ifndef AVIS_VISUALIZER_RENDERING_PASS_STATISTICS_H
#define AVIS_VISUALIZER_RENDERING_PASS_STATISTICS_H

// What the scene passes recorded (docs/tasks/11-renderer.md §11.20): the range walker's counters of each pass, summed
// over its recording parts, for scene_renderer::debug_information. The passes are registered while the graph is built,
// in declaration order, and their parts record into them while it executes, possibly on several threads at once.

#include "rendering/range_walker.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace avis::visualizer::rendering
{
    struct pass_draw_statistics
    {
        std::string view;
        std::string pass; // the pass's name, without the _part suffixes
        std::size_t parts = 0; // its recording parts
        std::size_t parts_recorded = 0; // of the last execute
        range_walk_statistics walk; // summed over the parts recorded
    };

    class pass_statistics_collector
    {
    public:
        // Forgets every pass: the frame being built starts again
        void clear()
        {
            const std::scoped_lock lock{ mutex_ };
            passes_.clear();
        }

        // Registers a pass of the frame being built (not while it executes); returns the slot its parts record into
        std::size_t add_pass(const std::string_view view, const std::string_view pass, const std::size_t parts)
        {
            const std::scoped_lock lock{ mutex_ };
            passes_.push_back({ .view = std::string{ view }, .pass = std::string{ pass }, .parts = parts });
            return passes_.size() - 1;
        }

        // A part's counters; thread-safe
        void record(const std::size_t slot, const range_walk_statistics& walk)
        {
            const std::scoped_lock lock{ mutex_ };
            if (slot < passes_.size())
            {
                passes_[slot].walk += walk;
                ++passes_[slot].parts_recorded;
            }
        }

        // In declaration order. Complete once the frame's execute has completed.
        std::vector<pass_draw_statistics> passes() const
        {
            const std::scoped_lock lock{ mutex_ };
            return passes_;
        }

    private:
        mutable std::mutex mutex_;
        std::vector<pass_draw_statistics> passes_;
    };
} // namespace avis::visualizer::rendering

#endif
