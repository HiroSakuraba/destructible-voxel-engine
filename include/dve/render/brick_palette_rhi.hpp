#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/brick_palette_asset_cooker.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

inline constexpr std::uint32_t kBrickPaletteRecordBinding = 0U;
inline constexpr std::uint32_t kBrickPaletteRemapBinding = 1U;
inline constexpr std::uint32_t kBrickPaletteSlotBinding = 2U;
inline constexpr std::uint32_t kBrickPaletteSampleBinding = 3U;
inline constexpr std::uint32_t kBrickPaletteBakedBinding = 4U;
inline constexpr std::uint32_t kBrickPaletteSingleBinding = 5U;
inline constexpr std::uint32_t kBrickPaletteBindingCount = 6U;
inline constexpr std::uint32_t kInvalidBrickPaletteOffset = 0xFFFFFFFFU;

enum class BrickPalettePipelineVariant : std::uint8_t {
    Palette2AssetUv,
    Palette2WorldTriplanar,
    Palette4AssetUv,
    Palette4WorldTriplanar,
};

struct BrickPalettePipelineContract {
    BrickPalettePipelineVariant variant{BrickPalettePipelineVariant::Palette2AssetUv};
    VoxelMaterialRuntimePath runtimePath{VoxelMaterialRuntimePath::DeferredPalette2};
    BrickPaletteMappingMode mapping{BrickPaletteMappingMode::AssetUv};
    std::uint8_t maximumSlots{2U};
    std::array<std::uint32_t, kBrickPaletteBindingCount> bindings{
        kBrickPaletteRecordBinding,
        kBrickPaletteRemapBinding,
        kBrickPaletteSlotBinding,
        kBrickPaletteSampleBinding,
        kBrickPaletteBakedBinding,
        kBrickPaletteSingleBinding};
};

struct BrickPaletteGpuBrickRecord {
    BrickKey key{};
    std::uint32_t sourceGeneration{};
    std::uint32_t runtimePath{};
    std::uint32_t paletteSlotOffset{kInvalidBrickPaletteOffset};
    std::uint32_t paletteSlotCount{};
    std::uint32_t sampleOffset{kInvalidBrickPaletteOffset};
    std::uint32_t sampleCount{};
    std::uint32_t bakedOffset{kInvalidBrickPaletteOffset};
    std::uint32_t singleOffset{kInvalidBrickPaletteOffset};
    std::uint32_t mappingMode{};
};
static_assert(sizeof(BrickPaletteGpuBrickRecord) == 48U);

struct BrickPaletteGpuSampleEncoding {
    std::uint32_t packedSlotIndices{};
    std::uint32_t packedWeights{};
};
static_assert(sizeof(BrickPaletteGpuSampleEncoding) == 8U);

struct BrickPaletteGpuBakedSample {
    std::array<float, 4U> baseColorOpacity{};
    std::array<float, 4U> normalRoughness{};
    std::array<float, 4U> emissiveMetallic{};
    std::uint32_t valid{};
    std::uint32_t textureSamples{};
    std::uint32_t slotsEvaluated{};
    std::uint32_t reserved{};
};
static_assert(sizeof(BrickPaletteGpuBakedSample) == 64U);

struct BrickPaletteSubmissionSettings {
    VoxelMaterialRuntimePath preferredPath{VoxelMaterialRuntimePath::DeferredPalette4};
    BrickPaletteMappingMode mapping{BrickPaletteMappingMode::WorldTriplanar};
    bool allowFallback{true};
};

struct BrickPaletteUploadPacket {
    std::uint64_t sourceGeneration{};
    std::uint64_t dependencyKey{};
    std::vector<BrickPaletteGpuBrickRecord> records;
    std::vector<std::uint32_t> remapGlobalMaterialIds;
    std::vector<std::uint32_t> paletteSlots;
    std::vector<BrickPaletteGpuSampleEncoding> paletteSamples;
    std::vector<BrickPaletteGpuBakedSample> bakedSamples;
    std::vector<std::uint32_t> singleMaterialIds;
    std::uint64_t fallbackBrickCount{};
    std::uint64_t rejectedBrickCount{};
    std::uint64_t contentHash{};
};

[[nodiscard]] BrickPalettePipelineContract brick_palette_pipeline_contract(
    VoxelMaterialRuntimePath path, BrickPaletteMappingMode mapping) noexcept;
[[nodiscard]] std::array<BrickPalettePipelineContract, 4U>
brick_palette_pipeline_contracts() noexcept;
[[nodiscard]] rhi::BindGroupLayoutDesc brick_palette_bind_group_layout_desc();

[[nodiscard]] BrickPaletteUploadPacket build_brick_palette_upload_packet(
    const CookedBrickPaletteAsset& asset,
    const BrickPaletteSubmissionSettings& settings = {});
[[nodiscard]] bool validate_brick_palette_upload_packet(
    const BrickPaletteUploadPacket& packet, std::string* error = nullptr);

struct BrickPaletteRhiMirrorStats {
    std::array<std::size_t, kBrickPaletteBindingCount> capacityBytes{};
    std::uint64_t uploadedBytes{};
    std::uint64_t publications{};
    std::uint64_t reallocations{};
};

class BrickPaletteRhiMirror {
public:
    explicit BrickPaletteRhiMirror(rhi::IDevice& device) : device_(device) {}
    ~BrickPaletteRhiMirror();

    BrickPaletteRhiMirror(const BrickPaletteRhiMirror&) = delete;
    BrickPaletteRhiMirror& operator=(const BrickPaletteRhiMirror&) = delete;

    bool upload(const BrickPaletteUploadPacket& packet, std::string* error = nullptr);
    [[nodiscard]] bool readback_matches(const BrickPaletteUploadPacket& packet,
                                        std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] rhi::BufferHandle record_buffer() const noexcept { return buffers_[0U]; }
    [[nodiscard]] rhi::BufferHandle remap_buffer() const noexcept { return buffers_[1U]; }
    [[nodiscard]] rhi::BufferHandle slot_buffer() const noexcept { return buffers_[2U]; }
    [[nodiscard]] rhi::BufferHandle sample_buffer() const noexcept { return buffers_[3U]; }
    [[nodiscard]] rhi::BufferHandle baked_buffer() const noexcept { return buffers_[4U]; }
    [[nodiscard]] rhi::BufferHandle single_buffer() const noexcept { return buffers_[5U]; }
    [[nodiscard]] const BrickPaletteRhiMirrorStats& stats() const noexcept { return stats_; }

private:
    bool ensure_buffer(std::size_t index, std::size_t requiredBytes,
                       std::string_view debugName, std::string* error);

    rhi::IDevice& device_;
    std::array<rhi::BufferHandle, kBrickPaletteBindingCount> buffers_{};
    std::array<std::size_t, kBrickPaletteBindingCount> uploadedBytes_{};
    BrickPaletteRhiMirrorStats stats_{};
};

struct BrickPaletteRhiBindings {
    rhi::BindGroupLayoutHandle layout{};
    rhi::BindGroupHandle group{};
};

[[nodiscard]] bool create_brick_palette_rhi_bindings(
    rhi::IDevice& device,
    const BrickPaletteRhiMirror& mirror,
    BrickPaletteRhiBindings& output,
    std::string* error = nullptr);
[[nodiscard]] bool destroy_brick_palette_rhi_bindings(
    rhi::IDevice& device,
    BrickPaletteRhiBindings& bindings,
    std::string* error = nullptr);

[[nodiscard]] const char* to_string(BrickPalettePipelineVariant value) noexcept;

} // namespace dve::render
