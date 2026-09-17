cbuffer GaborTemporalConstants : register(b4) {
    uint pixelCount; uint resetHistory; float temporalWeight; float rejectionThreshold;
};
StructuredBuffer<float4> gGaborCurrent : register(t20);
StructuredBuffer<float4> gGaborHistory : register(t21);
StructuredBuffer<float> gGaborCurrentDepth : register(t22);
StructuredBuffer<float> gGaborHistoryDepth : register(t23);
RWStructuredBuffer<float4> gGaborResolved : register(u10);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pixelCount) return;
    float4 current = gGaborCurrent[id.x];
    float depthDelta = abs(gGaborCurrentDepth[id.x] - gGaborHistoryDepth[id.x]);
    float weight = (resetHistory != 0u || depthDelta > rejectionThreshold) ? 0.0 : saturate(temporalWeight);
    gGaborResolved[id.x] = lerp(current, gGaborHistory[id.x], weight);
}
