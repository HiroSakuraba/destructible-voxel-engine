#include "dve/render3d_diagnostics.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_demo_report() {
    const Render3DDiagnosticsReport report = make_render3d_diagnostics_demo_report();
    require(report.submittedObjectCount == 5U, "submitted object count is wrong");
    require(report.visibleObjectCount == 4U, "visible object count is wrong");
    require(report.drawCallCount == 4U, "draw-call count is wrong");
    require(report.pipelineBreaks.size() == 3U, "pipeline-break reconstruction is wrong");
    require(report.skinning.totalSkinnedVertices == 66'000U, "skinning attribution is wrong");
    require(report.skinning.gpuSkinnedVertices == 22'000U &&
            report.skinning.cpuSkinnedVertices == 44'000U,
            "CPU/GPU skinning split is wrong");
    require(report.occlusion.culledObjectCount == 1U, "occlusion inventory is wrong");
    require(report.depthComplexity.maximumDepthComplexity == 4U,
            "reference depth complexity did not reach four");
    require(report.referenceIssues.empty(), "retained demo contains broken references");
    require(report.budgetViolations.empty(), "retained demo exceeded desktop budgets");
    require(report.shadows.drawCount == 32U && report.lighting.visibleLightCount == 4U,
            "shadow/light accounting is wrong");
    require(report.residency.residentTextureBytes == 54'525'436U,
            "texture residency bytes are wrong");
    require(report.voxelMaterials.brickCount == 5U &&
            report.voxelMaterials.bakedBrickCount == 2U &&
            report.voxelMaterials.singleMaterialBrickCount == 1U &&
            report.voxelMaterials.deferredPalette2BrickCount == 1U &&
            report.voxelMaterials.deferredPalette4BrickCount == 1U,
            "voxel material path accounting is wrong");
    require(report.voxelMaterials.fallbackBrickCount == 1U &&
            report.voxelMaterials.recookRequestCount == 1U,
            "voxel material fallback/recook evidence is wrong");
    require(report.contentHash != 0U && report.depthComplexity.contentHash != 0U,
            "diagnostic hashes were not produced");
    const Render3DDiagnosticsReport repeated = make_render3d_diagnostics_demo_report();
    require(repeated.contentHash == report.contentHash &&
            repeated.depthComplexity.heatmapRgba8 == report.depthComplexity.heatmapRgba8,
            "retained diagnostics are not deterministic");
    const std::string json = render3d_diagnostics_json(report);
    require(json.find("\"draw_calls\": 4") != std::string::npos &&
            json.find("\"maximum_depth_complexity\": 4") != std::string::npos &&
            json.find("\"voxel_material_palette4\": 1") != std::string::npos,
            "JSON capture omitted required metrics");
}

void test_references_budgets_and_ordering() {
    std::vector<Render3DDrawSubmission> draws{
        {10U, 2U, "Broken", "missing.mesh", 0U, "missing.mat", "missing_shader", "missing_pipeline",
         "PNT", "Main", "Opaque", "ReadWrite", "BackFace", "missing.skel", "missing.anim",
         300U, 100U, 2U, true, false, true, true, true},
        {11U, 1U, "First", "ok.mesh", 0U, "ok.mat", "ok_shader", "ok_pipeline",
         "PNT", "Main", "Opaque", "ReadWrite", "BackFace", {}, {},
         30U, 10U, 1U, true, false, false, false, false},
    };
    std::vector<Render3DTextureResidencyRecord> textures{
        {"missing.dds", "linear", 64U, 64U, 7U, 0U, 0U, 0U, false},
    };
    std::vector<Render3DLodDecision> lods{
        {10U, "Broken", "missing.mesh", 3U, 2U, 0.1F, 0.0F, std::nullopt, "bad"},
    };
    std::vector<Render3DProjectedTriangle> triangles{
        {10U, {{0.0F, 0.0F}}, {{64.0F, 0.0F}}, {{0.0F, 64.0F}}},
    };
    std::vector<std::string> meshes{"ok.mesh"};
    std::vector<std::string> materials{"ok.mat"};
    std::vector<std::string> shaders{"ok_shader"};
    std::vector<std::string> pipelines{"ok_pipeline"};
    Render3DDiagnosticsInput input;
    input.viewportWidth = 64U; input.viewportHeight = 64U;
    input.referenceWidth = 16U; input.referenceHeight = 16U;
    input.draws = draws; input.textures = textures; input.lodDecisions = lods;
    input.referenceTriangles = triangles;
    input.knownMeshes = meshes; input.knownMaterials = materials;
    input.knownShaders = shaders; input.knownPipelines = pipelines;
    Render3DCameraBudget budget{};
    budget.maximumDrawCalls = 1U; budget.maximumTriangles = 10U;
    budget.maximumDepthComplexity = 1U;
    input.budget = budget;
    const Render3DDiagnosticsReport report = build_render3d_diagnostics(input);
    require(report.pipelineBreaks.size() == 1U && report.pipelineBreaks.front().previousOwner == 11U,
            "submission ordering was not deterministic");
    require(report.referenceIssues.size() >= 8U, "broken references were not fully reported");
    require(report.residency.missingMeshes == 1U && report.residency.missingTextures == 1U,
            "residency misses are wrong");
    require(report.budgetViolations.size() >= 2U, "custom budgets were not applied");
}

void test_empty_input() {
    Render3DDiagnosticsInput input;
    input.referenceWidth = 0U; input.referenceHeight = 0U;
    const Render3DDiagnosticsReport report = build_render3d_diagnostics(input);
    require(report.drawCallCount == 0U && report.depthComplexity.samples.empty(),
            "empty input did not produce an empty report");
    require(report.contentHash != 0U, "empty report did not receive a stable hash");
}
}

int main() {
    try {
        test_demo_report();
        test_references_budgets_and_ordering();
        test_empty_input();
        std::cout << "render3d diagnostics v2.23 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
