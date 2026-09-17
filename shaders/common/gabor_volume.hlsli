#ifndef DVE_GABOR_VOLUME_HLSLI
#define DVE_GABOR_VOLUME_HLSLI
struct GaborPrimitive {
    float3 center; float opacity;
    float3 scale; float frequency;
    float4 rotation;
    float3 albedo; float extent;
    uint lodLevel; uint orientationBin; uint2 padding;
};
float3 GaborRotateInverse(float4 q, float3 v) {
    q = normalize(q);
    float3 t = 2.0 * cross(q.xyz, v);
    return v - q.w * t + cross(q.xyz, t);
}
float GaborDensity(GaborPrimitive p, float3 position, float lodWeight) {
    float3 local = GaborRotateInverse(p.rotation, position - p.center) / max(p.scale, 1.0e-5.xxx);
    float radius2 = dot(local, local);
    if (radius2 > p.extent * p.extent) return 0.0;
    float envelope = exp(-0.5 * radius2);
    float carrier = p.frequency <= 1.0e-5 ? 1.0 : 0.5 + 0.5 * cos(p.frequency * local.x);
    return max(0.0, p.opacity * envelope * carrier * lodWeight);
}
#endif
