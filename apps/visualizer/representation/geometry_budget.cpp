#include "representation/geometry_budget.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace avis::visualizer::representation
{
    namespace
    {
        enum class skip_state : std::uint8_t
        {
            none,
            keep, // step 3: keeps its previous version, if it has one
            release // the budget shrank under it: its previous version goes too
        };

        // Step 2 never takes a progressive mesh below this
        std::uint32_t minimum_vertices(const budget_item& item) noexcept
        {
            if (item.full_vertices == 0)
            {
                return 0;
            }
            const auto minimum = static_cast<std::uint32_t>(
                std::ceil(static_cast<double>(item.full_vertices) * minimum_prefix_fraction));
            return std::clamp<std::uint32_t>(minimum, 1, item.full_vertices);
        }

        std::uint32_t capped(const budget_item& item, const std::uint32_t cap) noexcept
        {
            return std::min(item.full_vertices, std::max(minimum_vertices(item), cap));
        }

        bool wants_new_version(const budget_item& item) noexcept
        {
            return !item.exists || item.changed;
        }

        // Steps 1 and 2 for the items that aren't skipped: as far as needed to fit, or all the way (`maximum`)
        struct fitting
        {
            std::vector<bool> shown;
            std::vector<std::uint32_t> vertices;
            std::uint32_t dropped = 0;
            std::uint64_t total = 0;
        };

        fitting
            fit(const std::span<const budget_item> items,
                const std::vector<skip_state>& skips,
                const std::uint64_t budget_bytes,
                const bool maximum)
        {
            const std::size_t count = items.size();
            fitting fitted{ .shown = std::vector<bool>(count, false),
                            .vertices = std::vector<std::uint32_t>(count, 0) };
            for (std::size_t index = 0; index < count; ++index)
            {
                const budget_item& item = items[index];
                if (skips[index] == skip_state::none)
                {
                    fitted.shown[index] = true;
                    fitted.vertices[index] = item.full_vertices;
                    fitted.total += item.full_bytes;
                }
                else if (skips[index] == skip_state::keep && item.exists)
                {
                    fitted.total += item.current_bytes;
                }
            }
            const auto over = [&] { return maximum || fitted.total > budget_bytes; };

            // 1. The globally oldest sweeps, keeping each sensor's newest
            if (over())
            {
                std::map<std::uint64_t, std::uint32_t> per_sensor;
                std::vector<std::uint32_t> sweeps;
                for (std::size_t index = 0; index < count; ++index)
                {
                    if (fitted.shown[index] && items[index].kind == budget_kind::sweep)
                    {
                        ++per_sensor[items[index].sensor];
                        sweeps.push_back(static_cast<std::uint32_t>(index));
                    }
                }
                std::ranges::stable_sort(sweeps, {}, [&](const std::uint32_t index) { return items[index].age; });
                for (const std::uint32_t index : sweeps)
                {
                    if (!over())
                    {
                        break;
                    }
                    std::uint32_t& remaining = per_sensor[items[index].sensor];
                    if (remaining > 1)
                    {
                        --remaining;
                        fitted.shown[index] = false;
                        ++fitted.dropped;
                        fitted.total -= items[index].full_bytes;
                    }
                }
            }

            // 2. One vertex cap for every progressive mesh, so the largest shrink first
            std::vector<std::uint32_t> progressive;
            std::uint64_t fixed = fitted.total;
            std::uint32_t largest = 0;
            for (std::size_t index = 0; index < count; ++index)
            {
                if (fitted.shown[index] && items[index].progressive)
                {
                    progressive.push_back(static_cast<std::uint32_t>(index));
                    fixed -= items[index].full_bytes;
                    largest = std::max(largest, items[index].full_vertices);
                }
            }
            if (!over() || progressive.empty())
            {
                return fitted;
            }
            const auto cost = [&](const std::uint32_t cap)
            {
                std::uint64_t bytes = fixed;
                for (const std::uint32_t index : progressive)
                {
                    bytes += prefix_bytes(items[index], capped(items[index], cap));
                }
                return bytes;
            };

            // The largest cap that fits, in quanta; 0 puts every mesh at its minimum
            std::uint32_t cap = 0;
            if (!maximum && cost(0) <= budget_bytes)
            {
                std::uint32_t low = 0; // fits
                std::uint32_t high = largest; // doesn't: the full sizes didn't
                while (high - low > 1)
                {
                    const std::uint32_t middle = low + (high - low) / 2;
                    (cost(middle) <= budget_bytes ? low : high) = middle;
                }
                cap = low / prefix_quantum * prefix_quantum; // smaller still fits
            }
            for (const std::uint32_t index : progressive)
            {
                fitted.vertices[index] = capped(items[index], cap);
            }
            fitted.total = cost(cap);
            return fitted;
        }
    } // namespace

    std::uint64_t prefix_bytes(const budget_item& item, const std::uint32_t vertices) noexcept
    {
        if (!item.progressive || item.full_vertices == 0)
        {
            return item.full_bytes;
        }
        return item.full_bytes * std::min(vertices, item.full_vertices) / item.full_vertices;
    }

    geometry_plan plan_geometry(const std::span<const budget_item> items, const std::uint64_t budget_bytes)
    {
        const std::size_t count = items.size();
        geometry_plan plan;
        plan.decisions.resize(count);
        plan.reduction.budget_bytes = budget_bytes;

        // 3. Which new versions to skip: as few as steps 1 and 2, all the way, need. Chunks and trails first, newest
        // first; then sweeps, oldest first.
        std::vector<skip_state> skips(count, skip_state::none);
        const auto fits = [&] { return fit(items, skips, budget_bytes, true).total <= budget_bytes; };
        if (!fits())
        {
            std::vector<std::uint32_t> candidates;
            for (std::size_t index = count; index-- > 0;)
            {
                if (items[index].kind != budget_kind::sweep && wants_new_version(items[index]))
                {
                    candidates.push_back(static_cast<std::uint32_t>(index));
                }
            }
            std::vector<std::uint32_t> sweeps;
            for (std::size_t index = 0; index < count; ++index)
            {
                if (items[index].kind == budget_kind::sweep && wants_new_version(items[index]))
                {
                    sweeps.push_back(static_cast<std::uint32_t>(index));
                }
            }
            std::ranges::stable_sort(sweeps, {}, [&](const std::uint32_t index) { return items[index].age; });
            candidates.insert(candidates.end(), sweeps.begin(), sweeps.end());
            for (const std::uint32_t index : candidates)
            {
                skips[index] = skip_state::keep;
                if (fits())
                {
                    break;
                }
            }

            // Still too much: the budget shrank under what's there. The previous versions go, the skipped ones first,
            // then the others, newest first.
            if (!fits())
            {
                std::vector<std::uint32_t> releases;
                for (const std::uint32_t index : candidates)
                {
                    if (items[index].exists)
                    {
                        releases.push_back(index);
                    }
                }
                for (std::size_t index = count; index-- > 0;)
                {
                    if (items[index].exists && skips[index] == skip_state::none)
                    {
                        releases.push_back(static_cast<std::uint32_t>(index));
                    }
                }
                for (const std::uint32_t index : releases)
                {
                    skips[index] = skip_state::release;
                    if (fits())
                    {
                        break;
                    }
                }
            }
        }

        // 1 and 2, as far as needed
        const fitting fitted = fit(items, skips, budget_bytes, false);
        plan.reduction.sweeps_dropped = fitted.dropped;

        // Releases first, then shrinking builds, then growing ones: the bytes never pass the plan's total on the way
        std::vector<std::uint32_t> shrinks;
        std::vector<std::uint32_t> growers;
        std::uint64_t used = 0;
        std::uint64_t drawn = 0;
        std::uint64_t full = 0;
        for (std::size_t index = 0; index < count; ++index)
        {
            const budget_item& item = items[index];
            budget_decision& decision = plan.decisions[index];
            const auto position = static_cast<std::uint32_t>(index);
            if (skips[index] != skip_state::none)
            {
                decision.skipped = true;
                ++plan.reduction.skipped;
                if (skips[index] == skip_state::release && item.exists)
                {
                    decision.action = budget_action::release;
                    plan.order.push_back(position);
                }
                else
                {
                    used += item.exists ? item.current_bytes : 0;
                }
                continue;
            }
            if (!fitted.shown[index])
            {
                decision.dropped = true;
                if (item.exists)
                {
                    decision.action = budget_action::release;
                    plan.order.push_back(position);
                }
                continue;
            }

            const std::uint32_t vertices = fitted.vertices[index];
            const std::uint64_t target = prefix_bytes(item, vertices);
            used += target;
            if (item.progressive)
            {
                drawn += vertices;
                full += item.full_vertices;
            }
            const bool build = wants_new_version(item) || (item.progressive && vertices != item.current_vertices);
            if (!build)
            {
                continue;
            }
            decision = { .action = budget_action::build, .vertices = vertices };
            (item.exists && target <= item.current_bytes ? shrinks : growers).push_back(position);
        }
        plan.order.insert(plan.order.end(), shrinks.begin(), shrinks.end());
        plan.order.insert(plan.order.end(), growers.begin(), growers.end());

        plan.reduction.prefix_fraction = full != 0 ? static_cast<float>(static_cast<double>(drawn) / full) : 1.0f;
        plan.reduction.used_bytes = used;
        return plan;
    }
} // namespace avis::visualizer::representation
