// The GPU atmosphere tables (atmosphere_gpu.hpp):
//   - the checked-in SPIR-V matches the HLSL it was built from (shaders/compiled/atmosphere);
//   - on the Null RHI: three dispatches, nothing leaked;
//   - on a real Vulkan device (any ICD; Mesa's lavapipe is enough): the transmittance, multiple-
//     scattering and sky-view tables match the CPU's for a clear noon, a low sun and a hazy day.
// Set DVE_REQUIRE_ATMOSPHERE_GPU=1 to fail, rather than skip, when no Vulkan device is available.
#include "dve/network_crypto.hpp"
#include "dve/render/atmosphere.hpp"
#include "dve/render/atmosphere_gpu.hpp"
#include "dve/rhi/null_device.hpp"
#include "dve/transform.hpp"
#ifdef DVE_ATMOSPHERE_HAS_VULKAN
#include "dve/rhi/vulkan_device.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    std::ifstream list(root / "shaders/compiled/atmosphere/SOURCES.sha256");
    std::string line;
    int checked = 0;
    while (std::getline(list, line)) {
        if (line.size() < 67) continue;
        const std::string expected = line.substr(0, 64), path = line.substr(66);
        const auto bytes = read_bytes(root / path);
        const auto digest = network_sha256({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        check(hex(digest) == expected, path + " changed since shaders/compiled/atmosphere was built: rebuild the SPIR-V (README)");
        ++checked;
    }
    check(checked == 4, "SOURCES.sha256 lists the three shaders and their include");
}

// Worst relative difference between two tables of rgb, ignoring texels darker than `floor`
// relative to the table's brightest (they are compared absolutely instead).
double worst_difference(const std::vector<Float4>& gpu, const std::vector<Float3>& cpu, double floor) {
    double peak = 0.0;
    for (const Float3& c : cpu) peak = std::max({peak, static_cast<double>(c.x), static_cast<double>(c.y), static_cast<double>(c.z)});
    double worst = 0.0;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const double a[3] = {gpu[i].x, gpu[i].y, gpu[i].z}, b[3] = {cpu[i].x, cpu[i].y, cpu[i].z};
        for (int k = 0; k < 3; ++k) {
            const double scale = std::max(std::abs(b[k]), floor * peak);
            worst = std::max(worst, std::abs(a[k] - b[k]) / std::max(scale, 1e-30));
        }
    }
    return worst;
}

RenderEnvironment sky(float elevationDegrees, float mie) {
    RenderEnvironment e;
    e.skyMode = SkyMode::Physical;
    const float el = elevationDegrees * 3.14159265F / 180.0F;
    e.sunDirection = {std::cos(el) * 0.6F, std::sin(el), std::cos(el) * 0.8F};
    e.atmosphereMieScale = mie;
    return e;
}

} // namespace

int main() {
    const std::filesystem::path root = DVE_SOURCE_DIR;
    test_spirv_is_current(root);
    const auto transmittance = read_bytes(root / "shaders/compiled/atmosphere/atmosphere_transmittance.spv");
    const auto multiple = read_bytes(root / "shaders/compiled/atmosphere/atmosphere_multiple_scattering.spv");
    const auto skyView = read_bytes(root / "shaders/compiled/atmosphere/atmosphere_sky_view.spv");
    const GpuAtmosphereShaders shaders{transmittance, multiple, skyView};

    {
        rhi::NullDevice device;
        GpuAtmosphereTables tables;
        std::string error;
        check(execute_gpu_atmosphere(device, sky(30.0F, 1.0F), shaders, tables, &error), "Null RHI: " + error);
        const auto stats = device.statistics();
        check(stats.dispatchesExecuted == 3U, "three dispatches: transmittance, multiple scattering, sky view");
        check(stats.buffersCreated == stats.buffersDestroyed, "no buffers leaked");
    }

#ifdef DVE_ATMOSPHERE_HAS_VULKAN
    rhi::VulkanDevice device;
    if (device.status() == rhi::DeviceStatus::Ready) {
        std::printf("Vulkan device: %s\n", device.capabilities().adapterName.c_str());
        for (const auto& [name, environment] : {std::pair{"sun 30 deg", sky(30.0F, 1.0F)}, std::pair{"sun 3 deg", sky(3.0F, 1.0F)},
                                                std::pair{"hazy, sun 45 deg", sky(45.0F, 8.0F)}}) {
            GpuAtmosphereTables gpu;
            std::string error;
            if (!execute_gpu_atmosphere(device, environment, shaders, gpu, &error)) {
                check(false, std::string(name) + ": " + error);
                continue;
            }
            const auto model = atmosphere_model(environment);
            const double t = worst_difference(gpu.transmittance, model->tables().transmittance_table(), 1e-4);
            const double m = worst_difference(gpu.multipleScattering, model->tables().multiple_scattering_table(), 1e-3);
            const double s = worst_difference(gpu.skyView, model->sky_view_table(), 1e-3);
            std::printf("%s: GPU vs CPU tables, worst relative difference: transmittance %.2e, multiple scattering %.2e, sky view %.2e\n",
                        name, t, m, s);
            check(t < 4e-3, std::string(name) + ": GPU transmittance = CPU (float kilometres vs double metres)");
            check(m < 1e-2, std::string(name) + ": GPU multiple scattering = CPU");
            check(s < 2e-2, std::string(name) + ": GPU sky view = CPU");
        }
    } else if (std::getenv("DVE_REQUIRE_ATMOSPHERE_GPU") != nullptr) {
        check(false, std::string("Vulkan unavailable: ") + std::string(device.device_loss_reason()));
    } else {
        std::printf("Vulkan atmosphere check skipped: no Vulkan device\n");
    }
#else
    if (std::getenv("DVE_REQUIRE_ATMOSPHERE_GPU") != nullptr) check(false, "built without the Vulkan backend");
    else std::printf("Vulkan atmosphere check skipped: built without the Vulkan backend\n");
#endif

    if (g_failures == 0) std::printf("dve_atmosphere_gpu_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
