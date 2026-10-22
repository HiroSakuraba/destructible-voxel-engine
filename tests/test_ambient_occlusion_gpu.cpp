// The GPU ambient-occlusion passes (ambient_occlusion_gpu.hpp):
//   - the checked-in SPIR-V matches the HLSL it was built from (shaders/compiled/ao);
//   - on the Null RHI: four dispatches, nothing leaked;
//   - on a real Vulkan device (any ICD; Mesa's lavapipe is enough): the GPU's primary hits and
//     per-pixel AO match the CPU mirror of the same four passes, and the GPU's AO averaged over
//     frames matches the exact open-sky share at the bottom of a slot.
// Set DVE_REQUIRE_AO_GPU=1 to fail, rather than skip, when no Vulkan device is available.
#include "dve/network_crypto.hpp"
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/ambient_occlusion_gpu.hpp"
#include "dve/rhi/null_device.hpp"
#ifdef DVE_AO_HAS_VULKAN
#include "dve/rhi/vulkan_device.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    return bytes;
}

std::string hex(const NetworkSha256Digest& digest) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : digest) { out += digits[b >> 4U]; out += digits[b & 15U]; }
    return out;
}

void test_spirv_is_current(const std::filesystem::path& root) {
    std::ifstream list(root / "shaders/compiled/ao/SOURCES.sha256");
    std::string line;
    int checked = 0;
    while (std::getline(list, line)) {
        if (line.size() < 67) continue;
        const std::string expected = line.substr(0, 64), path = line.substr(66);
        const auto bytes = read_bytes(root / path);
        const auto digest = network_sha256({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        check(hex(digest) == expected, path + " changed since shaders/compiled/ao was built: rebuild the SPIR-V (README)");
        ++checked;
    }
    check(checked == 6, "SOURCES.sha256 lists the three shaders and their includes");
}

// Floor, a wall, a slot 10 wide and 10 deep, and a few blocks: open ground, corners, crevices.
void build_scene(VoxelObject& object) {
    for (int x = -40; x < 40; ++x)
        for (int z = -40; z < 40; ++z) object.set_voxel({x, 0, z}, 1);
    for (int z = -40; z < 40; ++z)
        for (int y = 1; y <= 10; ++y) {
            for (int x = -40; x < -5; ++x) object.set_voxel({x, y, z}, 2);
            for (int x = 5; x < 12; ++x) object.set_voxel({x, y, z}, 2);
        }
    for (int x = 20; x < 26; ++x)
        for (int z = -10; z < -4; ++z)
            for (int y = 1; y <= 3; ++y) object.set_voxel({x, y, z}, 1);
}

} // namespace

int main() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    test_spirv_is_current(root);
    const auto trace = read_bytes(root / "shaders/compiled/ao/primary_brickmap_trace.spv");
    const auto generate = read_bytes(root / "shaders/compiled/ao/generate_ao_rays.spv");
    const auto resolve = read_bytes(root / "shaders/compiled/ao/resolve_ao.spv");
    const GpuAmbientOcclusionShaders shaders{trace, generate, resolve};

    VoxelObject object(1U);
    build_scene(object);
    PackedBrickmapScene scene;
    scene.rebuild(object);
    RenderEnvironment environment;
    environment.ambientOcclusionSamples = 16;
    environment.ambientOcclusionMaxDistanceMeters = 1.0F;   // 10 voxels
    const float metersPerVoxel = 0.1F;
    const auto rays = gpu_primary_rays({8.0F, 40.0F, 30.0F}, {8.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 0.9F, 96U, 64U, 500.0F);
    const GpuAmbientOcclusionResult reference = reference_gpu_ambient_occlusion(scene, rays, environment, metersPerVoxel, 0U);
    std::size_t hits = 0;
    double meanAo = 0.0;
    for (std::size_t p = 0; p < rays.size(); ++p) {
        hits += reference.primary[p].hit;
        meanAo += reference.ambientOcclusion[p] / static_cast<double>(rays.size());
    }
    std::printf("CPU mirror: %zu of %zu pixels hit, mean AO %.4f\n", hits, rays.size(), meanAo);
    check(hits > rays.size() / 2 && meanAo < 0.99 && meanAo > 0.5, "the scene has open ground and occluded corners");

    {
        rhi::NullDevice device;
        GpuAmbientOcclusionResult result;
        std::string error;
        check(execute_gpu_ambient_occlusion(device, scene, rays, environment, metersPerVoxel, 0U, shaders, result, &error),
              "Null RHI: " + error);
        const auto stats = device.statistics();
        check(stats.dispatchesExecuted == 4U, "four dispatches: trace, generate, trace, resolve");
        check(stats.buffersCreated == stats.buffersDestroyed, "no buffers leaked");
    }

#ifdef DVE_AO_HAS_VULKAN
    rhi::VulkanDevice device;
    if (device.status() == rhi::DeviceStatus::Ready) {
        std::printf("Vulkan device: %s\n", device.capabilities().adapterName.c_str());
        GpuAmbientOcclusionResult gpu;
        std::string error;
        const bool ran = execute_gpu_ambient_occlusion(device, scene, rays, environment, metersPerVoxel, 0U, shaders, gpu, &error);
        check(ran, "Vulkan: " + error);
        if (ran) {
            std::size_t sameHits = 0, close = 0;
            double worst = 0.0, meanDifference = 0.0;
            for (std::size_t p = 0; p < rays.size(); ++p) {
                const GpuTraceResult& a = gpu.primary[p];
                const GpuTraceResult& b = reference.primary[p];
                if (a.hit == b.hit && (a.hit == 0U || (a.voxel.x == b.voxel.x && a.voxel.y == b.voxel.y && a.voxel.z == b.voxel.z &&
                                                       a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
                                                       a.normal.z == b.normal.z && std::abs(a.distance - b.distance) < 1e-3F)))
                    ++sameHits;
                const double d = std::abs(static_cast<double>(gpu.ambientOcclusion[p]) - reference.ambientOcclusion[p]);
                worst = std::max(worst, d);
                meanDifference += d / static_cast<double>(rays.size());
                close += d < 1e-3 ? 1U : 0U;
            }
            std::printf("Vulkan vs CPU mirror: %zu/%zu primary hits identical; AO within 0.001 at %zu/%zu pixels, "
                        "mean difference %.6f, worst %.4f\n",
                        sameHits, rays.size(), close, rays.size(), meanDifference, worst);
            check(sameHits == rays.size(), "GPU primary hits = CPU mirror");
            check(close >= rays.size() * 995 / 1000 && meanDifference < 1e-3, "GPU AO = CPU mirror (a ray grazing an edge may differ)");

            // Averaged over 32 frames (new rotations each frame), the GPU's AO at the bottom of
            // the slot (x in -5..5) converges on the exact open-sky share with a long max distance.
            RenderEnvironment open = environment;
            open.ambientOcclusionMaxDistanceMeters = 1.0e3F;
            open.ambientOcclusionSamples = 64;
            const auto down = gpu_primary_rays({0.0F, 30.0F, 0.0F}, {0.0F, 1.0F, 0.01F}, {0.0F, 0.0F, -1.0F}, 0.05F, 4U, 64U, 500.0F);
            double sum = 0.0;
            std::size_t count = 0;
            for (std::uint32_t frame = 0; frame < 32U; ++frame) {
                GpuAmbientOcclusionResult slot;
                if (!execute_gpu_ambient_occlusion(device, scene, down, open, metersPerVoxel, frame * 977U, shaders, slot, &error)) {
                    check(false, "Vulkan slot frame: " + error);
                    break;
                }
                for (std::size_t p = 0; p < down.size(); ++p)
                    if (slot.primary[p].hit && slot.primary[p].voxel.y == 0) { sum += slot.ambientOcclusion[p]; ++count; }
            }
            const double depth = 10.0 - ambient_occlusion_origin_bias(open, metersPerVoxel);   // rays leave from the bias
            const double exact = 5.0 / std::sqrt(25.0 + depth * depth);
            std::printf("GPU AO at the bottom of a 10 x 10 slot, %zu samples x 64 rays: %.4f (exact %.4f)\n", count, sum / count, exact);
            check(count > 0 && std::abs(sum / count - exact) < 5e-3, "GPU AO converges on the exact open-sky share");
        }
    } else if (std::getenv("DVE_REQUIRE_AO_GPU") != nullptr) {
        check(false, std::string("Vulkan unavailable: ") + std::string(device.device_loss_reason()));
    } else {
        std::printf("Vulkan AO check skipped: no Vulkan device\n");
    }
#else
    if (std::getenv("DVE_REQUIRE_AO_GPU") != nullptr) check(false, "built without the Vulkan backend");
    else std::printf("Vulkan AO check skipped: built without the Vulkan backend\n");
#endif

    if (g_failures == 0) std::printf("dve_ambient_occlusion_gpu_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
