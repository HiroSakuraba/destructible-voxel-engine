#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/cpu_hair.hpp"
#include "dve/render/cpu_hair_renderer.hpp"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool nearly_equal(dve::Float3 a, dve::Float3 b, float tolerance = 1.0e-6F) {
    return std::abs(a.x - b.x) <= tolerance &&
           std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.z - b.z) <= tolerance;
}

} // namespace

int main() {
    try {
        std::string error;
        dve::HairAsset groom = dve::make_straight_hair_groom(4U, 5U, 0.025F, 0.04F);
        for (std::uint32_t guide = 0U; guide < groom.guides.size(); ++guide)
            groom.guides[guide].layerId = 10U + guide;
        groom.recompute_hash();

        dve::CpuHairInstanceDesc simulation;
        simulation.gravity = {};
        simulation.solver.enableSleeping = false;
        dve::CpuHairWorld world(0U);
        const dve::CpuHairId id = world.create(groom, simulation, &error);
        require(id != dve::kInvalidCpuHairId, error);
        const dve::CpuHairView source = world.view(id);

        dve::render::PolygonCamera camera;
        camera.position = {0.05F, -0.08F, 1.0F};
        camera.target = {0.05F, -0.08F, 0.0F};
        camera.nearPlane = 0.01F;
        camera.farPlane = 10.0F;

        dve::render::CpuHairRibbonBuildSettings settings;
        settings.visibleStrandsPerGuide = 4U;
        settings.rootWidthMeters = 0.01F;
        settings.tipWidthMeters = 0.002F;
        settings.rootSpreadMeters = 0.001F;
        settings.tipSpreadMeters = 0.008F;
        settings.longitudinalJitterMeters = 0.0F;

        dve::render::CpuHairRibbonExpander expander;
        require(expander.worker_count() <= 4U,
                "visible hair expander created an excessive default worker pool");
        require(expander.build(source, groom, camera, settings, &error), error);
        const dve::render::CpuHairRibbonPacketView packet = expander.view();
        require(packet.visible, "visible source generated an invisible packet");
        require(packet.strands.size() == 16U, "child strand expansion count is wrong");
        require(packet.vertices.size() == 160U, "ribbon vertex count is wrong");
        require(packet.indices.size() == 384U, "ribbon index count is wrong");
        require(packet.triangleLayerIds.size() == 128U,
                "ribbon triangle layer metadata is incomplete");
        require(expander.telemetry().selectedGuides == 4U &&
                expander.telemetry().visibleStrands == 16U &&
                expander.telemetry().ribbonTriangles == 128U,
                "ribbon build telemetry is wrong");
        require(packet.strands.front().layerId == 10U &&
                packet.strands.back().layerId == 13U,
                "guide layer ids were not propagated");

        const float rootWidth = dve::length(dve::subtract(packet.vertices[0U].position,
                                                          packet.vertices[1U].position));
        const float tipWidth = dve::length(dve::subtract(packet.vertices[8U].position,
                                                         packet.vertices[9U].position));
        require(std::abs(rootWidth - settings.rootWidthMeters) < 1.0e-5F,
                "root ribbon width is wrong");
        require(std::abs(tipWidth - settings.tipWidthMeters) < 1.0e-5F,
                "tip ribbon width is wrong");
        require(nearly_equal(packet.vertices[0U].position,
                             dve::subtract(source.positions[0U],
                                 dve::multiply(dve::normalize(dve::subtract(
                                     packet.vertices[1U].position,
                                     packet.vertices[0U].position)),
                                     settings.rootWidthMeters * 0.5F)), 2.0e-5F),
                "guide centerline ribbon did not remain on its simulated guide");

        const std::vector<dve::render::CpuHairRibbonVertex> firstVertices(
            packet.vertices.begin(), packet.vertices.end());
        const std::uint64_t firstHash = packet.topologyHash;
        require(expander.build(source, groom, camera, settings, &error), error);
        const dve::render::CpuHairRibbonPacketView rebuilt = expander.view();
        require(rebuilt.topologyHash == firstHash && rebuilt.vertices.size() == firstVertices.size(),
                "identical ribbon builds changed topology");
        for (std::size_t vertex = 0U; vertex < rebuilt.vertices.size(); ++vertex) {
            require(nearly_equal(rebuilt.vertices[vertex].position,
                                 firstVertices[vertex].position),
                    "identical ribbon builds were not deterministic");
        }

        settings.maximumVisibleGuides = 2U;
        settings.maximumVisibleStrands = 5U;
        settings.maximumPointsPerStrand = 3U;
        require(expander.build(source, groom, camera, settings, &error), error);
        const dve::render::CpuHairRibbonPacketView lodPacket = expander.view();
        require(lodPacket.strands.size() == 5U && lodPacket.vertices.size() == 30U &&
                lodPacket.indices.size() == 60U,
                "visible hair guide/strand/point LOD is wrong");
        require(expander.telemetry().guideBudgetDrops == 2U &&
                expander.telemetry().strandBudgetDrops == 3U,
                "visible hair LOD drops were not reported");
        require(lodPacket.strands.front().sourceGuide == 0U &&
                lodPacket.strands.back().sourceGuide == 3U,
                "visible guide LOD was not distributed across the groom");

        const float projectedDiameter = dve::render::estimate_cpu_hair_projected_diameter_pixels(
            source, camera, 1080U);
        require(projectedDiameter > 100.0F && std::isfinite(projectedDiameter),
                "visible hair projected-size estimate is invalid");

        const auto culledLod = dve::render::select_cpu_hair_ribbon_lod(8.0F, settings);
        const auto distantLod = dve::render::select_cpu_hair_ribbon_lod(20.0F, settings);
        const auto farLod = dve::render::select_cpu_hair_ribbon_lod(80.0F, settings);
        const auto mediumLod = dve::render::select_cpu_hair_ribbon_lod(180.0F, settings);
        const auto nearLod = dve::render::select_cpu_hair_ribbon_lod(400.0F, settings);
        require(!culledLod.visible && culledLod.tier == dve::render::CpuHairRibbonLodTier::Culled,
                "visible hair LOD did not cull a subpixel groom");
        require(distantLod.visible && distantLod.settings.visibleStrandsPerGuide == 1U &&
                distantLod.settings.maximumPointsPerStrand == 3U,
                "distant visible hair LOD did not retain the authored tighter point limit");
        require(farLod.tier == dve::render::CpuHairRibbonLodTier::Far &&
                farLod.settings.visibleStrandsPerGuide == 2U,
                "far visible hair LOD selected the wrong strand density");
        require(mediumLod.tier == dve::render::CpuHairRibbonLodTier::Medium &&
                mediumLod.settings.visibleStrandsPerGuide == 4U,
                "medium visible hair LOD selected the wrong strand density");
        require(nearLod.tier == dve::render::CpuHairRibbonLodTier::Near &&
                nearLod.settings.maximumVisibleGuides == settings.maximumVisibleGuides,
                "near visible hair LOD changed the authored settings");

        dve::CpuHairRuntime runtime(0U);
        require(runtime.bind(991U, groom, {}, {}, &error), error);
        require(expander.build(runtime, 991U, camera, settings, &error), error);
        require(expander.view().strands.size() == 5U,
                "runtime-owner visible hair bridge did not use the bound groom");
        require(!expander.build(runtime, 992U, camera, settings, &error),
                "runtime-owner visible hair bridge accepted an unbound owner");

        dve::render::CpuHairRibbonBuildSettings invalid = settings;
        invalid.visibleStrandsPerGuide = 0U;
        require(!expander.build(source, groom, camera, invalid, &error),
                "visible hair accepted zero strands per guide");
        invalid = settings;
        invalid.rootColor.x = -0.1F;
        require(!expander.build(source, groom, camera, invalid, &error),
                "visible hair accepted a negative material color");

        settings.maximumVisibleGuides = 0U;
        settings.maximumVisibleStrands = 0U;
        settings.maximumPointsPerStrand = 0U;
        require(expander.build(source, groom, camera, settings, &error), error);
        dve::render::PolygonRenderTarget target;
        target.resize(192U, 192U);
        target.clear({0.01F, 0.01F, 0.01F, 1.0F});
        dve::RenderEnvironment environment;
        environment.sunDirection = {0.3F, 0.8F, 0.4F};
        dve::render::ReferenceCpuHairRenderer renderer;
        const auto renderStats = renderer.render(expander.view(), 77U, camera, environment, target);
        require(renderStats.rasterizedTriangles > 0U && renderStats.shadedFragments > 0U,
                "reference hair renderer produced no fragments");
        bool foundObject = false;
        for (std::size_t pixel = 0U; pixel < target.objectId.size(); ++pixel) {
            if (target.objectId[pixel] == 77U) {
                foundObject = true;
                require(target.materialIndex[pixel] >= 10U && target.materialIndex[pixel] <= 13U,
                        "reference hair renderer lost the guide layer id");
                break;
            }
        }
        require(foundObject, "reference hair renderer did not publish object identification");

        dve::render::PolygonRenderTarget occluded;
        occluded.resize(64U, 64U);
        occluded.clear({0.2F, 0.2F, 0.2F, 1.0F}, 0.0F);
        const auto occludedStats = renderer.render(expander.view(), 88U, camera, environment, occluded);
        require(occludedStats.shadedFragments == 0U &&
                occludedStats.depthRejectedFragments > 0U,
                "reference hair renderer ignored existing opaque depth");

        std::cout << "DVE CPU visible-hair rendering tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE CPU visible-hair rendering tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
