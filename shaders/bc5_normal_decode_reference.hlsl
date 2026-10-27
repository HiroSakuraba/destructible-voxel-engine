// T4 GPU conformance probe: runs the production BC5 normal decoder (material_mapping.hlsli) on a
// BC5_UNORM texture through a sampled image, so the CPU mirror decode_bc5_normal_sample() can be
// compared with real GPU texture decode + HLSL math. Not used by any render pass.
#include "common/material_mapping.hlsli"
#include "common/live_bindings.hlsli"

DVE_LIVE_IMAGE(0,0) Texture2D<float4> gBc5Normals : register(t0);
DVE_LIVE_IMAGE(0,0) SamplerState gBc5Sampler : register(s0);
DVE_LIVE_BIND(1,0) RWStructuredBuffer<float4> gBc5DecodeResults : register(u0);

// The texture is 32x4: eight constant 4x4 BC5 blocks. Thread i samples the center of block i.
[numthreads(8,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = float2((float(id.x) * 4.0f + 2.0f) / 32.0f, 0.5f);
    const float4 sampled = gBc5Normals.SampleLevel(gBc5Sampler, uv, 0.0f);
    // [2i]: decoded with the BC5 flag, [2i+1]: raw sampled RG, and the flag-clear passthrough in .zw
    gBc5DecodeResults[id.x * 2u] =
        float4(DecodeMaterialNormalSample(sampled.xyz, kMaterialFlagNormalBc5), 1.0f);
    const float3 legacy = DecodeMaterialNormalSample(sampled.xyz, 0u);
    gBc5DecodeResults[id.x * 2u + 1u] = float4(sampled.xy, legacy.x == sampled.x ? 1.0f : 0.0f,
                                               legacy.y == sampled.y ? 1.0f : 0.0f);
}
