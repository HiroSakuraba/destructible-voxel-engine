#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define STEAMAUDIO_VERSION 0x00040801u
#define IPL_NUM_BANDS 3

typedef int32_t IPLint32;
typedef uint32_t IPLuint32;
typedef uint8_t IPLuint8;
typedef float IPLfloat32;
typedef size_t IPLsize;
typedef const char* IPLstring;
typedef enum IPLbool { IPL_FALSE = 0, IPL_TRUE = 1 } IPLbool;
typedef enum IPLerror {
    IPL_STATUS_SUCCESS = 0,
    IPL_STATUS_FAILURE = 1,
    IPL_STATUS_OUTOFMEMORY = 2,
    IPL_STATUS_INITIALIZATION = 3
} IPLerror;
typedef enum IPLSIMDLevel { IPL_SIMDLEVEL_SSE2 = 0, IPL_SIMDLEVEL_SSE4 = 1, IPL_SIMDLEVEL_AVX = 2, IPL_SIMDLEVEL_AVX2 = 3 } IPLSIMDLevel;
typedef enum IPLContextFlags { IPL_CONTEXTFLAGS_NONE = 0, IPL_CONTEXTFLAGS_VALIDATION = 1 } IPLContextFlags;
typedef enum IPLHRTFType { IPL_HRTFTYPE_DEFAULT = 0, IPL_HRTFTYPE_SOFA = 1 } IPLHRTFType;
typedef enum IPLHRTFInterpolation { IPL_HRTFINTERPOLATION_NEAREST = 0, IPL_HRTFINTERPOLATION_BILINEAR = 1 } IPLHRTFInterpolation;
typedef enum IPLAudioEffectState { IPL_AUDIOEFFECTSTATE_TAILCOMPLETE = 0, IPL_AUDIOEFFECTSTATE_TAILREMAINING = 1 } IPLAudioEffectState;
typedef enum IPLSceneType { IPL_SCENETYPE_DEFAULT = 0, IPL_SCENETYPE_EMBREE = 1, IPL_SCENETYPE_RADEONRAYS = 2, IPL_SCENETYPE_CUSTOM = 3 } IPLSceneType;
typedef enum IPLReflectionEffectType { IPL_REFLECTIONEFFECTTYPE_CONVOLUTION = 0 } IPLReflectionEffectType;
typedef enum IPLSimulationFlags { IPL_SIMULATIONFLAGS_DIRECT = 1 << 0, IPL_SIMULATIONFLAGS_REFLECTIONS = 1 << 1, IPL_SIMULATIONFLAGS_PATHING = 1 << 2 } IPLSimulationFlags;
typedef enum IPLDirectSimulationFlags {
    IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION = 1 << 0,
    IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION = 1 << 1,
    IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY = 1 << 2,
    IPL_DIRECTSIMULATIONFLAGS_OCCLUSION = 1 << 3,
    IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION = 1 << 4
} IPLDirectSimulationFlags;
typedef enum IPLDistanceAttenuationModelType { IPL_DISTANCEATTENUATIONTYPE_DEFAULT = 0, IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE = 1, IPL_DISTANCEATTENUATIONTYPE_CALLBACK = 2 } IPLDistanceAttenuationModelType;
typedef enum IPLAirAbsorptionModelType { IPL_AIRABSORPTIONTYPE_DEFAULT = 0, IPL_AIRABSORPTIONTYPE_EXPONENTIAL = 1, IPL_AIRABSORPTIONTYPE_CALLBACK = 2 } IPLAirAbsorptionModelType;
typedef enum IPLOcclusionType { IPL_OCCLUSIONTYPE_RAYCAST = 0, IPL_OCCLUSIONTYPE_VOLUMETRIC = 1 } IPLOcclusionType;
typedef enum IPLDirectEffectFlags { IPL_DIRECTEFFECTFLAGS_NONE = 0 } IPLDirectEffectFlags;
typedef enum IPLTransmissionType { IPL_TRANSMISSIONTYPE_FREQINDEPENDENT = 0, IPL_TRANSMISSIONTYPE_FREQDEPENDENT = 1 } IPLTransmissionType;

typedef struct _IPLContext_t* IPLContext;
typedef struct _IPLHRTF_t* IPLHRTF;
typedef struct _IPLBinauralEffect_t* IPLBinauralEffect;
typedef struct _IPLScene_t* IPLScene;
typedef struct _IPLStaticMesh_t* IPLStaticMesh;
typedef struct _IPLSimulator_t* IPLSimulator;
typedef struct _IPLSource_t* IPLSource;
typedef void* IPLEmbreeDevice;
typedef void* IPLRadeonRaysDevice;
typedef void* IPLOpenCLDevice;
typedef void* IPLTrueAudioNextDevice;
typedef void* IPLProbeBatch;

typedef struct IPLVector3 { IPLfloat32 x, y, z; } IPLVector3;
typedef struct IPLMatrix4x4 { IPLfloat32 elements[4][4]; } IPLMatrix4x4;
typedef struct IPLCoordinateSpace3 { IPLVector3 right, up, ahead, origin; } IPLCoordinateSpace3;
typedef struct IPLTriangle { IPLint32 indices[3]; } IPLTriangle;
typedef struct IPLMaterial { IPLfloat32 absorption[IPL_NUM_BANDS]; IPLfloat32 scattering; IPLfloat32 transmission[IPL_NUM_BANDS]; } IPLMaterial;

typedef struct IPLContextSettings { IPLuint32 version; void* logCallback; void* allocateCallback; void* freeCallback; IPLSIMDLevel simdLevel; IPLContextFlags flags; } IPLContextSettings;
typedef struct IPLAudioSettings { IPLint32 samplingRate; IPLint32 frameSize; } IPLAudioSettings;
typedef struct IPLHRTFSettings { IPLHRTFType type; const char* sofaFileName; IPLfloat32 volume; int normType; } IPLHRTFSettings;
typedef struct IPLBinauralEffectSettings { IPLHRTF hrtf; } IPLBinauralEffectSettings;
typedef struct IPLBinauralEffectParams { IPLVector3 direction; IPLHRTFInterpolation interpolation; IPLfloat32 spatialBlend; IPLHRTF hrtf; IPLfloat32* peakDelays; } IPLBinauralEffectParams;
typedef struct IPLAudioBuffer { IPLint32 numChannels; IPLint32 numSamples; IPLfloat32** data; } IPLAudioBuffer;

typedef struct IPLSceneSettings {
    IPLSceneType type;
    void* closestHitCallback;
    void* anyHitCallback;
    void* batchedClosestHitCallback;
    void* batchedAnyHitCallback;
    void* userData;
    IPLEmbreeDevice embreeDevice;
    IPLRadeonRaysDevice radeonRaysDevice;
} IPLSceneSettings;
typedef struct IPLStaticMeshSettings {
    IPLint32 numVertices;
    IPLint32 numTriangles;
    IPLint32 numMaterials;
    IPLVector3* vertices;
    IPLTriangle* triangles;
    IPLint32* materialIndices;
    IPLMaterial* materials;
} IPLStaticMeshSettings;

typedef struct IPLSimulationSettings {
    IPLSimulationFlags flags;
    IPLSceneType sceneType;
    IPLReflectionEffectType reflectionType;
    IPLint32 maxNumOcclusionSamples;
    IPLint32 maxNumRays;
    IPLint32 numDiffuseSamples;
    IPLfloat32 maxDuration;
    IPLint32 maxOrder;
    IPLint32 maxNumSources;
    IPLint32 numThreads;
    IPLint32 rayBatchSize;
    IPLint32 numVisSamples;
    IPLint32 samplingRate;
    IPLint32 frameSize;
    IPLOpenCLDevice openCLDevice;
    IPLRadeonRaysDevice radeonRaysDevice;
    IPLTrueAudioNextDevice tanDevice;
} IPLSimulationSettings;
typedef struct IPLSourceSettings { IPLSimulationFlags flags; } IPLSourceSettings;
typedef struct IPLDistanceAttenuationModel { IPLDistanceAttenuationModelType type; IPLfloat32 minDistance; void* callback; void* userData; IPLbool dirty; } IPLDistanceAttenuationModel;
typedef struct IPLAirAbsorptionModel { IPLAirAbsorptionModelType type; IPLfloat32 coefficients[IPL_NUM_BANDS]; void* callback; void* userData; IPLbool dirty; } IPLAirAbsorptionModel;
typedef struct IPLDirectivity { IPLfloat32 dipoleWeight; IPLfloat32 dipolePower; void* callback; void* userData; } IPLDirectivity;
typedef struct IPLBakedDataIdentifier { int type; int variation; } IPLBakedDataIdentifier;
typedef struct IPLSimulationInputs {
    IPLSimulationFlags flags;
    IPLDirectSimulationFlags directFlags;
    IPLCoordinateSpace3 source;
    IPLDistanceAttenuationModel distanceAttenuationModel;
    IPLAirAbsorptionModel airAbsorptionModel;
    IPLDirectivity directivity;
    IPLOcclusionType occlusionType;
    IPLfloat32 occlusionRadius;
    IPLint32 numOcclusionSamples;
    IPLfloat32 reverbScale[IPL_NUM_BANDS];
    IPLfloat32 hybridReverbTransitionTime;
    IPLfloat32 hybridReverbOverlapPercent;
    IPLbool baked;
    IPLBakedDataIdentifier bakedDataIdentifier;
    IPLProbeBatch pathingProbes;
    IPLfloat32 visRadius;
    IPLfloat32 visThreshold;
    IPLfloat32 visRange;
    IPLint32 pathingOrder;
    IPLbool enableValidation;
    IPLbool findAlternatePaths;
    IPLint32 numTransmissionRays;
    void* deviationModel;
} IPLSimulationInputs;
typedef struct IPLSimulationSharedInputs {
    IPLCoordinateSpace3 listener;
    IPLint32 numRays;
    IPLint32 numBounces;
    IPLfloat32 duration;
    IPLint32 order;
    IPLfloat32 irradianceMinDistance;
    void* pathingVisCallback;
    void* pathingUserData;
} IPLSimulationSharedInputs;
typedef struct IPLDirectEffectParams {
    IPLDirectEffectFlags flags;
    IPLTransmissionType transmissionType;
    IPLfloat32 distanceAttenuation;
    IPLfloat32 airAbsorption[IPL_NUM_BANDS];
    IPLfloat32 directivity;
    IPLfloat32 occlusion;
    IPLfloat32 transmission[IPL_NUM_BANDS];
} IPLDirectEffectParams;
typedef struct IPLReflectionEffectParams { int unused; } IPLReflectionEffectParams;
typedef struct IPLPathEffectParams { int unused; } IPLPathEffectParams;
typedef struct IPLSimulationOutputs { IPLDirectEffectParams direct; IPLReflectionEffectParams reflections; IPLPathEffectParams pathing; } IPLSimulationOutputs;

IPLerror iplContextCreate(IPLContextSettings*, IPLContext*);
void iplContextRelease(IPLContext*);
IPLerror iplHRTFCreate(IPLContext, IPLAudioSettings*, IPLHRTFSettings*, IPLHRTF*);
void iplHRTFRelease(IPLHRTF*);
IPLerror iplBinauralEffectCreate(IPLContext, IPLAudioSettings*, IPLBinauralEffectSettings*, IPLBinauralEffect*);
void iplBinauralEffectRelease(IPLBinauralEffect*);
void iplBinauralEffectReset(IPLBinauralEffect);
IPLAudioEffectState iplBinauralEffectApply(IPLBinauralEffect, IPLBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*);

IPLerror iplSceneCreate(IPLContext, IPLSceneSettings*, IPLScene*);
void iplSceneRelease(IPLScene*);
void iplSceneCommit(IPLScene);
IPLerror iplStaticMeshCreate(IPLScene, IPLStaticMeshSettings*, IPLStaticMesh*);
void iplStaticMeshRelease(IPLStaticMesh*);
void iplStaticMeshAdd(IPLStaticMesh, IPLScene);
void iplStaticMeshRemove(IPLStaticMesh, IPLScene);
IPLerror iplSimulatorCreate(IPLContext, IPLSimulationSettings*, IPLSimulator*);
void iplSimulatorRelease(IPLSimulator*);
void iplSimulatorSetScene(IPLSimulator, IPLScene);
void iplSimulatorCommit(IPLSimulator);
void iplSimulatorSetSharedInputs(IPLSimulator, IPLSimulationFlags, IPLSimulationSharedInputs*);
void iplSimulatorRunDirect(IPLSimulator);
IPLerror iplSourceCreate(IPLSimulator, IPLSourceSettings*, IPLSource*);
void iplSourceRelease(IPLSource*);
void iplSourceAdd(IPLSource, IPLSimulator);
void iplSourceRemove(IPLSource, IPLSimulator);
void iplSourceSetInputs(IPLSource, IPLSimulationFlags, IPLSimulationInputs*);
void iplSourceGetOutputs(IPLSource, IPLSimulationFlags, IPLSimulationOutputs*);

#ifdef __cplusplus
}
#endif
