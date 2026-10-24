#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include "dve/gpu_material_buffer.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {
// Scalar/float3 layouts are mirrored in common/live_clear_glass.hlsli.
struct GpuOpticalNode {
    Float3 minimum{}; std::uint32_t escape{};
    Float3 maximum{}; std::uint32_t firstTriangle{};
    std::uint32_t triangleCount{}, pad0{}, pad1{}, pad2{};
};
struct GpuOpticalTriangle {
    Float3 p0{}, p1{}, p2{}, n0{}, n1{}, n2{}, c0{}, c1{}, c2{};
    std::uint32_t material{};
};
struct GpuOpticalSettings {
    std::uint32_t nodeCount{}, triangleCount{}, materialCount{}, maximumInteractions{32};
    std::uint32_t maximumPaths{256}, mediumCount{}; float rayEpsilonMeters{1e-5F}, maximumDistanceMeters{1000};
    std::array<std::uint32_t,8> media{}; // material index + 1; zero denotes air
};
static_assert(sizeof(GpuOpticalNode)==48 && sizeof(GpuOpticalTriangle)==112);
static_assert(sizeof(GpuOpticalSettings)==64);
static_assert(offsetof(GpuOpticalNode,escape)==12 && offsetof(GpuOpticalNode,maximum)==16 && offsetof(GpuOpticalNode,triangleCount)==32);
static_assert(offsetof(GpuOpticalTriangle,n0)==36 && offsetof(GpuOpticalTriangle,c0)==72 && offsetof(GpuOpticalTriangle,material)==108);
static_assert(offsetof(GpuOpticalSettings,media)==32);
struct LiveOpticalInstance {
    const CookedPolygonAsset* asset{};
    std::array<float,16> objectToWorld{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
};
struct LiveOpticalVolume { std::uint32_t material{}; double volume{}; };
struct LiveOpticalSceneData {
    std::vector<GpuOpticalNode> nodes;
    std::vector<GpuOpticalTriangle> triangles;
    std::vector<GpuMaterialRecord> materials;
    std::vector<LiveOpticalVolume> volumes;
    GpuOpticalSettings settings;
};
// Closed, outward-wound clear dielectrics; opaque constant/vertex-color receivers.
// Rejects unsupported secondary-hit shading instead of silently dropping it.
[[nodiscard]] bool build_live_optical_scene(std::span<const LiveOpticalInstance> instances,
    LiveOpticalSceneData& out, std::string* error=nullptr);
[[nodiscard]] bool set_live_optical_camera(LiveOpticalSceneData& scene,Float3 camera,
    std::string* error=nullptr);
class LiveOpticalSceneGpu {
public:
    explicit LiveOpticalSceneGpu(rhi::IDevice& device):device_(device){}
    ~LiveOpticalSceneGpu();
    LiveOpticalSceneGpu(const LiveOpticalSceneGpu&)=delete;
    LiveOpticalSceneGpu& operator=(const LiveOpticalSceneGpu&)=delete;
    bool initialize(std::string* error=nullptr);
    bool prepare(std::span<const LiveOpticalInstance> instances,Float3 camera,std::string* error=nullptr);
    void clear() noexcept;
    [[nodiscard]] rhi::BindGroupLayoutHandle layout() const noexcept{return layout_;}
    [[nodiscard]] rhi::BindGroupHandle group() const noexcept{return group_;}
    [[nodiscard]] const LiveOpticalSceneData& data() const noexcept{return data_;}
    [[nodiscard]] std::uint64_t builds() const noexcept{return builds_;}
private:
    rhi::IDevice& device_;
    rhi::BindGroupLayoutHandle layout_;
    rhi::BindGroupHandle group_;
    std::array<rhi::BufferHandle,4> buffers_{};
    LiveOpticalSceneData data_;
    std::vector<std::uint64_t> key_;
    std::uint64_t builds_{};
    Float3 camera_{};
    bool cameraValid_{};
};
} // namespace dve::render
