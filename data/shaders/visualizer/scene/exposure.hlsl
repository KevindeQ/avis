// @avis-shader entry=histogram_main stage=cs entry=adapt_main stage=cs
//
// Automatic exposure (docs/design/11-renderer.md §10.3, §2.3 pass 12, docs/tasks/11-renderer.md §11.16), into the
// view's exposure buffer (layout in shared_types.hlsli):
// - histogram_main, 16 × 16 groups: each pixel of taa_output adds to the bin of its pre-exposed luminance relative
//   to the key, in log2 over 16 stops (clamped at both ends), and its position within the bin to the bin's sum. Groups
//   gather in groupshared memory, then add to the buffer.
// - adapt_main, one group of 128 threads: reads and clears the bins. In automatic mode it averages the bins' log
//   luminance over the pixels between the darkest 50% and the brightest 5%, which gives the exposure that brings that
//   average to the key, and moves towards it exponentially with the frame time: faster while the exposure rises (the
//   image brightens) than while it falls. The manual mode takes the manual exposure, and the reset mode writes it as
//   both the current and the previous exposure. The previous exposure becomes the one this frame used.

#include "bindless.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(exposure_constants, constants);

groupshared uint shared_counts[exposure_histogram_bins];
groupshared uint shared_sums[exposure_histogram_bins];

float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(16, 16, 1)] void histogram_main(uint3 thread : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < exposure_histogram_bins)
    {
        shared_counts[index] = 0;
        shared_sums[index] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (all(thread.xy < constants.size))
    {
        Texture2D<float4> color = ResourceDescriptorHeap[constants.color];
        const float pre_exposed = luminance(max(color.Load(int3(thread.xy, 0)).rgb, 0.0));
        // Black, and NaN through max, go to the start of bin 0
        const float stops = log2(max(pre_exposed, 1.0e-30) / constants.key);
        const float position = clamp((stops - exposure_histogram_minimum) * exposure_histogram_bins_per_stop, 0.0,
                                     float(exposure_histogram_bins) - 1.0e-3);
        const uint bin = min(uint(position), exposure_histogram_bins - 1);
        const uint fraction = min(uint((position - float(bin)) * float(exposure_histogram_fraction_steps)),
                                  exposure_histogram_fraction_steps - 1);
        InterlockedAdd(shared_counts[bin], 1u);
        InterlockedAdd(shared_sums[bin], fraction);
    }
    GroupMemoryBarrierWithGroupSync();

    if (index < exposure_histogram_bins && shared_counts[index] != 0)
    {
        RWByteAddressBuffer exposure = ResourceDescriptorHeap[constants.exposure_buffer];
        exposure.InterlockedAdd(exposure_offset_counts + index * 4, shared_counts[index]);
        exposure.InterlockedAdd(exposure_offset_sums + index * 4, shared_sums[index]);
    }
}

[numthreads(128, 1, 1)] void adapt_main(uint index : SV_GroupIndex)
{
    RWByteAddressBuffer exposure = ResourceDescriptorHeap[constants.exposure_buffer];
    shared_counts[index] = exposure.Load(exposure_offset_counts + index * 4);
    shared_sums[index] = exposure.Load(exposure_offset_sums + index * 4);
    exposure.Store(exposure_offset_counts + index * 4, 0u);
    exposure.Store(exposure_offset_sums + index * 4, 0u);
    GroupMemoryBarrierWithGroupSync();
    if (index != 0)
    {
        return;
    }

    if (constants.mode == exposure_mode_reset)
    {
        exposure.Store(exposure_offset_current, asuint(constants.manual_exposure));
        exposure.Store(exposure_offset_previous, asuint(constants.manual_exposure));
        exposure.Store(exposure_offset_luminance, asuint(0.0));
        return;
    }

    float used = asfloat(exposure.Load(exposure_offset_current));
    if (!(used > 0.0) || isinf(used))
    {
        used = constants.manual_exposure; // can't happen after the reset; keeps a broken value from sticking
    }
    float next = constants.manual_exposure;
    float measured = 0.0;
    if (constants.mode == exposure_mode_automatic)
    {
        next = used;
        uint total = 0;
        for (uint bin = 0; bin < exposure_histogram_bins; ++bin)
        {
            total += shared_counts[bin];
        }
        // The pixels from the darkest 50% to the brightest 5%; a bin on either boundary counts in part, at its mean
        const float lower = 0.5 * float(total);
        const float upper = 0.95 * float(total);
        float start = 0.0;
        float weight = 0.0;
        float weighted = 0.0;
        for (uint bin = 0; bin < exposure_histogram_bins; ++bin)
        {
            const float count = float(shared_counts[bin]);
            const float end = start + count;
            const float inside = max(min(end, upper) - max(start, lower), 0.0);
            if (inside > 0.0)
            {
                const float mean = float(bin) + (float(shared_sums[bin]) / count + 0.5) /
                                                    float(exposure_histogram_fraction_steps);
                weighted += inside * mean;
                weight += inside;
            }
            start = end;
        }
        if (weight > 0.0)
        {
            // The average's stops above the key, pre-exposed; the target exposure brings it to the key
            const float stops = weighted / weight / exposure_histogram_bins_per_stop + exposure_histogram_minimum;
            measured = constants.key * exp2(stops) / used;
            const float step = -stops;
            const float rate = step > 0.0 ? constants.brighten_rate : constants.darken_rate;
            next = used * exp2(step * (1.0 - exp(-constants.frame_time * rate)));
            next = clamp(next, constants.minimum_exposure, constants.maximum_exposure);
        }
    }
    exposure.Store(exposure_offset_previous, asuint(used));
    exposure.Store(exposure_offset_current, asuint(next));
    exposure.Store(exposure_offset_luminance, asuint(measured));
}
