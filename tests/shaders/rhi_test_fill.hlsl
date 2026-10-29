// RHI tests (docs/tasks/06-rhi.md §6.8, §6.10): writes base + i into element i of a raw buffer reached through
// ResourceDescriptorHeap. Root signature: 3 root constants at b0.

struct fill_constants
{
    uint buffer_index; // bindless UAV index of a raw buffer
    uint base;
    uint count;
};

ConstantBuffer<fill_constants> constants : register(b0);

[numthreads(64, 1, 1)]
void fill_main(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= constants.count)
    {
        return;
    }
    RWByteAddressBuffer output = ResourceDescriptorHeap[constants.buffer_index];
    output.Store(thread.x * 4, constants.base + thread.x);
}
