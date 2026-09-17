#ifndef DVE_RENDER_ENVIRONMENT_HLSLI
#define DVE_RENDER_ENVIRONMENT_HLSLI

static const uint kTonemapACES = 0u;
static const uint kTonemapReinhard = 1u;
static const uint kTonemapClamp = 2u;
static const uint kGlobalIlluminationOff = 0u;
static const uint kGlobalIlluminationAmbientHemisphere = 1u;
static const uint kGlobalIlluminationVoxelOneBounce = 2u;
static const uint kShadowOff = 0u;
static const uint kShadowHard = 1u;
static const uint kShadowSoft = 2u;
static const uint kShadowContact = 3u;
static const uint kShadowHybrid = 4u;

cbuffer RenderEnvironmentConstants : register(b2) {
    float3 gSunDirection;
    float gSunIntensity;
    float3 gSunColor;
    float gSubsurfaceMaxDistanceMeters;
    float3 gSkyColor;
    float gExposure;
    float3 gGroundColor;
    uint gTonemapOperator;
    float3 gGlobalTint;
    float gBloomThreshold;
    float gBloomIntensity;
    float gBloomRadius;
    uint gImageWidth;
    uint gImageHeight;

    uint gGlobalIlluminationMode;
    float gGlobalIlluminationIntensity;
    float gGlobalIlluminationMaxDistanceMeters;
    uint gGlobalIlluminationSamples;

    uint gShadowMode;
    float gShadowStrength;
    float gShadowSoftnessRadians;
    uint gShadowSamples;

    float gShadowMaxDistanceMeters;
    float gContactShadowDistanceMeters;
    float gShadowBiasMeters;
    float gMetersPerVoxel;
}

float MetersToVoxelUnits(float meters) {
    return max(0.0F, meters) / max(gMetersPerVoxel, 1.0e-6F);
}

float VoxelUnitsToMeters(float voxels) {
    return max(0.0F, voxels) * max(gMetersPerVoxel, 1.0e-6F);
}

#endif // DVE_RENDER_ENVIRONMENT_HLSLI
