#version 460 core

layout(location = 0) in vec2 texCoords;

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D imageSampler;

void main()
{
    fragColor = texture(imageSampler, texCoords);
}
