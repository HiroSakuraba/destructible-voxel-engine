#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/voxel_material_policy.hpp"

namespace dve {

enum class Render3DDiagnosticSeverity : std::uint8_t { Information, Warning, Error };

enum class Render3DBudgetProfile : std::uint8_t { Unrestricted, Mobile, Desktop, HighEnd };

struct Render3DCameraBudget {
    std::uint64_t maximumDrawCalls{};
    std::uint64_t maximumTriangles{};
    std::uint64_t maximumVertices{};
    std::uint64_t maximumInstances{};
    std::uint64_t maximumMaterials{};
    std::uint64_t maximumPipelines{};
    std::uint64_t maximumTextures{};
    std::uint64_t maximumResidentTextureBytes{};
    std::uint64_t maximumSkinnedVertices{};
    std::uint64_t maximumShadowDraws{};
    std::uint64_t maximumVisibleLights{};
    std::uint64_t maximumClusterLightReferences{};
    std::uint16_t maximumDepthComplexity{};
};

[[nodiscard]] Render3DCameraBudget render3d_budget_for_profile(
    Render3DBudgetProfile profile, std::uint32_t viewportWidth,
    std::uint32_t viewportHeight) noexcept;

enum class Render3DPipelineBreakReason : std::uint16_t {
    NoBreak = 0U,
    Mesh = 1U << 0U,
    Material = 1U << 1U,
    Shader = 1U << 2U,
    Pipeline = 1U << 3U,
    VertexLayout = 1U << 4U,
    RenderPass = 1U << 5U,
    BlendState = 1U << 6U,
    DepthState = 1U << 7U,
    RasterState = 1U << 8U,
    SkinningMode = 1U << 9U,
};

[[nodiscard]] constexpr Render3DPipelineBreakReason operator|(
    Render3DPipelineBreakReason left, Render3DPipelineBreakReason right) noexcept {
    return static_cast<Render3DPipelineBreakReason>(
        static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}
[[nodiscard]] constexpr bool has_render3d_break_reason(
    Render3DPipelineBreakReason value, Render3DPipelineBreakReason reason) noexcept {
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(reason)) != 0U;
}

struct Render3DDrawSubmission {
    std::uint64_t owner{};
    std::uint64_t submissionOrder{};
    std::string ownerName;
    std::string meshAsset;
    std::uint32_t submeshIndex{};
    std::string materialAsset;
    std::string shader;
    std::string pipeline;
    std::string vertexLayout;
    std::string renderPass{"Main"};
    std::string blendState{"Opaque"};
    std::string depthState{"ReadWrite"};
    std::string rasterState{"BackFace"};
    std::string skeletonAsset;
    std::string animationAsset;
    std::uint64_t vertexCount{};
    std::uint64_t triangleCount{};
    std::uint64_t instanceCount{1U};
    bool visible{true};
    bool occlusionCulled{};
    bool castsShadow{};
    bool skinned{};
    bool gpuSkinned{};
};

struct Render3DPipelineBreakDiagnostic {
    std::size_t itemIndex{};
    std::uint64_t previousOwner{};
    std::uint64_t owner{};
    Render3DPipelineBreakReason reasons{Render3DPipelineBreakReason::NoBreak};
    std::string summary;
};

struct Render3DTextureResidencyRecord {
    std::string asset;
    std::string sampler;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t totalMipCount{};
    std::uint32_t firstResidentMip{};
    std::uint32_t residentMipCount{};
    std::uint64_t residentBytes{};
    bool resident{};
};

struct Render3DResidencyEntry {
    std::string asset;
    std::string detail;
    bool resident{};
    std::uint64_t estimatedBytes{};
    std::uint64_t references{};
};

struct Render3DResidencyDiagnostic {
    std::vector<Render3DResidencyEntry> meshes;
    std::vector<Render3DResidencyEntry> materials;
    std::vector<Render3DResidencyEntry> shaders;
    std::vector<Render3DResidencyEntry> pipelines;
    std::vector<Render3DResidencyEntry> textures;
    std::uint64_t residentTextureBytes{};
    std::uint64_t missingMeshes{};
    std::uint64_t missingMaterials{};
    std::uint64_t missingShaders{};
    std::uint64_t missingPipelines{};
    std::uint64_t missingTextures{};
};

struct Render3DSkinningRecord {
    std::uint64_t owner{};
    std::string ownerName;
    std::string skeletonAsset;
    std::string animationAsset;
    std::uint64_t skinnedVertices{};
    std::uint32_t boneCount{};
    std::uint32_t dispatchCount{};
    bool gpu{};
};

struct Render3DSkinningDiagnostic {
    std::uint64_t cpuSkinnedVertices{};
    std::uint64_t gpuSkinnedVertices{};
    std::uint64_t totalSkinnedVertices{};
    std::uint64_t cpuObjects{};
    std::uint64_t gpuObjects{};
    std::uint64_t gpuDispatches{};
    std::uint64_t maximumBonesPerObject{};
    std::vector<Render3DSkinningRecord> records;
};

struct Render3DLodDecision {
    std::uint64_t owner{};
    std::string ownerName;
    std::string meshAsset;
    std::uint32_t selectedLod{};
    std::uint32_t availableLodCount{};
    float projectedScreenFraction{};
    float transitionWeight{};
    std::optional<std::uint32_t> forcedLod;
    std::string reason;
};

struct Render3DShadowRecord {
    std::uint64_t owner{};
    std::string ownerName;
    std::uint32_t cascadeCount{};
    std::uint32_t drawCount{};
    std::uint64_t atlasPixels{};
    std::uint64_t occupiedAtlasPixels{};
    bool accepted{};
    std::string rejectionReason;
};

struct Render3DShadowDiagnostic {
    std::uint64_t casterCount{};
    std::uint64_t drawCount{};
    std::uint64_t cascadeCount{};
    std::uint64_t atlasPixels{};
    std::uint64_t occupiedAtlasPixels{};
    std::uint64_t rejectedCasterCount{};
    double atlasOccupancy{};
    std::vector<Render3DShadowRecord> records;
};

enum class Render3DLightType : std::uint8_t { Directional, Point, Spot, Area };

struct Render3DLightRecord {
    std::uint64_t id{};
    std::string name;
    Render3DLightType type{Render3DLightType::Point};
    bool visible{true};
    bool castsShadow{};
    std::uint32_t clusterReferences{};
};

struct Render3DLightDiagnostic {
    std::uint64_t visibleLightCount{};
    std::uint64_t shadowedLightCount{};
    std::uint64_t directionalCount{};
    std::uint64_t pointCount{};
    std::uint64_t spotCount{};
    std::uint64_t areaCount{};
    std::uint64_t clusterReferenceCount{};
    std::uint32_t maximumLightsPerCluster{};
    std::vector<Render3DLightRecord> lights;
};

struct Render3DOcclusionRecord {
    std::uint64_t owner{};
    std::string ownerName;
    bool visible{};
    bool hardwareQuery{};
    std::uint64_t sampleCount{};
    std::string reason;
};

struct Render3DOcclusionDiagnostic {
    std::uint64_t testedObjectCount{};
    std::uint64_t visibleObjectCount{};
    std::uint64_t culledObjectCount{};
    std::vector<Render3DOcclusionRecord> records;
};

struct Render3DProjectedTriangle {
    std::uint64_t owner{};
    std::array<float, 2> a{};
    std::array<float, 2> b{};
    std::array<float, 2> c{};
};

struct Render3DDepthComplexityImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint16_t> samples;
    std::vector<std::byte> heatmapRgba8;
    std::uint64_t coveredPixels{};
    std::uint64_t fragmentCount{};
    std::uint16_t maximumDepthComplexity{};
    double averageDepthComplexityOnCoveredPixels{};
    std::uint64_t contentHash{};
};

enum class Render3DReferenceKind : std::uint8_t {
    Mesh, Material, Texture, Shader, Pipeline, Skeleton, Animation, Lod
};

struct Render3DReferenceIssue {
    Render3DDiagnosticSeverity severity{Render3DDiagnosticSeverity::Error};
    Render3DReferenceKind kind{Render3DReferenceKind::Mesh};
    std::uint64_t owner{};
    std::string reference;
    std::string location;
    std::string message;
};

struct Render3DBudgetViolation {
    Render3DDiagnosticSeverity severity{Render3DDiagnosticSeverity::Warning};
    std::string metric;
    std::uint64_t measured{};
    std::uint64_t limit{};
    std::string message;
};

struct Render3DDiagnosticsInput {
    std::string cameraName{"Main Camera"};
    std::string renderPassName{"Main"};
    std::uint32_t viewportWidth{1920U};
    std::uint32_t viewportHeight{1080U};
    std::uint32_t referenceWidth{320U};
    std::uint32_t referenceHeight{180U};
    Render3DBudgetProfile profile{Render3DBudgetProfile::Desktop};
    std::optional<Render3DCameraBudget> budget;
    std::span<const Render3DDrawSubmission> draws;
    std::span<const Render3DTextureResidencyRecord> textures;
    std::span<const Render3DSkinningRecord> skinning;
    std::span<const Render3DLodDecision> lodDecisions;
    std::span<const Render3DShadowRecord> shadows;
    std::span<const Render3DLightRecord> lights;
    std::span<const Render3DOcclusionRecord> occlusion;
    std::span<const Render3DProjectedTriangle> referenceTriangles;
    std::span<const VoxelMaterialSelectionRequest> voxelMaterialRequests;
    std::span<const std::string> knownMeshes;
    std::span<const std::string> knownMaterials;
    std::span<const std::string> knownTextures;
    std::span<const std::string> knownShaders;
    std::span<const std::string> knownPipelines;
    std::span<const std::string> knownSkeletons;
    std::span<const std::string> knownAnimations;
};

struct Render3DDiagnosticsReport {
    std::string cameraName;
    std::string renderPassName;
    Render3DBudgetProfile profile{Render3DBudgetProfile::Desktop};
    Render3DCameraBudget budget{};
    std::uint64_t submittedObjectCount{};
    std::uint64_t visibleObjectCount{};
    std::uint64_t drawCallCount{};
    std::uint64_t meshCount{};
    std::uint64_t submeshCount{};
    std::uint64_t triangleCount{};
    std::uint64_t vertexCount{};
    std::uint64_t instanceCount{};
    std::uint64_t materialCount{};
    std::uint64_t shaderCount{};
    std::uint64_t pipelineCount{};
    std::uint64_t textureCount{};
    std::vector<Render3DPipelineBreakDiagnostic> pipelineBreaks;
    Render3DResidencyDiagnostic residency;
    Render3DSkinningDiagnostic skinning;
    std::vector<Render3DLodDecision> lodDecisions;
    Render3DShadowDiagnostic shadows;
    Render3DLightDiagnostic lighting;
    Render3DOcclusionDiagnostic occlusion;
    std::vector<Render3DReferenceIssue> referenceIssues;
    std::vector<Render3DBudgetViolation> budgetViolations;
    Render3DDepthComplexityImage depthComplexity;
    VoxelMaterialPolicyReport voxelMaterials;
    std::uint64_t contentHash{};
};

[[nodiscard]] Render3DDepthComplexityImage render_3d_depth_complexity_reference(
    const Render3DDiagnosticsInput& input);
[[nodiscard]] Render3DDiagnosticsReport build_render3d_diagnostics(
    const Render3DDiagnosticsInput& input);
[[nodiscard]] Render3DDiagnosticsReport make_render3d_diagnostics_demo_report();
[[nodiscard]] std::string render3d_diagnostics_json(const Render3DDiagnosticsReport& report);

} // namespace dve
