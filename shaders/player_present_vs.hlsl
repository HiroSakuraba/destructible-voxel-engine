// Fullscreen triangle for the player's present pass. No vertex buffer: positions come from
// SV_VertexID (indices 0, 1, 2), covering the viewport with one oversized triangle.
struct PresentVertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
PresentVertexOutput main(uint vertexId : SV_VertexID) {
    PresentVertexOutput output;
    const float2 uv = float2((vertexId << 1u) & 2u, vertexId & 2u);
    output.uv = uv;
    // Vulkan NDC +Y is down, so uv (0,0) is the top-left of the viewport and of the HDR image.
    output.position = float4(uv * 2.0F - 1.0F, 0.0F, 1.0F);
    return output;
}
