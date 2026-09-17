#ifndef DVE_MATERIAL_MAPPING_HLSLI
#define DVE_MATERIAL_MAPPING_HLSLI

static const uint kMaterialMappingUV0 = 0u;
static const uint kMaterialMappingUV1 = 1u;
static const uint kMaterialMappingWorldTriplanar = 2u;
static const uint kMaterialMappingObjectTriplanar = 3u;

static const uint kHeightMappingOff = 0u;
static const uint kHeightMappingOffsetParallax = 1u;
static const uint kHeightMappingSteepParallax = 2u;
static const uint kHeightMappingParallaxOcclusion = 3u;
static const uint kNoMaterialTexture = 0xFFFFFFFFu;
static const uint kMaterialFlagDoubleSided = 1u << 0u;
static const uint kMaterialFlagBaseColorSrgb = 1u << 1u;
static const uint kMaterialFlagEmissiveSrgb = 1u << 2u;
static const uint kMaterialFlagDetailBaseColorSrgb = 1u << 3u;

struct PolygonMaterialMappingRecord {
    uint mappingMode;
    uint heightMode;
    uint baseColorTexture;
    uint metallicRoughnessTexture;

    uint normalTexture;
    uint emissiveTexture;
    uint opacityTexture;
    uint heightTexture;

    uint detailBaseColorTexture;
    uint detailNormalTexture;
    uint detailRoughnessTexture;
    uint baseTexcoord;

    uint normalTexcoord;
    uint heightTexcoord;
    uint detailTexcoord;
    uint mappingFlags;

    float baseScaleX;
    float baseScaleY;
    float baseOffsetX;
    float baseOffsetY;

    float baseRotationRadians;
    float triplanarScale;
    float triplanarBlendSharpness;
    float detailScaleX;

    float detailScaleY;
    float detailOffsetX;
    float detailOffsetY;
    float detailRotationRadians;

    float detailColorStrength;
    float detailNormalStrength;
    float detailRoughnessStrength;
    float detailFadeStartMeters;

    float detailFadeEndMeters;
    float heightScale;
    float heightReferencePlane;
    float maximumParallaxDistanceMeters;

    uint minimumHeightSteps;
    uint maximumHeightSteps;
    uint refinementSteps;
    float normalScale;

    float detailNormalScale;
    float alphaCutoff;
    uint reserved1;
    uint reserved2;
};

float2 TransformMaterialUV(float2 uv, float2 scale, float2 offset, float rotationRadians) {
    const float cosine = cos(rotationRadians);
    const float sine = sin(rotationRadians);
    const float2 scaled = uv * scale;
    return float2(cosine * scaled.x - sine * scaled.y,
                  sine * scaled.x + cosine * scaled.y) + offset;
}

float3 MaterialTriplanarWeights(float3 surfaceNormal, float blendSharpness) {
    const float3 powered = pow(max(abs(surfaceNormal), 1.0e-6f), max(blendSharpness, 1.0e-4f));
    return powered / max(powered.x + powered.y + powered.z, 1.0e-6f);
}

void MaterialTriplanarCoordinates(float3 position, float scale, out float2 xProjection,
                                  out float2 yProjection, out float2 zProjection) {
    const float safeScale = max(abs(scale), 1.0e-6f);
    xProjection = position.zy * safeScale;
    yProjection = position.xz * safeScale;
    zProjection = position.xy * safeScale;
}

float MaterialDetailFade(float cameraDistanceMeters, float fadeStartMeters,
                         float fadeEndMeters) {
    if (fadeEndMeters <= fadeStartMeters) return cameraDistanceMeters <= fadeStartMeters ? 1.0f : 0.0f;
    return 1.0f - saturate((cameraDistanceMeters - fadeStartMeters) /
                          (fadeEndMeters - fadeStartMeters));
}

float3 ReorientTriplanarNormalX(float3 tangentNormal, float axisSign) {
    return normalize(float3(tangentNormal.z * axisSign, tangentNormal.y, tangentNormal.x));
}
float3 ReorientTriplanarNormalY(float3 tangentNormal, float axisSign) {
    return normalize(float3(tangentNormal.x, tangentNormal.z * axisSign, tangentNormal.y));
}
float3 ReorientTriplanarNormalZ(float3 tangentNormal, float axisSign) {
    return normalize(float3(tangentNormal.x, tangentNormal.y, tangentNormal.z * axisSign));
}

#endif
