// @avis-shader entry=main stage=cs
//
// A view's cluster statistics (docs/design/11-renderer.md §6.3, docs/tasks/11-renderer.md §11.20), for
// scene_renderer::debug_information: one group sums light_cull's cluster_lights into a uint4 (cluster_statistics_bytes,
// see shared_types.hlsli), which a copy pass reads back.

#include "bindless.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(cluster_statistics_constants, constants);

static const uint statistics_threads = 256;

groupshared uint dropped_references;
groupshared uint dropping_clusters;
groupshared uint most_lights;
groupshared uint kept_references;

[numthreads(statistics_threads, 1, 1)] void main(uint3 thread : SV_GroupThreadID)
{
    if (thread.x == 0)
    {
        dropped_references = 0;
        dropping_clusters = 0;
        most_lights = 0;
        kept_references = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    StructuredBuffer<cluster_lights> clusters = ResourceDescriptorHeap[constants.cluster_lights];
    uint dropped = 0;
    uint dropping = 0;
    uint most = 0;
    uint kept = 0;
    for (uint index = thread.x; index < cluster_count; index += statistics_threads)
    {
        const cluster_lights cluster = clusters[index];
        dropped += cluster.dropped;
        dropping += cluster.dropped > 0 ? 1 : 0;
        most = max(most, cluster.count + cluster.dropped);
        kept += cluster.count;
    }
    InterlockedAdd(dropped_references, dropped);
    InterlockedAdd(dropping_clusters, dropping);
    InterlockedMax(most_lights, most);
    InterlockedAdd(kept_references, kept);
    GroupMemoryBarrierWithGroupSync();

    if (thread.x == 0)
    {
        RWByteAddressBuffer output = ResourceDescriptorHeap[constants.output];
        output.Store4(0, uint4(dropped_references, dropping_clusters, most_lights, kept_references));
    }
}
