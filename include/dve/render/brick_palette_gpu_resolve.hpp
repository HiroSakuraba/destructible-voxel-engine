#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "dve/render/brick_palette_rhi.hpp"

namespace dve::render {

// The first six bindings are byte-identical to the v2.26 renderer submission contract. The
// conformance kernel extends that layout with analytic material records, requests, and outputs so
// physical devices can execute the packed palette decoder before texture residency is connected.
inline constexpr std::uint32_t kBrickPaletteResolveMaterialBinding = 6U;
inline constexpr std::uint32_t kBrickPaletteResolveRequestBinding = 7U;
inline constexpr std::uint32_t kBrickPaletteResolveOutputBinding = 8U;
inline constexpr std::uint32_t kBrickPaletteResolveBindingCount = 9U;
inline constexpr std::uint32_t kBrickPaletteResolveThreads = 64U;

struct BrickPaletteGpuMaterialRecord {
    std::uint32_t globalMaterialId{};
    float baseColorX{1.0F};
    float baseColorY{1.0F};
    float baseColorZ{1.0F};
    float roughness{1.0F};
    float metallic{};
    float emissiveX{};
    float emissiveY{};
    float emissiveZ{};
    float opacity{1.0F};
    float normalX{};
    float normalY{};
    float normalZ{1.0F};
    float textureScale{1.0F};
    float detailContrast{};
    std::uint32_t reserved{};
};
static_assert(sizeof(BrickPaletteGpuMaterialRecord) == 64U);

struct BrickPaletteGpuResolveRequest {
    std::uint32_t brickRecordIndex{};
    std::uint32_t sampleIndex{};
    float worldPositionX{};
    float worldPositionY{};
    float worldPositionZ{};
    float assetU{};
    float worldNormalX{};
    float worldNormalY{1.0F};
    float worldNormalZ{};
    float assetV{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
};
static_assert(sizeof(BrickPaletteGpuResolveRequest) == 48U);

struct BrickPaletteResolveRhiStats {
    std::size_t materialCapacityBytes{};
    std::size_t requestCapacityBytes{};
    std::size_t outputCapacityBytes{};
    std::uint64_t uploadedBytes{};
    std::uint64_t publications{};
    std::uint64_t dispatches{};
};

[[nodiscard]] BrickPaletteGpuMaterialRecord pack_brick_palette_gpu_material(
    const BrickPaletteMaterialRecord& material) noexcept;
[[nodiscard]] std::vector<BrickPaletteGpuMaterialRecord> pack_brick_palette_gpu_materials(
    std::span<const BrickPaletteMaterialRecord> materials);

// Packed CPU oracle. It resolves from BrickPaletteUploadPacket rather than the original cooked
// palette, so it independently checks remap indices, byte-packed weights, offsets, runtime paths,
// and fallback representations before a hardware shader is trusted.
[[nodiscard]] BrickPaletteShadedSample resolve_brick_palette_upload_request(
    const BrickPaletteUploadPacket& packet,
    std::span<const BrickPaletteMaterialRecord> materials,
    const BrickPaletteGpuResolveRequest& request,
    std::string* error = nullptr);
[[nodiscard]] bool resolve_brick_palette_upload_requests(
    const BrickPaletteUploadPacket& packet,
    std::span<const BrickPaletteMaterialRecord> materials,
    std::span<const BrickPaletteGpuResolveRequest> requests,
    std::span<BrickPaletteGpuBakedSample> outputs,
    std::string* error = nullptr);

[[nodiscard]] rhi::BindGroupLayoutDesc brick_palette_resolve_bind_group_layout_desc();

// Runtime harness for the compiled brick_palette_resolve compute shader. Null-RHI exercises the
// complete resource/pipeline/dispatch lifetime; Vulkan can execute it when valid SPIR-V and a
// physical device are available. D3D12/Metal source artifacts are produced by the shared shader
// build tooling, but those RHI backends are not present in this engine snapshot.
class BrickPaletteResolveRhiHarness {
public:
    explicit BrickPaletteResolveRhiHarness(rhi::IDevice& device) : device_(device) {}
    ~BrickPaletteResolveRhiHarness();

    BrickPaletteResolveRhiHarness(const BrickPaletteResolveRhiHarness&) = delete;
    BrickPaletteResolveRhiHarness& operator=(const BrickPaletteResolveRhiHarness&) = delete;

    bool publish(const BrickPaletteRhiMirror& paletteMirror,
                 std::span<const BrickPaletteGpuMaterialRecord> materials,
                 std::span<const BrickPaletteGpuResolveRequest> requests,
                 std::string* error = nullptr);
    bool create_pipeline(std::span<const std::byte> bytecode,
                         std::string_view entryPoint = "main",
                         std::string* error = nullptr);
    bool dispatch_and_wait(std::uint32_t requestCount, std::string* error = nullptr);
    bool readback(std::span<BrickPaletteGpuBakedSample> outputs,
                  std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] rhi::BindGroupLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::BindGroupHandle group() const noexcept { return group_; }
    [[nodiscard]] rhi::ComputePipelineHandle pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] const BrickPaletteResolveRhiStats& stats() const noexcept { return stats_; }

private:
    bool ensure_buffer(rhi::BufferHandle& handle, std::size_t& capacity,
                       std::size_t requiredBytes, rhi::BufferUsage usage,
                       std::string_view debugName, std::string* error);
    bool destroy_bindings(std::string* error = nullptr) noexcept;

    rhi::IDevice& device_;
    rhi::BufferHandle materialBuffer_{};
    rhi::BufferHandle requestBuffer_{};
    rhi::BufferHandle outputBuffer_{};
    rhi::BindGroupLayoutHandle layout_{};
    rhi::BindGroupHandle group_{};
    rhi::ComputePipelineHandle pipeline_{};
    std::uint32_t publishedRequestCount_{};
    BrickPaletteResolveRhiStats stats_{};
};

[[nodiscard]] bool load_brick_palette_resolve_spirv(
    const std::filesystem::path& path,
    std::vector<std::byte>& bytecode,
    std::string* error = nullptr);

} // namespace dve::render
