#version 460 core

// One triangle covering the target. QRhi's D3D12 conventions: clip-space y points up, texture v = 0 is the top row, so
// v grows downwards on screen and the frame appears upright.

layout(location = 0) out vec2 texCoords;

void main()
{
    // (0, 0), (2, 0), (0, 2) for vertex 0, 1, 2
    texCoords = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

    // u = 0 is the left edge, v = 0 the top edge
    gl_Position = vec4(texCoords.x * 2.0 - 1.0, 1.0 - texCoords.y * 2.0, 0.0, 1.0);
}
