#version 460 core

layout(location = 0) out vec2 texCoords;

void main() 
{
    // Generate UV-coördinates (0.0 to 2.0) based on the vertex ID (0, 1, or 2)
    texCoords = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    
    // Convert UV coordinates to Normalized Device Coordinates (NDC) (range: -1.0 to 1.0)
    gl_Position = vec4(texCoords * 2.0 - 1.0, 0.0, 1.0);
}
