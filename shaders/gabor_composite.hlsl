cbuffer GaborCompositeConstants : register(b4) { uint pixelCount; float exposure; uint debugView; uint padding; };
StructuredBuffer<float4> gSceneHdr : register(t20);
StructuredBuffer<float4> gGaborResolved : register(t21);
RWStructuredBuffer<float4> gCompositedHdr : register(u10);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pixelCount) return;
    float4 volume = gGaborResolved[id.x];
    float3 color = volume.rgb + volume.a * gSceneHdr[id.x].rgb;
    gCompositedHdr[id.x] = float4(color * exposure, 1.0);
}
