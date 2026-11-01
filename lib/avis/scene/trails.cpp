// scene::update_trails (docs/design/08-scene-management.md §8.2)

#include "avis/detail/check.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/scene.h"

#include <chrono>
#include <format>
#include <string_view>

namespace avis::scene
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";
    } // namespace

    void scene::update_trails(const simulation::simulation_duration time)
    {
        check_thread("update_trails");
        check_snapshot_order("update_trails");
#ifndef NDEBUG
        AVIS_CHECK(
            logger_name,
            !trails_updated_,
            "update_trails: called twice in one tick; call it once, after update_world_transforms (08 §9.6)");
#endif
        trails_updated_ = true;

        for (const auto [entity, reference, path] :
             registry_.view<components::node_reference, components::trail>().each())
        {
            // trail::samples is scene's: only this function changes it (08 §9.2)
            std::size_t& counted = trail_counts_.get(entity);
            AVIS_CHECK(
                logger_name,
                path.samples.size() == counted,
                std::format(
                    "update_trails: trail::samples of {:#x} changed outside scene ({} samples, {} expected); it's "
                    "owned by scene",
                    id_of(entity),
                    path.samples.size(),
                    counted));

            const Eigen::Vector3f origin = graph_.world(reference.node).translation();
            if (jumped_.contains(entity))
            {
                // No line across the jump (§4.1)
                path.samples.push_back({ .time = time, .position = origin, .starts_segment = !path.samples.empty() });
            }
            else if (
                path.samples.empty() || (origin - path.samples.back().position).norm() >= path.settings.minimum_spacing)
            {
                path.samples.push_back({ .time = time, .position = origin });
            }

            const auto kept = std::chrono::duration_cast<simulation::simulation_duration>(
                std::chrono::duration<double>{ path.settings.duration_seconds });
            while (!path.samples.empty() && path.samples.front().time < time - kept)
            {
                path.samples.pop_front();
            }

            counters_.trail_samples += path.samples.size();
            counters_.trail_samples -= counted;
            counted = path.samples.size();
        }
    }
} // namespace avis::scene
