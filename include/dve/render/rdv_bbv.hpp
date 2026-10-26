#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/packed_brickmap.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render::rdv {

// RDV-1/RDV-2: occupancy-only 8^3 bitmask. Bits use DVE's native
// x + 8*y + 64*z order; InstantRDV's Morton/subbrick layout is NOT ABI-compatible.
// This structure deliberately does not replace DVE's authoritative material bricks.
struct BbvBrick {
    std::array<std::uint32_t, 16> occupancy{};
};
static_assert(sizeof(BbvBrick) == 64);

struct BbvHit {
    Int3 voxel{};
    Int3 normal{};
    float distance{}; // voxel-index units, never metres
};

struct BbvUpdateStats {
    std::size_t changedBricks{};
    std::size_t examinedBricks{};
    std::size_t dirtyBricks{};
    bool recentered{};
    bool fullRefresh{};
};

// CPU reference and toroidal GPU-buffer source for native-voxel occupancy.
// The initial source is a PackedBrickmapScene; raster-derived polygon injection
// is explicitly out of scope until RDV-3. One instance belongs to one viewport.
// Dims are bounded to limit CPU and GPU allocations.
class BbvClipmap {
public:
    explicit BbvClipmap(Int3 dimensions = {16, 16, 16});

    // center is an absolute WORLD BRICK key; no camera-relative float drift.
    // Moving by one brick retains physical GPU slots for the overlapping region.
    [[nodiscard]] BbvUpdateStats recenter(BrickKey center, const PackedBrickmapScene& source);
    // Camera is in metres; metresPerVoxel is DVE's explicit geometry scale.
    // Sub-brick camera movements keep all physical slots and upload nothing.
    [[nodiscard]] BbvUpdateStats recenter_camera(Float3 cameraWorldMeters,
                                                  float metresPerVoxel,
                                                  const PackedBrickmapScene& source);
    // Reconcile edits after PackedBrickmapScene has published the authoritative values.
    [[nodiscard]] BbvUpdateStats refresh(std::span<const BrickKey> changedKeys,
                                          const PackedBrickmapScene& source);
    // Conservative reconciliation when an authoritative revision changed but
    // the caller cannot prove its edit-key list is complete.
    [[nodiscard]] BbvUpdateStats refresh_all(const PackedBrickmapScene& source);
    void mark_all_dirty() noexcept;
    void clear_dirty() noexcept;

    // Independent voxel-DDA correctness oracle, clipped to this clipmap volume.
    // For exact material, refraction or transparent queries, use DVE's existing tracer.
    [[nodiscard]] std::optional<BbvHit> trace(Float3 origin, Float3 direction,
                                               float maxDistance) const noexcept;
    [[nodiscard]] bool occupied_at(Int3 voxel) const noexcept;
    [[nodiscard]] const BbvBrick* find_brick(BrickKey key) const noexcept;
    [[nodiscard]] std::size_t dirty_count() const noexcept;
    [[nodiscard]] std::vector<std::size_t> dirty_indices() const;

    [[nodiscard]] Int3 dimensions() const noexcept { return dimensions_; }
    [[nodiscard]] BrickKey minimum_key() const noexcept { return minimum_; }
    [[nodiscard]] Int3 toroidal_offset() const noexcept { return offset_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] std::span<const BbvBrick> bricks() const noexcept { return bricks_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bricks_.size() * sizeof(BbvBrick); }

private:
    Int3 dimensions_{};
    BrickKey minimum_{};
    Int3 offset_{};
    bool initialized_{};
    std::vector<BrickKey> physicalKeys_;
    std::vector<BbvBrick> bricks_;
    std::vector<std::uint8_t> dirty_;

    [[nodiscard]] std::optional<std::size_t> physical_index(BrickKey key) const noexcept;
    [[nodiscard]] bool publish_key(BrickKey key, const PackedBrickmapScene& source,
                                   bool forceDirty) noexcept;
};

// RDV-2: persistent occupancy buffer. Uploads only coalesced dirty ranges; no
// per-frame 16-MiB reupload on an unchanged camera. Shader consumers use
// (minimum_key, dimensions, toroidal_offset) to address this 64-byte/brick buffer.
// A failed publication retains dirty flags so a retry cannot silently lose changes.
class BbvRhiMirror {
public:
    explicit BbvRhiMirror(rhi::IDevice& device) noexcept : device_(device) {}
    ~BbvRhiMirror();
    BbvRhiMirror(const BbvRhiMirror&) = delete;
    BbvRhiMirror& operator=(const BbvRhiMirror&) = delete;

    [[nodiscard]] bool publish(BbvClipmap& clipmap, std::string* error = nullptr);
    [[nodiscard]] bool readback_matches(const BbvClipmap& clipmap,
                                         std::string* error = nullptr) const;
    void reset() noexcept;
    [[nodiscard]] rhi::BufferHandle buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::uint64_t bytes_uploaded() const noexcept { return bytesUploaded_; }
    [[nodiscard]] std::uint64_t upload_ranges() const noexcept { return uploadRanges_; }

private:
    rhi::IDevice& device_;
    rhi::BufferHandle buffer_{};
    std::size_t capacity_{};
    std::uint64_t bytesUploaded_{};
    std::uint64_t uploadRanges_{};
};

// RDV-1 offscreen compute ABI; mirrors rdv_bbv_trace.hlsl (48 bytes per hit).
struct BbvGpuHit {
    Int3 voxel{};
    std::uint32_t hit{};
    Int3 normal{};
    std::uint32_t padding{};
    float distance{};
    std::uint32_t voxelSteps{};
    std::uint32_t brickLookups{};
    std::uint32_t padding2{};
};
static_assert(sizeof(BbvGpuHit) == 48);

struct BbvTraceConstants {
    Int3 minimumBrick{};
    std::uint32_t rayCount{};
    Int3 gridDimensions{};
    std::uint32_t maximumSteps{65536};
    Int3 toroidalOffset{};
    std::uint32_t padding{};
};
static_assert(sizeof(BbvTraceConstants) == 48);

// Opt-in GPU reference dispatch. Caller provides compiled compute shader bytecode
// for the selected backend. Vulkan SPIR-V must use DVE's 0/16/32 b/t/u shifts.
// Null-RHI checks dispatch/resources only; native GPU is needed for hit parity.
// The mirror and the supplied device MUST refer to the same IDevice instance.
[[nodiscard]] bool execute_bbv_gpu_trace(rhi::IDevice& device, BbvRhiMirror& mirror,
                                         BbvClipmap& clipmap,
                                         std::span<const GpuTraceRay> rays,
                                         std::span<const std::byte> bytecode,
                                         std::vector<BbvGpuHit>& results,
                                         std::string* error = nullptr);

} // namespace dve::render::rdv
