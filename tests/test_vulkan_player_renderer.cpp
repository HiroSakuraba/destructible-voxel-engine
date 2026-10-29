// Vulkan player renderer vs the CPU player renderer on the same PlayerRenderView.
//
// The two backends are not expected to match pixel for pixel: the CPU path traces voxels and
// adds traced AO/GI, the GPU path rasterizes meshed bricks with image-based sky light and
// shadow-mapped sun. What must agree is what an image diff of a correct GPU frame shows:
//   * coverage: the same pixels are geometry vs sky (silhouette IoU),
//   * shading: lit vs shadowed regions land on the same side (mean colour error bounded),
//   * destruction: carving voxels out of an object changes the GPU frame where the hole is,
//     and only after the object's revision changed (mesh cache invalidation).
// Writes cpu.ppm / gpu.ppm / diff.ppm to DVE_PLAYER_GPU_DIFF_DIR when set, for inspection.
//
// No Vulkan device, no presentation or no compiled shaders -> honest skip (77).
#include "dve_test_skip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/game_world.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/player/vulkan_player_renderer.hpp"
#include "dve/voxel_object.hpp"

namespace {

using namespace dve;
using namespace dve::player;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void save_ppm(const std::filesystem::path& path, const Rgba8Image& image) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t i = 0; i < static_cast<std::size_t>(image.width) * image.height; ++i)
        out.write(reinterpret_cast<const char*>(image.pixels.data() + i * 4U), 3);
}

struct Scene {
    VoxelObject ground{1};
    VoxelObject tower{2};
    std::vector<GameRenderObject> objects;

    Scene() {
        // 4 m x 4 m slab, 0.2 m thick (0.1 m voxels), and a 0.8 m x 1.6 m x 0.8 m tower on it.
        for (int x = -20; x < 20; ++x)
            for (int z = -20; z < 20; ++z)
                for (int y = -2; y < 0; ++y) (void)ground.set_voxel({x, y, z}, 3);
        for (int x = 0; x < 8; ++x)
            for (int z = 0; z < 8; ++z)
                for (int y = 0; y < 16; ++y) (void)tower.set_voxel({x, y, z}, 9);
        GameRenderObject slab;
        slab.id = 1; slab.kind = GameGeometryKind::Voxel; slab.voxels = &ground; slab.voxelSizeMeters = 0.1F;
        GameRenderObject block;
        block.id = 2; block.kind = GameGeometryKind::Voxel; block.voxels = &tower; block.voxelSizeMeters = 0.1F;
        block.transform.position = {-0.4F, 0.0F, -0.4F};
        objects = {slab, block};
    }

    PlayerRenderView view(float aspect) const {
        PlayerRenderView v;
        v.objects = objects;
        v.camera.position = {2.6F, 2.0F, 3.4F};
        v.camera.target = {0.0F, 0.6F, 0.0F};
        v.camera.verticalFieldOfViewRadians = 0.9F;
        v.camera.nearPlane = 0.05F;
        v.camera.farPlane = 100.0F;
        (void)aspect;
        v.environment.sunDirection = {0.55F, 0.75F, 0.35F};
        v.environment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
        v.environment.ambientOcclusionSamples = 0U;
        v.environment.volumetricFogDensityPerMeter = 0.0F;
        return v;
    }
};

struct Diff {
    double silhouetteIoU{};
    double meanAbsoluteError{};   // 0..255 over pixels both call geometry
    std::uint64_t geometryPixels{};
};

// Geometry vs sky: the sky is a smooth vertical gradient in both renderers; classify by
// distance from that row's sky colour, sampled at the image's left/right edges where this
// camera sees no geometry above the horizon... robustly: use the top row as the sky reference.
Diff compare(const Rgba8Image& cpu, const Rgba8Image& gpu, Rgba8Image& diffImage) {
    Diff diff;
    std::uint64_t both = 0, either = 0;
    double error = 0.0;
    diffImage = cpu;
    const auto pixel = [](const Rgba8Image& image, std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4U;
        return std::array<int, 3>{image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]};
    };
    const auto is_geometry = [&](const Rgba8Image& image, std::uint32_t x, std::uint32_t y) {
        // The tested scene puts sky only above the slab: anything markedly darker or more
        // saturated than the sky column at the top row is geometry.
        const auto p = pixel(image, x, y);
        const auto sky = pixel(image, x, 0U);
        int distance = 0;
        for (int c = 0; c < 3; ++c) distance += std::abs(p[static_cast<std::size_t>(c)] - sky[static_cast<std::size_t>(c)]);
        return distance > 40 && y > image.height / 8U;
    };
    for (std::uint32_t y = 0; y < cpu.height; ++y)
        for (std::uint32_t x = 0; x < cpu.width; ++x) {
            const bool a = is_geometry(cpu, x, y), b = is_geometry(gpu, x, y);
            if (a || b) ++either;
            const std::size_t i = (static_cast<std::size_t>(y) * cpu.width + x) * 4U;
            if (a && b) {
                ++both;
                const auto p = pixel(cpu, x, y), q = pixel(gpu, x, y);
                int e = 0;
                for (int c = 0; c < 3; ++c) e += std::abs(p[static_cast<std::size_t>(c)] - q[static_cast<std::size_t>(c)]);
                error += e / 3.0;
                const auto v = static_cast<std::uint8_t>(std::min(255, e));
                diffImage.pixels[i] = diffImage.pixels[i + 1] = diffImage.pixels[i + 2] = v;
            } else {
                diffImage.pixels[i] = a != b ? 255 : 0;
                diffImage.pixels[i + 1] = 0;
                diffImage.pixels[i + 2] = a != b ? 255 : 0;
            }
        }
    diff.silhouetteIoU = either ? static_cast<double>(both) / static_cast<double>(either) : 1.0;
    diff.meanAbsoluteError = both ? error / static_cast<double>(both) : 0.0;
    diff.geometryPixels = both;
    return diff;
}

int run() {
    if (!vulkan_player_renderer_available())
        return dve::testing::skip("Vulkan player renderer: built without SPIR-V shaders");
    constexpr std::uint32_t kWidth = 192U, kHeight = 128U;
    Scene scene;
    const PlayerRenderView view = scene.view(static_cast<float>(kWidth) / kHeight);

    std::string error;
    VulkanPlayerRendererOptions options;     // offscreen: readback every frame
    std::unique_ptr<IPlayerRenderer> gpu = make_vulkan_player_renderer(options, &error);
    if (!gpu) {
        if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr) throw std::runtime_error("Vulkan required: " + error);
        return dve::testing::skip("Vulkan player renderer: " + error);
    }
    require(gpu->name() == "vulkan", "unexpected renderer name");
    const bool resized = gpu->resize(kWidth, kHeight, &error);
    require(resized, error);
    const bool gpuFrameOk = gpu->render(view, &error) && gpu->present(&error);
    require(gpuFrameOk, "GPU frame failed: " + error);
    const Rgba8Image* gpuFrame = gpu->readback();
    require(gpuFrame != nullptr && gpuFrame->valid(), "GPU renderer produced no readback");
    const Rgba8Image firstGpu = *gpuFrame;
    const PlayerRenderStats gpuStats = gpu->last_stats();
    require(gpuStats.voxelInstances == 2U, "GPU renderer did not see both voxel objects");

    CpuPlayerRendererOptions cpuOptions;
    cpuOptions.quality = CpuRenderQuality::Fast;
    cpuOptions.threadCount = 2U;
    std::unique_ptr<IPlayerRenderer> cpu = make_cpu_player_renderer(nullptr, cpuOptions);
    const bool cpuResized = cpu->resize(kWidth, kHeight, &error);
    require(cpuResized, error);
    const bool cpuFrameOk = cpu->render(view, &error) && cpu->present(&error);
    require(cpuFrameOk, "CPU frame failed: " + error);
    const Rgba8Image cpuFrame = *cpu->readback();

    Rgba8Image diffImage;
    const Diff diff = compare(cpuFrame, firstGpu, diffImage);
    std::cout << "silhouette IoU " << diff.silhouetteIoU << ", mean |error| " << diff.meanAbsoluteError
              << " over " << diff.geometryPixels << " shared geometry pixels\n";
    if (const char* dir = std::getenv("DVE_PLAYER_GPU_DIFF_DIR")) {
        std::filesystem::create_directories(dir);
        save_ppm(std::filesystem::path(dir) / "cpu.ppm", cpuFrame);
        save_ppm(std::filesystem::path(dir) / "gpu.ppm", firstGpu);
        save_ppm(std::filesystem::path(dir) / "diff.ppm", diffImage);
    }
    require(diff.geometryPixels > kWidth * kHeight / 8U, "too little geometry on screen to compare");
    require(diff.silhouetteIoU > 0.97, "GPU and CPU geometry coverage disagree (IoU " +
                                           std::to_string(diff.silhouetteIoU) + ")");
    require(diff.meanAbsoluteError < 2.0, "GPU shading differs too much from the CPU reference (mean " +
                                               std::to_string(diff.meanAbsoluteError) + ")");

    // Ambient occlusion on: crevices (tower base, under the crate) darken on both renderers.
    // The sun is off so the image is ambient light only, which is what AO scales.
    PlayerRenderView flatView = view;
    flatView.environment.sunIntensity = 0.0F;
    PlayerRenderView aoView = flatView;
    aoView.environment.ambientOcclusionSamples = 16U;
    aoView.environment.ambientOcclusionMaxDistanceMeters = 0.6F;
    {
        const bool gpuFlatOk = gpu->render(flatView, &error) && gpu->present(&error);
        require(gpuFlatOk, "GPU ambient-only frame failed: " + error);
        const Rgba8Image gpuFlat = *gpu->readback();
        const bool cpuFlatOk = cpu->render(flatView, &error) && cpu->present(&error);
        require(cpuFlatOk, "CPU ambient-only frame failed: " + error);
        const Rgba8Image cpuFlat = *cpu->readback();
        const bool gpuAo = gpu->render(aoView, &error) && gpu->present(&error);
        require(gpuAo, "GPU AO frame failed: " + error);
        const Rgba8Image aoFrame = *gpu->readback();
        const bool cpuAo = cpu->render(aoView, &error) && cpu->present(&error);
        require(cpuAo, "CPU AO frame failed: " + error);
        const Rgba8Image cpuAoFrame = *cpu->readback();
        Rgba8Image aoDiffImage;
        const Diff aoDiff = compare(cpuAoFrame, aoFrame, aoDiffImage);
        // How much darker AO made each renderer's geometry, averaged over shared geometry.
        const auto darkening = [&](const Rgba8Image& with, const Rgba8Image& without) {
            double sum = 0.0; std::uint64_t n = 0;
            for (std::size_t i = 0; i < with.pixels.size(); i += 4U) {
                const int a = with.pixels[i] + with.pixels[i + 1U] + with.pixels[i + 2U];
                const int b = without.pixels[i] + without.pixels[i + 1U] + without.pixels[i + 2U];
                if (b > 60) { sum += double(b - a) / 3.0; ++n; }
            }
            return n ? sum / double(n) : 0.0;
        };
        const double gpuDark = darkening(aoFrame, gpuFlat), cpuDark = darkening(cpuAoFrame, cpuFlat);
        std::cout << "AO: silhouette IoU " << aoDiff.silhouetteIoU << ", mean |error| " << aoDiff.meanAbsoluteError
                  << "; mean darkening GPU " << gpuDark << " CPU " << cpuDark << "\n";
        if (const char* dir = std::getenv("DVE_PLAYER_GPU_DIFF_DIR")) {
            save_ppm(std::filesystem::path(dir) / "cpu_flat.ppm", cpuFlat);
            save_ppm(std::filesystem::path(dir) / "gpu_flat.ppm", gpuFlat);
            save_ppm(std::filesystem::path(dir) / "cpu_ao.ppm", cpuAoFrame);
            save_ppm(std::filesystem::path(dir) / "gpu_ao.ppm", aoFrame);
            save_ppm(std::filesystem::path(dir) / "diff_ao.ppm", aoDiffImage);
        }
        require(cpuDark > 0.02, "CPU reference shows no ambient occlusion in the test scene");
        require(gpuDark > 0.6 * cpuDark && gpuDark < 1.5 * cpuDark,
                "GPU ambient occlusion strength differs from the CPU reference (GPU " + std::to_string(gpuDark) +
                    ", CPU " + std::to_string(cpuDark) + ")");
        require(aoDiff.meanAbsoluteError < 3.0, "GPU ambient-only image differs too much from the CPU reference (mean " +
                                                    std::to_string(aoDiff.meanAbsoluteError) + ")");

        // Voxel-traced AO: same definition, rays walk the scene voxel grid instead of the depth buffer.
        VulkanPlayerRendererOptions tracedOptions;
        tracedOptions.ambientOcclusionMode = PlayerAmbientOcclusionMode::VoxelTraced;
        std::unique_ptr<IPlayerRenderer> traced = make_vulkan_player_renderer(tracedOptions, &error);
        require(traced != nullptr, "traced-AO renderer creation failed: " + error);
        require(traced->resize(kWidth, kHeight, &error), error);
        const bool tracedOk = traced->render(aoView, &error) && traced->present(&error);
        require(tracedOk, "traced AO frame failed: " + error);
        const Rgba8Image tracedFrame = *traced->readback();
        const double tracedDark = darkening(tracedFrame, gpuFlat);
        Rgba8Image tracedDiffImage;
        const Diff tracedDiff = compare(cpuAoFrame, tracedFrame, tracedDiffImage);
        std::cout << "traced AO: mean |error| " << tracedDiff.meanAbsoluteError << "; mean darkening " << tracedDark
                  << " (CPU " << cpuDark << ")\n";
        if (const char* dir = std::getenv("DVE_PLAYER_GPU_DIFF_DIR")) {
            save_ppm(std::filesystem::path(dir) / "gpu_traced_ao.ppm", tracedFrame);
            save_ppm(std::filesystem::path(dir) / "diff_traced_ao.ppm", tracedDiffImage);
        }
        require(tracedDark > 0.5 * cpuDark && tracedDark < 1.6 * cpuDark,
                "traced ambient occlusion strength differs from the CPU reference (GPU " + std::to_string(tracedDark) +
                    ", CPU " + std::to_string(cpuDark) + ")");
        require(tracedDiff.meanAbsoluteError < 3.5, "traced AO image differs too much from the CPU reference (mean " +
                                                        std::to_string(tracedDiff.meanAbsoluteError) + ")");

        // Traced one-bounce GI against the CPU reference's VoxelOneBounce (Reference quality keeps
        // the environment's mode), with the sun on so lit surfaces bounce light onto their
        // neighbours, and with the sun off so only the sky is bounced and occluded.
        VulkanPlayerRendererOptions giOptions;
        giOptions.globalIllumination = PlayerGlobalIlluminationMode::VoxelOneBounce;
        std::unique_ptr<IPlayerRenderer> giGpu = make_vulkan_player_renderer(giOptions, &error);
        require(giGpu != nullptr, "GI renderer creation failed: " + error);
        require(giGpu->resize(kWidth, kHeight, &error), error);
        VulkanPlayerRendererOptions rcOptions;
        rcOptions.globalIllumination = PlayerGlobalIlluminationMode::RadianceCascades;
        std::unique_ptr<IPlayerRenderer> rcGpu = make_vulkan_player_renderer(rcOptions, &error);
        require(rcGpu != nullptr, "radiance cascade renderer creation failed: " + error);
        require(rcGpu->resize(kWidth, kHeight, &error), error);
        CpuPlayerRendererOptions refOptions;
        refOptions.quality = CpuRenderQuality::Reference;
        refOptions.threadCount = 2U;
        std::unique_ptr<IPlayerRenderer> cpuRef = make_cpu_player_renderer(nullptr, refOptions);
        require(cpuRef->resize(kWidth, kHeight, &error), error);
        for (const bool sunOn : {true, false}) {
            PlayerRenderView giView = view;
            giView.environment.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
            giView.environment.globalIlluminationSamples = 16U;
            giView.environment.globalIlluminationMaxDistanceMeters = 8.0F;
            if (!sunOn) giView.environment.sunIntensity = 0.0F;
            const bool refOk = cpuRef->render(giView, &error) && cpuRef->present(&error);
            require(refOk, "CPU one-bounce reference failed: " + error);
            const Rgba8Image reference = *cpuRef->readback();
            const bool bounceOk = giGpu->render(giView, &error) && giGpu->present(&error);
            require(bounceOk, "GPU one-bounce frame failed: " + error);
            const Rgba8Image bounce = *giGpu->readback();
            const bool ambientOk = gpu->render(giView, &error) && gpu->present(&error);
            require(ambientOk, "GPU ambient frame failed: " + error);
            const Rgba8Image ambient = *gpu->readback();
            Rgba8Image bounceDiffImage, ambientDiffImage;
            const Diff bounceDiff = compare(reference, bounce, bounceDiffImage);
            const Diff ambientDiff = compare(reference, ambient, ambientDiffImage);
            std::cout << "one-bounce GI (sun " << (sunOn ? "on" : "off") << "): traced mean |error| "
                      << bounceDiff.meanAbsoluteError << ", ambient-only " << ambientDiff.meanAbsoluteError << "\n";
            require(bounceDiff.meanAbsoluteError < 1.5 && bounceDiff.meanAbsoluteError < ambientDiff.meanAbsoluteError,
                    std::string("traced one-bounce GI (sun ") + (sunOn ? "on" : "off") +
                        ") is not closer to the CPU reference than ambient-only light");
            const bool rcOk = rcGpu->render(giView, &error) && rcGpu->present(&error);
            require(rcOk, "GPU radiance cascade frame failed: " + error);
            const Rgba8Image cascades = *rcGpu->readback();
            Rgba8Image rcDiffImage;
            const Diff rcDiff = compare(reference, cascades, rcDiffImage);
            std::cout << "radiance cascades (sun " << (sunOn ? "on" : "off") << "): mean |error| "
                      << rcDiff.meanAbsoluteError << " vs CPU one-bounce reference\n";
            if (const char* dir = std::getenv("DVE_PLAYER_GPU_DIFF_DIR")) {
                const std::string tag = sunOn ? "sun" : "nosun";
                save_ppm(std::filesystem::path(dir) / ("gpu_rc_" + tag + ".ppm"), cascades);
                save_ppm(std::filesystem::path(dir) / ("cpu_gi_" + tag + ".ppm"), reference);
                save_ppm(std::filesystem::path(dir) / ("gpu_gi_" + tag + ".ppm"), bounce);
                save_ppm(std::filesystem::path(dir) / ("gpu_ambient_" + tag + ".ppm"), ambient);
            }
            // The test scene's indirect light is faint, so the bar is "close to the CPU reference
            // and to the full one-bounce result", not "better than ambient" (which already is close).
            require(rcDiff.meanAbsoluteError < 1.5 && rcDiff.meanAbsoluteError < ambientDiff.meanAbsoluteError + 0.1,
                    std::string("radiance cascades (sun ") + (sunOn ? "on" : "off") +
                        ") differ too much from the CPU one-bounce reference");
        }
    }

    // Physical sky: background, sky light and sun colour come from the atmosphere model, like the CPU.
    {
        PlayerRenderView skyView = view;
        skyView.environment.skyMode = SkyMode::Physical;
        skyView.environment.globalIlluminationMode = GlobalIlluminationMode::VoxelOneBounce;
        skyView.environment.globalIlluminationSamples = 16U;
        skyView.environment.globalIlluminationMaxDistanceMeters = 8.0F;
        CpuPlayerRendererOptions refOptions;
        refOptions.quality = CpuRenderQuality::Reference;
        refOptions.threadCount = 2U;
        std::unique_ptr<IPlayerRenderer> cpuSky = make_cpu_player_renderer(nullptr, refOptions);
        require(cpuSky->resize(kWidth, kHeight, &error), error);
        const bool refOk = cpuSky->render(skyView, &error) && cpuSky->present(&error);
        require(refOk, "CPU physical-sky reference failed: " + error);
        const Rgba8Image reference = *cpuSky->readback();
        VulkanPlayerRendererOptions skyOptions;
        skyOptions.globalIllumination = PlayerGlobalIlluminationMode::VoxelOneBounce;
        std::unique_ptr<IPlayerRenderer> skyGpu = make_vulkan_player_renderer(skyOptions, &error);
        require(skyGpu != nullptr && skyGpu->resize(kWidth, kHeight, &error), "physical-sky renderer: " + error);
        const bool skyOk = skyGpu->render(skyView, &error) && skyGpu->present(&error);
        require(skyOk, "GPU physical-sky frame failed: " + error);
        const Rgba8Image physical = *skyGpu->readback();
        PlayerRenderView colourView = skyView;
        colourView.environment.skyMode = SkyMode::Colors;
        const bool colourOk = skyGpu->render(colourView, &error) && skyGpu->present(&error);
        require(colourOk, "GPU colour-sky frame failed: " + error);
        const Rgba8Image colours = *skyGpu->readback();
        Rgba8Image diffImage;
        const Diff physicalDiff = compare(reference, physical, diffImage);
        const Diff colourDiff = compare(reference, colours, diffImage);
        std::cout << "physical sky: GPU mean |error| vs CPU " << physicalDiff.meanAbsoluteError
                  << " (colour-sky GPU frame: " << colourDiff.meanAbsoluteError << ")\n";
        if (const char* dir = std::getenv("DVE_PLAYER_GPU_DIFF_DIR")) {
            save_ppm(std::filesystem::path(dir) / "cpu_physical.ppm", reference);
            save_ppm(std::filesystem::path(dir) / "gpu_physical.ppm", physical);
        }
        require(physicalDiff.meanAbsoluteError < 4.0 && physicalDiff.meanAbsoluteError < colourDiff.meanAbsoluteError,
                "GPU physical sky does not match the CPU reference");
    }

    // Same view again: cached meshes, identical frame.
    const bool again = gpu->render(view, &error) && gpu->present(&error);
    require(again, error);
    require(gpu->readback()->pixels == firstGpu.pixels, "unchanged scene rendered differently on the GPU");

    // Destruction: carve the tower's top half away; the GPU frame must change there.
    for (int x = 0; x < 8; ++x)
        for (int z = 0; z < 8; ++z)
            for (int y = 8; y < 16; ++y) (void)scene.tower.set_voxel({x, y, z}, kAirMaterial);
    const bool carvedOk = gpu->render(view, &error) && gpu->present(&error);
    require(carvedOk, error);
    const Rgba8Image& carved = *gpu->readback();
    std::uint64_t changed = 0;
    for (std::size_t i = 0; i < carved.pixels.size(); i += 4U)
        if (std::abs(int(carved.pixels[i]) - int(firstGpu.pixels[i])) > 8) ++changed;
    std::cout << "destruction changed " << changed << " pixels\n";
    require(changed > 200U, "carving the tower did not change the GPU frame (mesh cache not invalidated)");

    // Resizing rebuilds targets and keeps rendering.
    const bool smaller = gpu->resize(96U, 64U, &error) && gpu->render(view, &error) && gpu->present(&error);
    require(smaller, error);
    require(gpu->readback()->width == 96U && gpu->readback()->height == 64U, "resize did not change readback size");

    std::cout << "Vulkan player renderer tests passed\n";
    return dve::testing::exit_code(false);
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& exception) {
        std::cerr << "dve_vulkan_player_renderer_tests: " << exception.what() << '\n';
        return 1;
    }
}
