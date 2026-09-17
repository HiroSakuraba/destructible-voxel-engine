#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <stdexcept>

#include "dve/cpu_hair.hpp"
#include "dve/render/cpu_hair_renderer.hpp"

namespace {

template <class Function>
double measure_ms(std::uint32_t frames, Function&& function) {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t frame = 0U; frame < frames; ++frame) function(frame);
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

int main(int argc, char** argv) {
    const std::uint32_t guides = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 1024U;
    const std::uint32_t points = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 16U;
    const std::uint32_t children = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 4U;
    const std::uint32_t frames = argc > 4 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 120U;

    std::string error;
    dve::HairAsset groom = dve::make_straight_hair_groom(guides, points, 0.0025F, 0.012F);
    dve::CpuHairInstanceDesc desc;
    desc.solver.enableSleeping = false;
    desc.windVelocity = {0.8F, 0.0F, 0.25F};
    dve::CpuHairWorld world(4U);
    const dve::CpuHairId id = world.create(groom, desc, &error);
    if (id == dve::kInvalidCpuHairId) {
        std::cerr << error << '\n';
        return 1;
    }
    for (std::uint32_t warmup = 0U; warmup < 8U; ++warmup)
        (void)world.step(1.0F / 60.0F);

    dve::render::PolygonCamera camera;
    camera.position = {1.0F, -0.1F, 2.5F};
    camera.target = {1.0F, -0.1F, 0.0F};
    camera.nearPlane = 0.01F;
    camera.farPlane = 20.0F;
    dve::render::CpuHairRibbonBuildSettings settings;
    settings.visibleStrandsPerGuide = std::clamp(children, 1U, 64U);
    settings.maximumPointsPerStrand = std::min(points, 12U);
    dve::render::CpuHairRibbonExpander serialExpander(0U);
    dve::render::CpuHairRibbonExpander expander(4U);
    if (!serialExpander.build(world.view(id), groom, camera, settings, &error) ||
        !expander.build(world.view(id), groom, camera, settings, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const dve::CpuHairView expansionSource = world.view(id);
    const double serialExpansionMs = measure_ms(frames, [&](std::uint32_t frame) {
        camera.position.x = 1.0F + 0.1F * static_cast<float>(frame & 1U);
        if (!serialExpander.build(expansionSource, groom, camera, settings, &error))
            throw std::runtime_error(error);
    });
    const double parallelExpansionMs = measure_ms(frames, [&](std::uint32_t frame) {
        camera.position.x = 1.0F + 0.1F * static_cast<float>(frame & 1U);
        if (!expander.build(expansionSource, groom, camera, settings, &error))
            throw std::runtime_error(error);
    });
    const double pipelineMs = measure_ms(frames, [&](std::uint32_t frame) {
        camera.position.x = 1.0F + 0.1F * static_cast<float>(frame & 1U);
        (void)world.step(1.0F / 60.0F);
        if (!expander.build(world.view(id), groom, camera, settings, &error))
            throw std::runtime_error(error);
    });
    const auto expansionTelemetry = expander.telemetry();
    std::cout << "{\"mode\":\"ribbon_expansion\",\"guides\":" << guides
              << ",\"points_per_guide\":" << points
              << ",\"visible_strands_per_guide\":" << settings.visibleStrandsPerGuide
              << ",\"sampled_points_per_strand\":" << settings.maximumPointsPerStrand
              << ",\"frames\":" << frames
              << ",\"workers\":" << expander.worker_count()
              << ",\"serial_ms_per_frame\":"
              << serialExpansionMs / static_cast<double>(frames)
              << ",\"parallel_ms_per_frame\":"
              << parallelExpansionMs / static_cast<double>(frames)
              << ",\"parallel_speedup\":"
              << serialExpansionMs / std::max(parallelExpansionMs, 1.0e-9)
              << ",\"solver_plus_expansion_ms_per_frame\":"
              << pipelineMs / static_cast<double>(frames)
              << ",\"visible_strands\":" << expansionTelemetry.visibleStrands
              << ",\"vertices\":" << expansionTelemetry.ribbonVertices
              << ",\"triangles\":" << expansionTelemetry.ribbonTriangles
              << ",\"worker_batches\":" << expansionTelemetry.workerBatches << "}\n";

    dve::render::CpuHairRibbonBuildSettings renderSettings = settings;
    renderSettings.maximumVisibleGuides = std::min(guides, 128U);
    renderSettings.visibleStrandsPerGuide = std::min(settings.visibleStrandsPerGuide, 4U);
    if (!expander.build(world.view(id), groom, camera, renderSettings, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    dve::render::PolygonRenderTarget target;
    target.resize(256U, 256U);
    dve::RenderEnvironment environment;
    dve::render::ReferenceCpuHairRenderer renderer;
    const std::uint32_t renderFrames = std::min(frames, 30U);
    std::uint64_t fragments = 0U;
    const double renderMs = measure_ms(renderFrames, [&](std::uint32_t) {
        target.clear({0.01F, 0.01F, 0.01F, 1.0F});
        fragments += renderer.render(expander.view(), 1U, camera, environment, target).shadedFragments;
    });
    std::cout << "{\"mode\":\"reference_raster\",\"guides\":"
              << renderSettings.maximumVisibleGuides
              << ",\"visible_strands_per_guide\":" << renderSettings.visibleStrandsPerGuide
              << ",\"width\":256,\"height\":256,\"frames\":" << renderFrames
              << ",\"ms_per_frame\":" << renderMs / static_cast<double>(renderFrames)
              << ",\"fragments_total\":" << fragments << "}\n";
    return 0;
}
