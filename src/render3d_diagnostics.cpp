#include "dve/render3d_diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>

namespace dve {
namespace {

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}

template<class Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t shift = 0U; shift < sizeof(Unsigned) * 8U; shift += 8U)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> shift) & static_cast<Unsigned>(0xFFU)));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, value.size());
    for (const char character : value) hash_byte(hash, static_cast<std::uint8_t>(character));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    hash_integer(hash, bits);
}

[[nodiscard]] bool contains(std::span<const std::string> values, std::string_view wanted) {
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) { return value == wanted; });
}

[[nodiscard]] std::string join_break_reasons(Render3DPipelineBreakReason reasons) {
    std::ostringstream stream;
    bool first = true;
    const auto append = [&](Render3DPipelineBreakReason reason, std::string_view label) {
        if (!has_render3d_break_reason(reasons, reason)) return;
        if (!first) stream << ", ";
        stream << label;
        first = false;
    };
    append(Render3DPipelineBreakReason::Mesh, "mesh");
    append(Render3DPipelineBreakReason::Material, "material");
    append(Render3DPipelineBreakReason::Shader, "shader");
    append(Render3DPipelineBreakReason::Pipeline, "pipeline");
    append(Render3DPipelineBreakReason::VertexLayout, "vertex layout");
    append(Render3DPipelineBreakReason::RenderPass, "pass");
    append(Render3DPipelineBreakReason::BlendState, "blend");
    append(Render3DPipelineBreakReason::DepthState, "depth");
    append(Render3DPipelineBreakReason::RasterState, "raster");
    append(Render3DPipelineBreakReason::SkinningMode, "skinning");
    return stream.str();
}

[[nodiscard]] Render3DPipelineBreakReason classify_break(
    const Render3DDrawSubmission& previous, const Render3DDrawSubmission& current) noexcept {
    Render3DPipelineBreakReason reasons = Render3DPipelineBreakReason::NoBreak;
    if (previous.meshAsset != current.meshAsset) reasons = reasons | Render3DPipelineBreakReason::Mesh;
    if (previous.materialAsset != current.materialAsset) reasons = reasons | Render3DPipelineBreakReason::Material;
    if (previous.shader != current.shader) reasons = reasons | Render3DPipelineBreakReason::Shader;
    if (previous.pipeline != current.pipeline) reasons = reasons | Render3DPipelineBreakReason::Pipeline;
    if (previous.vertexLayout != current.vertexLayout) reasons = reasons | Render3DPipelineBreakReason::VertexLayout;
    if (previous.renderPass != current.renderPass) reasons = reasons | Render3DPipelineBreakReason::RenderPass;
    if (previous.blendState != current.blendState) reasons = reasons | Render3DPipelineBreakReason::BlendState;
    if (previous.depthState != current.depthState) reasons = reasons | Render3DPipelineBreakReason::DepthState;
    if (previous.rasterState != current.rasterState) reasons = reasons | Render3DPipelineBreakReason::RasterState;
    if (previous.skinned != current.skinned || previous.gpuSkinned != current.gpuSkinned)
        reasons = reasons | Render3DPipelineBreakReason::SkinningMode;
    return reasons;
}

[[nodiscard]] float edge(const std::array<float, 2>& a, const std::array<float, 2>& b,
                         float x, float y) noexcept {
    return (x - a[0]) * (b[1] - a[1]) - (y - a[1]) * (b[0] - a[0]);
}

void raster_triangle(Render3DDepthComplexityImage& image, const Render3DProjectedTriangle& triangle,
                     std::uint32_t viewportWidth, std::uint32_t viewportHeight) noexcept {
    if (image.width == 0U || image.height == 0U || viewportWidth == 0U || viewportHeight == 0U) return;
    const float scaleX = static_cast<float>(image.width) / static_cast<float>(viewportWidth);
    const float scaleY = static_cast<float>(image.height) / static_cast<float>(viewportHeight);
    const std::array<float, 2> a{triangle.a[0] * scaleX, triangle.a[1] * scaleY};
    const std::array<float, 2> b{triangle.b[0] * scaleX, triangle.b[1] * scaleY};
    const std::array<float, 2> c{triangle.c[0] * scaleX, triangle.c[1] * scaleY};
    const float area = edge(a, b, c[0], c[1]);
    if (std::abs(area) <= std::numeric_limits<float>::epsilon()) return;
    const int minX = std::max(0, static_cast<int>(std::floor(std::min({a[0], b[0], c[0]}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min({a[1], b[1], c[1]}))));
    const int maxX = std::min(static_cast<int>(image.width) - 1,
                              static_cast<int>(std::ceil(std::max({a[0], b[0], c[0]}))));
    const int maxY = std::min(static_cast<int>(image.height) - 1,
                              static_cast<int>(std::ceil(std::max({a[1], b[1], c[1]}))));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float ab = edge(a, b, px, py);
            const float bc = edge(b, c, px, py);
            const float ca = edge(c, a, px, py);
            const bool inside = area > 0.0F ? (ab >= 0.0F && bc >= 0.0F && ca >= 0.0F)
                                           : (ab <= 0.0F && bc <= 0.0F && ca <= 0.0F);
            if (!inside) continue;
            const std::size_t index = static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x);
            if (image.samples[index] != std::numeric_limits<std::uint16_t>::max()) ++image.samples[index];
        }
    }
}

[[nodiscard]] std::array<std::byte, 4> heat_color(std::uint16_t value, std::uint16_t maximum) noexcept {
    if (value == 0U || maximum == 0U) return {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}};
    const double t = std::clamp(static_cast<double>(value) / static_cast<double>(maximum), 0.0, 1.0);
    const auto component = [](double channelValue) {
        return std::byte{static_cast<unsigned char>(std::clamp(channelValue, 0.0, 255.0))};
    };
    return {component(255.0 * t), component(255.0 * std::min(1.0, t * 1.8)),
            component(255.0 * (1.0 - t)), std::byte{255}};
}

void add_reference_issue(std::vector<Render3DReferenceIssue>& output, Render3DReferenceKind kind,
                         std::uint64_t owner, std::string reference, std::string location,
                         std::string message) {
    output.push_back({Render3DDiagnosticSeverity::Error, kind, owner, std::move(reference),
                      std::move(location), std::move(message)});
}

void add_budget(std::vector<Render3DBudgetViolation>& output, std::string metric,
                std::uint64_t measured, std::uint64_t limit) {
    if (limit == 0U || measured <= limit) return;
    output.push_back({Render3DDiagnosticSeverity::Warning, metric, measured, limit,
                      metric + " exceeded: " + std::to_string(measured) + " > " + std::to_string(limit)});
}

[[nodiscard]] std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 8U);
    for (const char c : input) {
        switch (c) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output.push_back(c); break;
        }
    }
    return output;
}

} // namespace

Render3DCameraBudget render3d_budget_for_profile(Render3DBudgetProfile profile,
                                                  std::uint32_t viewportWidth,
                                                  std::uint32_t viewportHeight) noexcept {
    if (profile == Render3DBudgetProfile::Unrestricted) return {};
    const std::uint64_t pixels = static_cast<std::uint64_t>(viewportWidth) * viewportHeight;
    const std::uint64_t scale = std::max<std::uint64_t>(1U, pixels / (1920ULL * 1080ULL));
    switch (profile) {
    case Render3DBudgetProfile::Mobile:
        return {120U, 1'000'000U * scale, 1'500'000U * scale, 5'000U, 128U, 128U, 256U,
                512ULL * 1024ULL * 1024ULL, 250'000U, 80U, 128U, 4'096U, 4U};
    case Render3DBudgetProfile::Desktop:
        return {2'000U, 8'000'000U * scale, 12'000'000U * scale, 100'000U, 1'024U, 2'048U, 4'096U,
                4ULL * 1024ULL * 1024ULL * 1024ULL, 2'000'000U, 2'000U, 2'048U, 65'536U, 12U};
    case Render3DBudgetProfile::HighEnd:
        return {8'000U, 30'000'000U * scale, 50'000'000U * scale, 1'000'000U, 4'096U, 8'192U, 16'384U,
                12ULL * 1024ULL * 1024ULL * 1024ULL, 8'000'000U, 8'000U, 8'192U, 262'144U, 24U};
    case Render3DBudgetProfile::Unrestricted: break;
    }
    return {};
}

Render3DDepthComplexityImage render_3d_depth_complexity_reference(const Render3DDiagnosticsInput& input) {
    Render3DDepthComplexityImage result;
    result.width = input.referenceWidth;
    result.height = input.referenceHeight;
    if (result.width == 0U || result.height == 0U) return result;
    result.samples.assign(static_cast<std::size_t>(result.width) * result.height, 0U);
    for (const Render3DProjectedTriangle& triangle : input.referenceTriangles)
        raster_triangle(result, triangle, input.viewportWidth, input.viewportHeight);
    for (const std::uint16_t value : result.samples) {
        if (value != 0U) ++result.coveredPixels;
        result.fragmentCount += value;
        result.maximumDepthComplexity = std::max(result.maximumDepthComplexity, value);
    }
    if (result.coveredPixels != 0U)
        result.averageDepthComplexityOnCoveredPixels = static_cast<double>(result.fragmentCount) /
            static_cast<double>(result.coveredPixels);
    result.heatmapRgba8.reserve(result.samples.size() * 4U);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint16_t value : result.samples) {
        hash_integer(hash, value);
        const auto color = heat_color(value, result.maximumDepthComplexity);
        result.heatmapRgba8.insert(result.heatmapRgba8.end(), color.begin(), color.end());
    }
    result.contentHash = hash;
    return result;
}

Render3DDiagnosticsReport build_render3d_diagnostics(const Render3DDiagnosticsInput& input) {
    Render3DDiagnosticsReport result;
    result.cameraName = input.cameraName;
    result.renderPassName = input.renderPassName;
    result.profile = input.profile;
    result.budget = input.budget.value_or(render3d_budget_for_profile(
        input.profile, input.viewportWidth, input.viewportHeight));
    result.voxelMaterials = build_voxel_material_policy_report(input.voxelMaterialRequests);

    std::vector<const Render3DDrawSubmission*> submitted;
    submitted.reserve(input.draws.size());
    std::set<std::uint64_t> submittedOwners;
    std::set<std::uint64_t> visibleOwners;
    std::set<std::string, std::less<>> meshes;
    std::set<std::string, std::less<>> materials;
    std::set<std::string, std::less<>> shaders;
    std::set<std::string, std::less<>> pipelines;
    std::map<std::string, std::uint64_t, std::less<>> meshRefs;
    std::map<std::string, std::uint64_t, std::less<>> materialRefs;
    std::map<std::string, std::uint64_t, std::less<>> shaderRefs;
    std::map<std::string, std::uint64_t, std::less<>> pipelineRefs;

    for (const Render3DDrawSubmission& draw : input.draws) {
        submittedOwners.insert(draw.owner);
        if (!draw.visible || draw.occlusionCulled) continue;
        visibleOwners.insert(draw.owner);
        submitted.push_back(&draw);
        ++result.drawCallCount;
        ++result.submeshCount;
        result.vertexCount += draw.vertexCount * draw.instanceCount;
        result.triangleCount += draw.triangleCount * draw.instanceCount;
        result.instanceCount += draw.instanceCount;
        meshes.insert(draw.meshAsset);
        materials.insert(draw.materialAsset);
        shaders.insert(draw.shader);
        pipelines.insert(draw.pipeline);
        ++meshRefs[draw.meshAsset]; ++materialRefs[draw.materialAsset];
        ++shaderRefs[draw.shader]; ++pipelineRefs[draw.pipeline];
    }
    result.submittedObjectCount = submittedOwners.size();
    result.visibleObjectCount = visibleOwners.size();
    result.meshCount = meshes.size();
    result.materialCount = materials.size();
    result.shaderCount = shaders.size();
    result.pipelineCount = pipelines.size();

    std::stable_sort(submitted.begin(), submitted.end(), [](const auto* left, const auto* right) {
        return std::tie(left->submissionOrder, left->owner, left->submeshIndex) <
               std::tie(right->submissionOrder, right->owner, right->submeshIndex);
    });
    for (std::size_t index = 1U; index < submitted.size(); ++index) {
        const Render3DPipelineBreakReason reasons = classify_break(*submitted[index - 1U], *submitted[index]);
        if (reasons == Render3DPipelineBreakReason::NoBreak) continue;
        result.pipelineBreaks.push_back({index, submitted[index - 1U]->owner, submitted[index]->owner,
                                         reasons, join_break_reasons(reasons)});
    }

    const auto addCatalog = [](const auto& refs, std::span<const std::string> known,
                               std::vector<Render3DResidencyEntry>& output,
                               std::uint64_t& missing) {
        for (const auto& [asset, references] : refs) {
            const bool resident = contains(known, asset);
            output.push_back({asset, {}, resident, 0U, references});
            if (!resident) ++missing;
        }
    };
    addCatalog(meshRefs, input.knownMeshes, result.residency.meshes, result.residency.missingMeshes);
    addCatalog(materialRefs, input.knownMaterials, result.residency.materials, result.residency.missingMaterials);
    addCatalog(shaderRefs, input.knownShaders, result.residency.shaders, result.residency.missingShaders);
    addCatalog(pipelineRefs, input.knownPipelines, result.residency.pipelines, result.residency.missingPipelines);

    std::map<std::string, std::uint64_t, std::less<>> textureRefs;
    for (const Render3DTextureResidencyRecord& texture : input.textures) {
        ++textureRefs[texture.asset];
        result.residency.residentTextureBytes += texture.resident ? texture.residentBytes : 0U;
        std::ostringstream detail;
        detail << texture.width << 'x' << texture.height << " mips " << texture.firstResidentMip << '+'
               << texture.residentMipCount << '/' << texture.totalMipCount;
        result.residency.textures.push_back({texture.asset, detail.str(), texture.resident,
                                             texture.residentBytes, 1U});
        if (!texture.resident || !contains(input.knownTextures, texture.asset))
            ++result.residency.missingTextures;
    }
    std::sort(result.residency.textures.begin(), result.residency.textures.end(),
              [](const auto& left, const auto& right) { return left.asset < right.asset; });
    result.textureCount = result.residency.textures.size();

    for (const Render3DSkinningRecord& record : input.skinning) {
        result.skinning.records.push_back(record);
        result.skinning.totalSkinnedVertices += record.skinnedVertices;
        result.skinning.maximumBonesPerObject = std::max<std::uint64_t>(
            result.skinning.maximumBonesPerObject, record.boneCount);
        if (record.gpu) {
            result.skinning.gpuSkinnedVertices += record.skinnedVertices;
            ++result.skinning.gpuObjects;
            result.skinning.gpuDispatches += record.dispatchCount;
        } else {
            result.skinning.cpuSkinnedVertices += record.skinnedVertices;
            ++result.skinning.cpuObjects;
        }
    }
    std::sort(result.skinning.records.begin(), result.skinning.records.end(),
              [](const auto& left, const auto& right) { return left.owner < right.owner; });

    result.lodDecisions.assign(input.lodDecisions.begin(), input.lodDecisions.end());
    std::sort(result.lodDecisions.begin(), result.lodDecisions.end(),
              [](const auto& left, const auto& right) { return left.owner < right.owner; });

    result.shadows.records.assign(input.shadows.begin(), input.shadows.end());
    for (const Render3DShadowRecord& record : result.shadows.records) {
        if (record.accepted) {
            ++result.shadows.casterCount;
            result.shadows.drawCount += record.drawCount;
            result.shadows.cascadeCount += record.cascadeCount;
            result.shadows.atlasPixels += record.atlasPixels;
            result.shadows.occupiedAtlasPixels += record.occupiedAtlasPixels;
        } else ++result.shadows.rejectedCasterCount;
    }
    if (result.shadows.atlasPixels != 0U)
        result.shadows.atlasOccupancy = static_cast<double>(result.shadows.occupiedAtlasPixels) /
            static_cast<double>(result.shadows.atlasPixels);

    for (const Render3DLightRecord& light : input.lights) {
        if (!light.visible) continue;
        result.lighting.lights.push_back(light);
        ++result.lighting.visibleLightCount;
        if (light.castsShadow) ++result.lighting.shadowedLightCount;
        result.lighting.clusterReferenceCount += light.clusterReferences;
        result.lighting.maximumLightsPerCluster = std::max(
            result.lighting.maximumLightsPerCluster, light.clusterReferences);
        switch (light.type) {
        case Render3DLightType::Directional: ++result.lighting.directionalCount; break;
        case Render3DLightType::Point: ++result.lighting.pointCount; break;
        case Render3DLightType::Spot: ++result.lighting.spotCount; break;
        case Render3DLightType::Area: ++result.lighting.areaCount; break;
        }
    }
    std::sort(result.lighting.lights.begin(), result.lighting.lights.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });

    result.occlusion.records.assign(input.occlusion.begin(), input.occlusion.end());
    result.occlusion.testedObjectCount = result.occlusion.records.size();
    for (const Render3DOcclusionRecord& record : result.occlusion.records) {
        if (record.visible) ++result.occlusion.visibleObjectCount;
        else ++result.occlusion.culledObjectCount;
    }
    std::sort(result.occlusion.records.begin(), result.occlusion.records.end(),
              [](const auto& left, const auto& right) { return left.owner < right.owner; });

    for (const Render3DDrawSubmission& draw : input.draws) {
        const std::string location = "draw owner " + std::to_string(draw.owner) + " submesh " +
            std::to_string(draw.submeshIndex);
        if (draw.meshAsset.empty() || !contains(input.knownMeshes, draw.meshAsset))
            add_reference_issue(result.referenceIssues, Render3DReferenceKind::Mesh, draw.owner,
                                draw.meshAsset, location, "missing mesh reference");
        if (draw.materialAsset.empty() || !contains(input.knownMaterials, draw.materialAsset))
            add_reference_issue(result.referenceIssues, Render3DReferenceKind::Material, draw.owner,
                                draw.materialAsset, location, "missing material reference");
        if (draw.shader.empty() || !contains(input.knownShaders, draw.shader))
            add_reference_issue(result.referenceIssues, Render3DReferenceKind::Shader, draw.owner,
                                draw.shader, location, "missing shader reference");
        if (draw.pipeline.empty() || !contains(input.knownPipelines, draw.pipeline))
            add_reference_issue(result.referenceIssues, Render3DReferenceKind::Pipeline, draw.owner,
                                draw.pipeline, location, "missing pipeline reference");
        if (draw.skinned) {
            if (draw.skeletonAsset.empty() || !contains(input.knownSkeletons, draw.skeletonAsset))
                add_reference_issue(result.referenceIssues, Render3DReferenceKind::Skeleton, draw.owner,
                                    draw.skeletonAsset, location, "missing skeleton reference");
            if (draw.animationAsset.empty() || !contains(input.knownAnimations, draw.animationAsset))
                add_reference_issue(result.referenceIssues, Render3DReferenceKind::Animation, draw.owner,
                                    draw.animationAsset, location, "missing animation reference");
        }
    }
    for (const Render3DTextureResidencyRecord& texture : input.textures) {
        if (!texture.resident || !contains(input.knownTextures, texture.asset))
            add_reference_issue(result.referenceIssues, Render3DReferenceKind::Texture, 0U,
                                texture.asset, "texture residency", "missing or nonresident texture");
    }
    for (const Render3DLodDecision& decision : input.lodDecisions) {
        if (decision.availableLodCount == 0U || decision.selectedLod >= decision.availableLodCount)
            add_reference_issue(result.referenceIssues, Render3DReferenceKind::Lod, decision.owner,
                                std::to_string(decision.selectedLod), "LOD selection",
                                "selected LOD is outside the available range");
    }
    std::sort(result.referenceIssues.begin(), result.referenceIssues.end(), [](const auto& left, const auto& right) {
        return std::tie(left.owner, left.kind, left.reference) < std::tie(right.owner, right.kind, right.reference);
    });

    result.depthComplexity = render_3d_depth_complexity_reference(input);
    add_budget(result.budgetViolations, "draw calls", result.drawCallCount, result.budget.maximumDrawCalls);
    add_budget(result.budgetViolations, "triangles", result.triangleCount, result.budget.maximumTriangles);
    add_budget(result.budgetViolations, "vertices", result.vertexCount, result.budget.maximumVertices);
    add_budget(result.budgetViolations, "instances", result.instanceCount, result.budget.maximumInstances);
    add_budget(result.budgetViolations, "materials", result.materialCount, result.budget.maximumMaterials);
    add_budget(result.budgetViolations, "pipelines", result.pipelineCount, result.budget.maximumPipelines);
    add_budget(result.budgetViolations, "textures", result.textureCount, result.budget.maximumTextures);
    add_budget(result.budgetViolations, "resident texture bytes", result.residency.residentTextureBytes,
               result.budget.maximumResidentTextureBytes);
    add_budget(result.budgetViolations, "skinned vertices", result.skinning.totalSkinnedVertices,
               result.budget.maximumSkinnedVertices);
    add_budget(result.budgetViolations, "shadow draws", result.shadows.drawCount,
               result.budget.maximumShadowDraws);
    add_budget(result.budgetViolations, "visible lights", result.lighting.visibleLightCount,
               result.budget.maximumVisibleLights);
    add_budget(result.budgetViolations, "cluster light references", result.lighting.clusterReferenceCount,
               result.budget.maximumClusterLightReferences);
    add_budget(result.budgetViolations, "depth complexity", result.depthComplexity.maximumDepthComplexity,
               result.budget.maximumDepthComplexity);

    std::uint64_t hash = 1469598103934665603ULL;
    hash_string(hash, result.cameraName); hash_string(hash, result.renderPassName);
    for (const std::uint64_t value : {result.submittedObjectCount, result.visibleObjectCount, result.drawCallCount,
         result.meshCount, result.submeshCount, result.triangleCount, result.vertexCount, result.instanceCount,
         result.materialCount, result.shaderCount, result.pipelineCount, result.textureCount,
         result.skinning.totalSkinnedVertices, result.shadows.drawCount, result.lighting.visibleLightCount,
         result.occlusion.culledObjectCount, result.depthComplexity.contentHash,
         result.voxelMaterials.contentHash}) hash_integer(hash, value);
    for (const Render3DPipelineBreakDiagnostic& item : result.pipelineBreaks) {
        hash_integer(hash, item.previousOwner); hash_integer(hash, item.owner);
        hash_integer(hash, static_cast<std::uint16_t>(item.reasons)); hash_string(hash, item.summary);
    }
    for (const Render3DLodDecision& item : result.lodDecisions) {
        hash_integer(hash, item.owner); hash_integer(hash, item.selectedLod);
        hash_float(hash, item.projectedScreenFraction); hash_float(hash, item.transitionWeight);
        hash_string(hash, item.reason);
    }
    for (const Render3DReferenceIssue& item : result.referenceIssues) {
        hash_integer(hash, item.owner); hash_integer(hash, static_cast<std::uint8_t>(item.kind));
        hash_string(hash, item.reference); hash_string(hash, item.location);
    }
    result.contentHash = hash;
    return result;
}

Render3DDiagnosticsReport make_render3d_diagnostics_demo_report() {
    const std::vector<Render3DDrawSubmission> draws{
        {1U, 1U, "Pilot", "meshes/pilot.mesh", 0U, "materials/pilot.mat", "pbr_skinned", "pbr_skinned_opaque", "PNTW", "Main", "Opaque", "ReadWrite", "BackFace", "skeletons/pilot.skel", "animations/pilot_idle.anim", 22'000U, 14'000U, 1U, true, false, true, true, true},
        {2U, 2U, "Crate", "meshes/crate.mesh", 0U, "materials/crate.mat", "pbr_static", "pbr_static_opaque", "PNT", "Main", "Opaque", "ReadWrite", "BackFace", {}, {}, 2'400U, 800U, 12U, true, false, true, false, false},
        {3U, 3U, "Glass Drone", "meshes/drone.mesh", 0U, "materials/glass.mat", "pbr_transparent", "pbr_transparent", "PNT", "Main", "Alpha", "ReadOnly", "BackFace", {}, {}, 8'000U, 4'000U, 2U, true, false, false, false, false},
        {4U, 4U, "Animated Guard", "meshes/guard.mesh", 0U, "materials/guard.mat", "pbr_skinned", "pbr_skinned_opaque", "PNTW", "Main", "Opaque", "ReadWrite", "BackFace", "skeletons/guard.skel", "animations/guard_walk.anim", 44'000U, 28'000U, 1U, true, false, true, true, false},
        {5U, 5U, "Occluded Prop", "meshes/prop.mesh", 0U, "materials/crate.mat", "pbr_static", "pbr_static_opaque", "PNT", "Main", "Opaque", "ReadWrite", "BackFace", {}, {}, 1'000U, 400U, 1U, true, true, true, false, false},
    };
    const std::vector<Render3DTextureResidencyRecord> textures{
        {"textures/pilot_albedo.dds", "linear_wrap", 2048U, 2048U, 12U, 2U, 10U, 5'592'064U, true},
        {"textures/pilot_normal.dds", "linear_wrap", 2048U, 2048U, 12U, 3U, 9U, 2'796'032U, true},
        {"textures/world_atlas.dds", "linear_wrap", 4096U, 4096U, 13U, 1U, 12U, 44'739'240U, true},
        {"textures/glass_mask.dds", "linear_clamp", 1024U, 1024U, 11U, 0U, 11U, 1'398'100U, true},
    };
    const std::vector<Render3DSkinningRecord> skinning{
        {1U, "Pilot", "skeletons/pilot.skel", "animations/pilot_idle.anim", 22'000U, 68U, 1U, true},
        {4U, "Animated Guard", "skeletons/guard.skel", "animations/guard_walk.anim", 44'000U, 92U, 0U, false},
    };
    const std::vector<Render3DLodDecision> lods{
        {1U, "Pilot", "meshes/pilot.mesh", 0U, 3U, 0.42F, 0.0F, std::nullopt, "screen fraction selected LOD0"},
        {2U, "Crate", "meshes/crate.mesh", 1U, 3U, 0.08F, 0.25F, std::nullopt, "cross-fading LOD0 to LOD1"},
        {3U, "Glass Drone", "meshes/drone.mesh", 0U, 2U, 0.19F, 0.0F, 0U, "forced by diagnostic override"},
        {4U, "Animated Guard", "meshes/guard.mesh", 1U, 4U, 0.11F, 0.0F, std::nullopt, "screen fraction selected LOD1"},
    };
    const std::vector<Render3DShadowRecord> shadows{
        {1U, "Pilot", 4U, 4U, 4'194'304U, 524'288U, true, {}},
        {2U, "Crate", 2U, 24U, 4'194'304U, 786'432U, true, {}},
        {4U, "Animated Guard", 4U, 4U, 4'194'304U, 524'288U, true, {}},
        {5U, "Occluded Prop", 0U, 0U, 0U, 0U, false, "occlusion culled before shadow submission"},
    };
    const std::vector<Render3DLightRecord> lights{
        {1U, "Sun", Render3DLightType::Directional, true, true, 16U},
        {2U, "Hall Lamp", Render3DLightType::Point, true, true, 20U},
        {3U, "Drone Spot", Render3DLightType::Spot, true, false, 12U},
        {4U, "Panel", Render3DLightType::Area, true, false, 8U},
    };
    const std::vector<Render3DOcclusionRecord> occlusion{
        {1U, "Pilot", true, false, 5'400U, "CPU/reference bounds visible"},
        {2U, "Crate", true, false, 7'800U, "CPU/reference bounds visible"},
        {3U, "Glass Drone", true, false, 2'100U, "CPU/reference bounds visible"},
        {4U, "Animated Guard", true, false, 4'900U, "CPU/reference bounds visible"},
        {5U, "Occluded Prop", false, false, 0U, "CPU/reference bounds hidden behind wall"},
    };
    const std::vector<Render3DProjectedTriangle> triangles{
        {1U, {{300.0F, 240.0F}}, {{1'100.0F, 240.0F}}, {{300.0F, 800.0F}}},
        {1U, {{1'100.0F, 240.0F}}, {{1'100.0F, 800.0F}}, {{300.0F, 800.0F}}},
        {2U, {{500.0F, 320.0F}}, {{1'300.0F, 320.0F}}, {{500.0F, 880.0F}}},
        {2U, {{1'300.0F, 320.0F}}, {{1'300.0F, 880.0F}}, {{500.0F, 880.0F}}},
        {3U, {{700.0F, 400.0F}}, {{1'500.0F, 400.0F}}, {{700.0F, 960.0F}}},
        {3U, {{1'500.0F, 400.0F}}, {{1'500.0F, 960.0F}}, {{700.0F, 960.0F}}},
        {4U, {{900.0F, 480.0F}}, {{1'700.0F, 480.0F}}, {{900.0F, 1'040.0F}}},
        {4U, {{1'700.0F, 480.0F}}, {{1'700.0F, 1'040.0F}}, {{900.0F, 1'040.0F}}},
    };
    const std::vector<std::string> meshes{"meshes/pilot.mesh", "meshes/crate.mesh", "meshes/drone.mesh", "meshes/guard.mesh", "meshes/prop.mesh"};
    const std::vector<std::string> materials{"materials/pilot.mat", "materials/crate.mat", "materials/glass.mat", "materials/guard.mat"};
    const std::vector<std::string> textureAssets{"textures/pilot_albedo.dds", "textures/pilot_normal.dds", "textures/world_atlas.dds", "textures/glass_mask.dds"};
    const std::vector<std::string> shaders{"pbr_skinned", "pbr_static", "pbr_transparent"};
    const std::vector<std::string> pipelines{"pbr_skinned_opaque", "pbr_static_opaque", "pbr_transparent"};
    const std::vector<std::string> skeletons{"skeletons/pilot.skel", "skeletons/guard.skel"};
    const std::vector<std::string> animations{"animations/pilot_idle.anim", "animations/guard_walk.anim"};
    VoxelMaterialPolicyConfig voxelPolicy;
    voxelPolicy.mode = VoxelMaterialMode::Hybrid;
    voxelPolicy.platform = VoxelMaterialPlatformProfile::HighEndDesktop;
    voxelPolicy.deferredMaximumDistance = 35.0F;
    std::vector<VoxelMaterialSelectionRequest> voxelMaterials;
    const auto addVoxelMaterial = [&](std::uint64_t id, std::string name, float distance,
                                      std::uint32_t lod, std::uint8_t materialCount,
                                      bool destructible, bool recentlyModified,
                                      VoxelMaterialCookedRepresentations cooked = VoxelMaterialCookedRepresentations{true, true, true, true}) {
        VoxelMaterialSelectionRequest request;
        request.project = voxelPolicy;
        request.brick.brickId = id;
        request.brick.assetName = std::move(name);
        request.brick.cameraDistance = distance;
        request.brick.lod = lod;
        request.brick.sourceMaterialCount = materialCount;
        request.brick.destructible = destructible;
        request.brick.recentlyModified = recentlyModified;
        request.brick.cooked = cooked;
        voxelMaterials.push_back(std::move(request));
    };
    addVoxelMaterial(101U, "FreshConcreteFracture", 4.0F, 0U, 2U, true, true);
    addVoxelMaterial(102U, "GoldRockHero", 12.0F, 0U, 4U, true, false);
    addVoxelMaterial(103U, "DistantMountain", 140.0F, 3U, 3U, false, false);
    addVoxelMaterial(104U, "MetalCrate", 9.0F, 0U, 1U, true, false);
    addVoxelMaterial(105U, "MissingPaletteCook", 6.0F, 0U, 2U, true, true,
                     VoxelMaterialCookedRepresentations{true, true, false, false});
    Render3DDiagnosticsInput input;
    input.cameraName = "Main Camera"; input.renderPassName = "Main";
    input.viewportWidth = 1920U; input.viewportHeight = 1080U;
    input.referenceWidth = 320U; input.referenceHeight = 180U;
    input.draws = draws; input.textures = textures; input.skinning = skinning;
    input.lodDecisions = lods; input.shadows = shadows; input.lights = lights;
    input.occlusion = occlusion; input.referenceTriangles = triangles;
    input.voxelMaterialRequests = voxelMaterials;
    input.knownMeshes = meshes; input.knownMaterials = materials; input.knownTextures = textureAssets;
    input.knownShaders = shaders; input.knownPipelines = pipelines;
    input.knownSkeletons = skeletons; input.knownAnimations = animations;
    return build_render3d_diagnostics(input);
}

std::string render3d_diagnostics_json(const Render3DDiagnosticsReport& report) {
    std::ostringstream stream;
    stream << "{\n"
           << "  \"camera\": \"" << json_escape(report.cameraName) << "\",\n"
           << "  \"pass\": \"" << json_escape(report.renderPassName) << "\",\n"
           << "  \"submitted_objects\": " << report.submittedObjectCount << ",\n"
           << "  \"visible_objects\": " << report.visibleObjectCount << ",\n"
           << "  \"draw_calls\": " << report.drawCallCount << ",\n"
           << "  \"pipeline_breaks\": " << report.pipelineBreaks.size() << ",\n"
           << "  \"meshes\": " << report.meshCount << ",\n"
           << "  \"triangles\": " << report.triangleCount << ",\n"
           << "  \"vertices\": " << report.vertexCount << ",\n"
           << "  \"instances\": " << report.instanceCount << ",\n"
           << "  \"materials\": " << report.materialCount << ",\n"
           << "  \"pipelines\": " << report.pipelineCount << ",\n"
           << "  \"textures\": " << report.textureCount << ",\n"
           << "  \"resident_texture_bytes\": " << report.residency.residentTextureBytes << ",\n"
           << "  \"skinned_vertices\": " << report.skinning.totalSkinnedVertices << ",\n"
           << "  \"shadow_draws\": " << report.shadows.drawCount << ",\n"
           << "  \"visible_lights\": " << report.lighting.visibleLightCount << ",\n"
           << "  \"occlusion_culled\": " << report.occlusion.culledObjectCount << ",\n"
           << "  \"maximum_depth_complexity\": " << report.depthComplexity.maximumDepthComplexity << ",\n"
           << "  \"voxel_material_bricks\": " << report.voxelMaterials.brickCount << ",\n"
           << "  \"voxel_material_baked\": " << report.voxelMaterials.bakedBrickCount << ",\n"
           << "  \"voxel_material_single\": " << report.voxelMaterials.singleMaterialBrickCount << ",\n"
           << "  \"voxel_material_palette2\": " << report.voxelMaterials.deferredPalette2BrickCount << ",\n"
           << "  \"voxel_material_palette4\": " << report.voxelMaterials.deferredPalette4BrickCount << ",\n"
           << "  \"voxel_material_fallbacks\": " << report.voxelMaterials.fallbackBrickCount << ",\n"
           << "  \"voxel_material_recooks\": " << report.voxelMaterials.recookRequestCount << ",\n"
           << "  \"reference_issues\": " << report.referenceIssues.size() << ",\n"
           << "  \"budget_violations\": " << report.budgetViolations.size() << ",\n"
           << "  \"content_hash\": " << report.contentHash << "\n"
           << "}\n";
    return stream.str();
}

} // namespace dve
