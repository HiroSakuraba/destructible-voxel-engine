#version 450

layout(location = 0) in vec3 inPositionNdc;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

void main() {
    gl_Position = vec4(inPositionNdc, 1.0);
    outUv = inUv;
    outColor = inColor;
}
