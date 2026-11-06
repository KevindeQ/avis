#ifndef AVIS_EXPOSURE_HLSLI
#define AVIS_EXPOSURE_HLSLI

// Reading the view's exposure buffer (docs/design/11-renderer.md §10.3, the layout in shared_types.hlsli). The passes
// before exposure_adapt read the frame's pre-exposure as `current` and the previous frame's as `previous`. After it,
// `current` is the next frame's and `previous` this frame's, which is what the tonemap rescales by.

#include "shared_types.hlsli"

float load_current_exposure(uint exposure_buffer)
{
    ByteAddressBuffer exposure = ResourceDescriptorHeap[exposure_buffer];
    return asfloat(exposure.Load(exposure_offset_current));
}

float load_previous_exposure(uint exposure_buffer)
{
    ByteAddressBuffer exposure = ResourceDescriptorHeap[exposure_buffer];
    return asfloat(exposure.Load(exposure_offset_previous));
}

// The current exposure over the previous one: what rescales colour pre-exposed with the previous exposure
float exposure_change(uint exposure_buffer)
{
    ByteAddressBuffer exposure = ResourceDescriptorHeap[exposure_buffer];
    return asfloat(exposure.Load(exposure_offset_current)) / asfloat(exposure.Load(exposure_offset_previous));
}

#endif
