#version 450

layout(set = 0, binding = 0) uniform sampler2D spriteIndexTexture;
layout(std430, set = 1, binding = 0) readonly buffer SpritePalette {
    uint colors[256];
} spritePalette;

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

vec4 unpackRgba8(uint packed) {
    return vec4(
        float(packed & 0xFFu),
        float((packed >> 8u) & 0xFFu),
        float((packed >> 16u) & 0xFFu),
        float((packed >> 24u) & 0xFFu)) / 255.0;
}

void main() {
    float encoded = texture(spriteIndexTexture, inUv).r;
    uint paletteIndex = min(255u, uint(round(clamp(encoded, 0.0, 1.0) * 255.0)));
    outColor = unpackRgba8(spritePalette.colors[paletteIndex]) * inColor;
}
