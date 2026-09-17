#version 450
// Readable source equivalent of the dependency-free SPIR-V assembled by
// tests/test_vulkan_graphics_device.cpp. Vertex data is deliberately generated from gl_VertexIndex
// so the test isolates Vulkan indexed-draw and render-pass behavior from vertex-format reflection.
const vec2 kPositions[3] = vec2[3](
    vec2(-0.8, -0.8),
    vec2( 0.8, -0.8),
    vec2( 0.0,  0.8));
void main() { gl_Position = vec4(kPositions[gl_VertexIndex], 0.5, 1.0); }
