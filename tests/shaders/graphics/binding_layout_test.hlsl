// Graphics tests (docs/tasks/08-graphics-shaders-and-pipelines.md §8.4): reads the three parameters of the standard
// signature (graphics::binding_layout) and writes base + i * multiplier + pass offset + view offset into element i of a
// raw buffer reached through ResourceDescriptorHeap.
// Bindings: b0 (16 bytes, draw constants), b1 (8 bytes, pass constants), b2 (4 bytes, view constants).

struct draw_constants
{
    uint buffer_index; // bindless UAV index of a raw buffer
    uint count;
    uint base;
    uint multiplier;
};

struct pass_constants
{
    uint offset;
    uint unused;
};

struct view_constants
{
    uint offset;
};

ConstantBuffer<draw_constants> draw : register(b0);
ConstantBuffer<pass_constants> pass : register(b1);
ConstantBuffer<view_constants> view : register(b2);

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= draw.count)
    {
        return;
    }
    RWByteAddressBuffer output = ResourceDescriptorHeap[draw.buffer_index];
    output.Store(thread.x * 4, draw.base + thread.x * draw.multiplier + pass.offset + view.offset);
}
