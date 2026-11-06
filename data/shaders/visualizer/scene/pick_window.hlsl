// @avis-shader entry=main stage=cs
//
// The pick window (docs/design/11-renderer.md §5.1, docs/tasks/11-renderer.md §11.19): the pick_window_size² texels
// of picking_id around a click, row by row, into a small buffer that a copy pass then copies into a readback buffer.
// One group, one texel per thread. Texels outside the view are 0 (nothing).

#include "bindless.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(pick_window_constants, constants);

[numthreads(pick_window_size, pick_window_size, 1)] void main(uint3 thread : SV_GroupThreadID)
{
    Texture2D<uint2> picking = ResourceDescriptorHeap[constants.picking_id];
    RWByteAddressBuffer window = ResourceDescriptorHeap[constants.window];
    const int2 pixel = constants.origin + int2(thread.xy);
    uint2 id = uint2(0, 0);
    if (all(pixel >= 0) && all(pixel < int2(constants.size)))
    {
        id = picking.Load(int3(pixel, 0));
    }
    window.Store2((thread.y * pick_window_size + thread.x) * 8, id);
}
