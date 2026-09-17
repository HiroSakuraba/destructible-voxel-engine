#include "phonon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

struct _IPLContext_t { int alive{1}; };
struct _IPLHRTF_t { int type{}; };
struct _IPLBinauralEffect_t { int resetCount{}; };

struct _IPLStaticMesh_t {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> materialIndices;
    std::vector<IPLMaterial> materials;
    bool added{};
};
struct _IPLScene_t {
    std::vector<IPLStaticMesh> meshes;
    std::uint64_t commits{};
};
struct _IPLSource_t {
    IPLSimulationInputs inputs{};
    IPLSimulationOutputs outputs{};
    bool added{};
};
struct _IPLSimulator_t {
    IPLScene scene{};
    IPLSimulationSharedInputs shared{};
    std::vector<IPLSource> sources;
    std::uint64_t commits{};
};

namespace {

IPLVector3 subtract(IPLVector3 a, IPLVector3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
IPLVector3 cross(IPLVector3 a, IPLVector3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
float dot(IPLVector3 a, IPLVector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(IPLVector3 value) { return std::sqrt(std::max(0.0F, dot(value, value))); }
IPLVector3 scaled(IPLVector3 value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

bool segment_triangle(IPLVector3 origin, IPLVector3 direction, float maximumDistance,
                      const IPLVector3& v0, const IPLVector3& v1, const IPLVector3& v2,
                      float& hitDistance) {
    constexpr float epsilon = 1.0e-6F;
    const IPLVector3 edge1 = subtract(v1, v0);
    const IPLVector3 edge2 = subtract(v2, v0);
    const IPLVector3 p = cross(direction, edge2);
    const float determinant = dot(edge1, p);
    if (std::abs(determinant) <= epsilon) return false;
    const float inverse = 1.0F / determinant;
    const IPLVector3 t = subtract(origin, v0);
    const float u = dot(t, p) * inverse;
    if (u < -epsilon || u > 1.0F + epsilon) return false;
    const IPLVector3 q = cross(t, edge1);
    const float v = dot(direction, q) * inverse;
    if (v < -epsilon || u + v > 1.0F + epsilon) return false;
    const float distanceAlong = dot(edge2, q) * inverse;
    if (distanceAlong <= epsilon || distanceAlong >= maximumDistance - epsilon) return false;
    hitDistance = distanceAlong;
    return true;
}

struct HitResult {
    bool hit{};
    float nearest{std::numeric_limits<float>::infinity()};
    IPLMaterial material{};
};

HitResult trace(IPLScene scene, IPLVector3 listener, IPLVector3 source) {
    HitResult result;
    if (!scene) return result;
    const IPLVector3 delta = subtract(source, listener);
    const float maximumDistance = length(delta);
    if (!(maximumDistance > 1.0e-5F)) return result;
    const IPLVector3 direction = scaled(delta, 1.0F / maximumDistance);
    for (IPLStaticMesh mesh : scene->meshes) {
        if (!mesh || !mesh->added) continue;
        for (std::size_t index = 0; index < mesh->triangles.size(); ++index) {
            const IPLTriangle& triangle = mesh->triangles[index];
            if (triangle.indices[0] < 0 || triangle.indices[1] < 0 || triangle.indices[2] < 0)
                continue;
            const std::size_t i0 = static_cast<std::size_t>(triangle.indices[0]);
            const std::size_t i1 = static_cast<std::size_t>(triangle.indices[1]);
            const std::size_t i2 = static_cast<std::size_t>(triangle.indices[2]);
            if (i0 >= mesh->vertices.size() || i1 >= mesh->vertices.size() ||
                i2 >= mesh->vertices.size()) continue;
            float hitDistance{};
            if (!segment_triangle(listener, direction, maximumDistance,
                                  mesh->vertices[i0], mesh->vertices[i1], mesh->vertices[i2],
                                  hitDistance)) continue;
            if (hitDistance < result.nearest) {
                result.hit = true;
                result.nearest = hitDistance;
                std::size_t materialIndex{};
                if (index < mesh->materialIndices.size() && mesh->materialIndices[index] >= 0)
                    materialIndex = static_cast<std::size_t>(mesh->materialIndices[index]);
                if (materialIndex < mesh->materials.size()) result.material = mesh->materials[materialIndex];
            }
        }
    }
    return result;
}

} // namespace

extern "C" {

IPLerror iplContextCreate(IPLContextSettings* settings, IPLContext* context) {
    if (!settings || !context || settings->version != STEAMAUDIO_VERSION) return IPL_STATUS_FAILURE;
    *context = new (std::nothrow) _IPLContext_t;
    return *context ? IPL_STATUS_SUCCESS : IPL_STATUS_OUTOFMEMORY;
}
void iplContextRelease(IPLContext* context) {
    if (context) { delete *context; *context = nullptr; }
}
IPLerror iplHRTFCreate(IPLContext context, IPLAudioSettings* audio,
                       IPLHRTFSettings* settings, IPLHRTF* hrtf) {
    if (!context || !audio || !settings || !hrtf || audio->frameSize <= 0) return IPL_STATUS_FAILURE;
    *hrtf = new (std::nothrow) _IPLHRTF_t;
    if (*hrtf) (*hrtf)->type = settings->type;
    return *hrtf ? IPL_STATUS_SUCCESS : IPL_STATUS_OUTOFMEMORY;
}
void iplHRTFRelease(IPLHRTF* hrtf) {
    if (hrtf) { delete *hrtf; *hrtf = nullptr; }
}
IPLerror iplBinauralEffectCreate(IPLContext context, IPLAudioSettings*,
                                 IPLBinauralEffectSettings* settings,
                                 IPLBinauralEffect* effect) {
    if (!context || !settings || !settings->hrtf || !effect) return IPL_STATUS_FAILURE;
    *effect = new (std::nothrow) _IPLBinauralEffect_t;
    return *effect ? IPL_STATUS_SUCCESS : IPL_STATUS_OUTOFMEMORY;
}
void iplBinauralEffectRelease(IPLBinauralEffect* effect) {
    if (effect) { delete *effect; *effect = nullptr; }
}
void iplBinauralEffectReset(IPLBinauralEffect effect) {
    if (effect) ++effect->resetCount;
}
IPLAudioEffectState iplBinauralEffectApply(IPLBinauralEffect effect,
                                            IPLBinauralEffectParams* params,
                                            IPLAudioBuffer* input,
                                            IPLAudioBuffer* output) {
    if (!effect || !params || !input || !output || input->numChannels != 1 ||
        output->numChannels != 2) return IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
    const float pan = std::clamp(params->direction.x, -1.0F, 1.0F);
    const float left = std::sqrt(0.5F * (1.0F - pan));
    const float right = std::sqrt(0.5F * (1.0F + pan));
    const float blend = std::clamp(params->spatialBlend, 0.0F, 1.0F);
    for (int i = 0; i < input->numSamples; ++i) {
        const float sample = input->data[0][i];
        output->data[0][i] = sample * ((1.0F - blend) * 0.70710678F + blend * left);
        output->data[1][i] = sample * ((1.0F - blend) * 0.70710678F + blend * right);
    }
    return IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
}

IPLerror iplSceneCreate(IPLContext context, IPLSceneSettings* settings, IPLScene* scene) {
    if (!context || !settings || !scene || settings->type != IPL_SCENETYPE_DEFAULT)
        return IPL_STATUS_FAILURE;
    *scene = new (std::nothrow) _IPLScene_t;
    return *scene ? IPL_STATUS_SUCCESS : IPL_STATUS_OUTOFMEMORY;
}
void iplSceneRelease(IPLScene* scene) {
    if (scene) { delete *scene; *scene = nullptr; }
}
void iplSceneCommit(IPLScene scene) { if (scene) ++scene->commits; }

IPLerror iplStaticMeshCreate(IPLScene scene, IPLStaticMeshSettings* settings,
                             IPLStaticMesh* staticMesh) {
    if (!scene || !settings || !staticMesh || settings->numVertices <= 0 ||
        settings->numTriangles <= 0 || settings->numMaterials <= 0 || !settings->vertices ||
        !settings->triangles || !settings->materialIndices || !settings->materials)
        return IPL_STATUS_FAILURE;
    auto* mesh = new (std::nothrow) _IPLStaticMesh_t;
    if (!mesh) return IPL_STATUS_OUTOFMEMORY;
    mesh->vertices.assign(settings->vertices, settings->vertices + settings->numVertices);
    mesh->triangles.assign(settings->triangles, settings->triangles + settings->numTriangles);
    mesh->materialIndices.assign(settings->materialIndices,
                                 settings->materialIndices + settings->numTriangles);
    mesh->materials.assign(settings->materials, settings->materials + settings->numMaterials);
    *staticMesh = mesh;
    return IPL_STATUS_SUCCESS;
}
void iplStaticMeshRelease(IPLStaticMesh* staticMesh) {
    if (staticMesh) { delete *staticMesh; *staticMesh = nullptr; }
}
void iplStaticMeshAdd(IPLStaticMesh staticMesh, IPLScene scene) {
    if (!staticMesh || !scene || staticMesh->added) return;
    staticMesh->added = true;
    scene->meshes.push_back(staticMesh);
}
void iplStaticMeshRemove(IPLStaticMesh staticMesh, IPLScene scene) {
    if (!staticMesh || !scene) return;
    staticMesh->added = false;
    scene->meshes.erase(std::remove(scene->meshes.begin(), scene->meshes.end(), staticMesh),
                        scene->meshes.end());
}

IPLerror iplSimulatorCreate(IPLContext context, IPLSimulationSettings* settings,
                            IPLSimulator* simulator) {
    if (!context || !settings || !simulator ||
        !(settings->flags & IPL_SIMULATIONFLAGS_DIRECT)) return IPL_STATUS_FAILURE;
    *simulator = new (std::nothrow) _IPLSimulator_t;
    return *simulator ? IPL_STATUS_SUCCESS : IPL_STATUS_OUTOFMEMORY;
}
void iplSimulatorRelease(IPLSimulator* simulator) {
    if (simulator) { delete *simulator; *simulator = nullptr; }
}
void iplSimulatorSetScene(IPLSimulator simulator, IPLScene scene) {
    if (simulator) simulator->scene = scene;
}
void iplSimulatorCommit(IPLSimulator simulator) { if (simulator) ++simulator->commits; }
void iplSimulatorSetSharedInputs(IPLSimulator simulator, IPLSimulationFlags,
                                 IPLSimulationSharedInputs* sharedInputs) {
    if (simulator && sharedInputs) simulator->shared = *sharedInputs;
}

IPLerror iplSourceCreate(IPLSimulator simulator, IPLSourceSettings* settings, IPLSource* source) {
    if (!simulator || !settings || !source || !(settings->flags & IPL_SIMULATIONFLAGS_DIRECT))
        return IPL_STATUS_FAILURE;
    *source = new (std::nothrow) _IPLSource_t;
    return *source ? IPL_STATUS_SUCCESS : IPL_STATUS_OUTOFMEMORY;
}
void iplSourceRelease(IPLSource* source) {
    if (source) { delete *source; *source = nullptr; }
}
void iplSourceAdd(IPLSource source, IPLSimulator simulator) {
    if (!source || !simulator || source->added) return;
    source->added = true;
    simulator->sources.push_back(source);
}
void iplSourceRemove(IPLSource source, IPLSimulator simulator) {
    if (!source || !simulator) return;
    source->added = false;
    simulator->sources.erase(std::remove(simulator->sources.begin(), simulator->sources.end(), source),
                             simulator->sources.end());
}
void iplSourceSetInputs(IPLSource source, IPLSimulationFlags, IPLSimulationInputs* inputs) {
    if (source && inputs) source->inputs = *inputs;
}
void iplSourceGetOutputs(IPLSource source, IPLSimulationFlags, IPLSimulationOutputs* outputs) {
    if (source && outputs) *outputs = source->outputs;
}

void iplSimulatorRunDirect(IPLSimulator simulator) {
    if (!simulator) return;
    for (IPLSource source : simulator->sources) {
        if (!source || !source->added) continue;
        const IPLVector3 listener = simulator->shared.listener.origin;
        const IPLVector3 emitter = source->inputs.source.origin;
        const float distanceMeters = length(subtract(emitter, listener));
        auto& direct = source->outputs.direct;
        direct.distanceAttenuation = 1.0F / std::max(1.0F, distanceMeters);
        direct.airAbsorption[0] = std::exp(-distanceMeters * 0.0002F);
        direct.airAbsorption[1] = std::exp(-distanceMeters * 0.0010F);
        direct.airAbsorption[2] = std::exp(-distanceMeters * 0.0030F);
        direct.directivity = 1.0F;
        const HitResult hit = trace(simulator->scene, listener, emitter);
        // Steam Audio's real direct output uses visibility semantics: 1=open, 0=occluded.
        direct.occlusion = hit.hit ? 0.0F : 1.0F;
        for (int band = 0; band < IPL_NUM_BANDS; ++band)
            direct.transmission[band] = hit.hit ? std::clamp(hit.material.transmission[band], 0.0F, 1.0F)
                                                : 1.0F;
    }
}

} // extern "C"
