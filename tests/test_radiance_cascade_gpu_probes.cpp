#include "dve/render/radiance_cascade_gpu_probes.hpp"
#include "dve/rhi/null_device.hpp"
#ifdef DVE_RC_HAS_VULKAN
#include "dve/rhi/vulkan_device.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

bool same(const dve::render::RadianceCascadeSurfaceSample& a,
          const dve::render::RadianceCascadeSurfaceSample& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.viewDistance == b.viewDistance &&
           a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
           a.normal.z == b.normal.z && a.valid == b.valid;
}

void compare(std::span<const dve::render::RadianceCascadeSurfaceSample> actual,
             std::span<const dve::render::RadianceCascadeSurfaceSample> expected) {
    check(actual.size() == expected.size(), "wrong GPU probe count");
    for (std::size_t i = 0; i < actual.size(); ++i)
        check(same(actual[i], expected[i]), "GPU probe differs from CPU reference");
}
} // namespace

int main() {
    try {
        namespace sp = dve::render::spwi;
        sp::VoxelGBuffer gbuffer;
        gbuffer.width = 7;
        gbuffer.height = 5;
        gbuffer.texels.resize(35);
        for (std::uint32_t y = 0; y < 5; ++y)
            for (std::uint32_t x = 0; x < 7; ++x) {
                auto& texel = gbuffer.texels[y * 7U + x];
                texel.valid = (x + y) % 3U != 0U;
                texel.position = {float(x), float(y), float(x + y)};
                texel.normal = {0.0F, 1.0F, 0.0F};
                texel.viewDistance = float(x + 2U * y);
            }
        sp::CascadeLevelInfo level;
        level.probeSpacingPixels = 2;
        level.probesX = 4;
        level.probesY = 3;
        const auto primary = dve::render::pack_radiance_cascade_primary(gbuffer);
        const auto reference = dve::render::reference_radiance_cascade_probes(gbuffer, level);
        check(reference.size() == 12U, "incorrect reference probe count");
        for (std::uint32_t i = 0; i < 12U; ++i) {
            const auto x = std::min((i % 4U) * 2U + 1U, 6U);
            const auto y = std::min((i / 4U) * 2U + 1U, 4U);
            check(same(reference[i], primary[y * 7U + x]), "incorrect centre sampling");
        }
        check(reference[5].valid == 0U, "missing hit was not cleared");

        auto scene = sp::make_synthetic_scene(sp::SyntheticScene::ThinWall);
        const auto instance = scene.instance();
        const sp::VoxelSceneTracer tracer(std::span(&instance, 1));
        const auto sceneGbuffer = sp::build_voxel_gbuffer(tracer, scene.camera, 32U, 18U);
        auto sceneLevel = level;
        sceneLevel.probeSpacingPixels = 4U;
        sceneLevel.probesX = 8U;
        sceneLevel.probesY = 5U;
        const auto sceneReference = dve::render::reference_radiance_cascade_probes(
            sceneGbuffer, sceneLevel);
        check(sceneReference.size() == 40U, "scene probe count is incorrect");
        try {
            auto bad = level;
            bad.probesX++;
            (void)dve::render::reference_radiance_cascade_probes(gbuffer, bad);
            throw std::runtime_error("invalid probe grid was accepted");
        } catch (const std::invalid_argument&) {}

        dve::rhi::NullDevice nullDevice;
        std::string error;
        std::vector<dve::render::RadianceCascadeSurfaceSample> probes;
        const std::byte dummy[]{std::byte{0x42}};
        check(dve::render::execute_radiance_cascade_probe_pass(nullDevice, gbuffer, level,
                    dummy, probes, &error), error.c_str());
        check(nullDevice.statistics().dispatchesExecuted == 1U, "probe pass did not dispatch");
        check(nullDevice.statistics().buffersCreated == nullDevice.statistics().buffersDestroyed,
              "probe pass leaked buffers");

#ifdef DVE_RC_HAS_VULKAN
        const char* shaderPath = std::getenv("DVE_RC_PROBE_SPV");
        dve::rhi::VulkanDevice device;
        if (shaderPath && device.status() == dve::rhi::DeviceStatus::Ready) {
            std::ifstream shader(shaderPath, std::ios::binary);
            check(bool(shader), "could not open DVE_RC_PROBE_SPV");
            const std::string binary((std::istreambuf_iterator<char>(shader)), {});
            check(!binary.empty() && binary.size() % 4U == 0U, "invalid probe SPIR-V size");
            std::vector<std::byte> bytecode(binary.size());
            for (std::size_t i = 0; i < binary.size(); ++i)
                bytecode[i] = static_cast<std::byte>(static_cast<unsigned char>(binary[i]));
            check(dve::render::execute_radiance_cascade_probe_pass(device, gbuffer, level,
                        bytecode, probes, &error), error.c_str());
            compare(probes, reference);
            check(dve::render::execute_radiance_cascade_probe_pass(device, sceneGbuffer,
                        sceneLevel, bytecode, probes, &error), error.c_str());
            compare(probes, sceneReference);
            check(device.statistics().dispatchesExecuted == 2U, "Vulkan passes did not dispatch");
            std::cout << "Vulkan probe readback matches CPU reference\n";
        } else if (std::getenv("DVE_REQUIRE_RC_GPU") != nullptr) {
            throw std::runtime_error(shaderPath ?
                std::string("Vulkan unavailable: ") + std::string(device.device_loss_reason()) :
                "DVE_RC_PROBE_SPV is required for GPU validation");
        } else {
            std::cout << "Vulkan probe readback skipped: set DVE_RC_PROBE_SPV and provide an ICD\n";
        }
#else
        if (std::getenv("DVE_REQUIRE_RC_GPU") != nullptr)
            throw std::runtime_error("Vulkan backend was not built");
        std::cout << "Vulkan probe readback skipped: Vulkan backend was not built\n";
#endif
        std::cout << "dve_rc_gpu_probe_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_rc_gpu_probe_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
