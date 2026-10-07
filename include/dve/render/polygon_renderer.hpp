#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

#include "dve/material_decal.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render_environment.hpp"
#include "dve/transform.hpp"

namespace dve::render {

class PolygonRenderCache;

struct PolygonCamera {
    Float3 position{0.0F, 1.5F, 4.0F};
    Float3 target{0.0F, 0.5F, 0.0F};
    Float3 up{0.0F, 1.0F, 0.0F};
    float verticalFieldOfViewRadians{1.0471975512F};
    float nearPlane{0.05F};
    float farPlane{1000.0F};
};

struct PolygonRenderTarget {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Float4> hdrColor;
    std::vector<float> depth;
    std::vector<std::uint64_t> objectId;
    std::vector<std::uint32_t> materialIndex;

    void resize(std::uint32_t newWidth, std::uint32_t newHeight);
    void clear(Float4 color = {0.02F, 0.03F, 0.05F, 1.0F}, float depthValue = 1.0F);
    [[nodiscard]] bool valid() const noexcept;
};

using PolygonLodLevel = dve::PolygonLodLevel;

struct PolygonRenderInstance {
    std::uint64_t objectId{};
    const CookedPolygonAsset* asset{};
    RigidTransform transform{};
    std::span<const PolygonLodLevel> lods{};
    Float4 tint{1.0F, 1.0F, 1.0F, 1.0F};
    bool visible{true};
};

enum class PolygonMaterialDebugView : std::uint8_t {
    Lit,
    BaseColor,
    WorldNormal,
    Metallic,
    Roughness,
    Emissive,
    Opacity,
    Uv0,
    Uv1,
    TriplanarWeights,
    DetailFade,
    ParallaxSampleCount,
    LayerMask,
    LayerCoverage,
};

struct PolygonRenderOptions {
    bool enableTextures{true};
    bool enableMipSelection{true};
    bool enableTransparentSorting{true};
    bool writeObjectIds{true};
    bool preserveExistingDepth{true};
    PolygonMaterialDebugView materialDebugView{PolygonMaterialDebugView::Lit};
    std::uint32_t materialLayerDebugIndex{};
    // Geometry-agnostic CPU decal records. The same records may be queried by voxel-derived
    // surface evaluators; this span only enables them for the polygon reference pass.
    std::span<const MaterialDecal> decals{};
    bool enableFrustumCulling{true};
    // Positive bias favors coarser meshes: effective diameter = diameter * 2^-bias.
    float lodBias{};
    float lodHysteresis{0.1F};
    // Optional persistent cache, owned by one viewport/render thread. Without it,
    // shared assets still reuse preparation within this render call.
    PolygonRenderCache* cache{};
};

struct PolygonRenderStats {
    std::uint64_t submittedInstances{};
    std::uint64_t culledInstances{};
    std::uint64_t lodSelections{};
    std::uint64_t submittedTriangles{};
    std::uint64_t clippedTriangles{};
    std::uint64_t backfaceCulledTriangles{};
    std::uint64_t rasterizedTriangles{};
    std::uint64_t shadedFragments{};
    std::uint64_t depthRejectedFragments{};
    std::uint64_t alphaRejectedFragments{};
    std::uint64_t transparentFragments{};
    std::uint64_t textureSamples{};
    std::uint64_t triplanarSamples{};
    std::uint64_t detailSamples{};
    std::uint64_t parallaxSamples{};
    std::uint64_t layerMaskSamples{};
    std::uint64_t layerSourceSamples{};
    std::uint64_t layeredFragments{};
    std::uint64_t decalCandidates{};
    std::uint64_t decalProjected{};
    std::uint64_t decalFragments{};
    std::uint64_t frustumCulledInstances{};
    std::uint64_t assetValidations{};
    std::uint64_t mipChainsBuilt{};
    std::uint64_t mipCacheHits{};
    std::uint64_t mipCacheMisses{};
    std::uint64_t mipCacheBytes{};
};

struct PolygonTextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8;
};

struct PolygonTextureMipChain {
    std::vector<PolygonTextureMip> levels;
};

// Retains immutable mip chains by validated content hash, not borrowed pointers.
// The byte budget bounds retained RGBA payload; oversized chains are frame-local.
// Transparent triangles pin their chains until rasterization completes.
class PolygonRenderCache {
public:
    explicit PolygonRenderCache(std::size_t maximumMipBytes = 64U * 1024U * 1024U)
        : maximumMipBytes_(maximumMipBytes) {}
    void clear() noexcept;
    [[nodiscard]] std::size_t resident_mip_bytes() const noexcept { return residentMipBytes_; }
    [[nodiscard]] std::size_t cached_assets() const noexcept { return mips_.size(); }
    [[nodiscard]] std::size_t lod_history_size() const noexcept { return lods_.size(); }
private:
    friend class ReferencePolygonRenderer;
    struct MipEntry {
        std::shared_ptr<const std::vector<PolygonTextureMipChain>> chains;
        std::size_t bytes{};
        std::uint64_t lastUse{};
    };
    struct LodEntry {
        const CookedPolygonAsset* base{};
        std::uint64_t baseHash{};
        const CookedPolygonAsset* selected{};
        float threshold{};
        float bias{};
        std::uint64_t lastUse{};
    };
    std::unordered_map<std::uint64_t, MipEntry> mips_;
    std::unordered_map<std::uint64_t, LodEntry> lods_;
    std::size_t maximumMipBytes_{}, residentMipBytes_{};
    std::uint64_t frame_{};
};

[[nodiscard]] PolygonTextureMipChain generate_texture_mips(const PolygonImage& image);

class ReferencePolygonRenderer {
public:
    [[nodiscard]] PolygonRenderStats render(
        std::span<const PolygonRenderInstance> instances,
        const PolygonCamera& camera,
        const RenderEnvironment& environment,
        PolygonRenderTarget& target,
        const PolygonRenderOptions& options = {}) const;
};

// Depth-aware composition used when a voxel tracer and polygon rasterizer publish independent
// HDR/depth layers. The nearest finite depth owns color and object/material identification.
[[nodiscard]] bool composite_hybrid_layers(
    const PolygonRenderTarget& voxelLayer,
    const PolygonRenderTarget& polygonLayer,
    PolygonRenderTarget& output,
    std::string* error = nullptr);

// Tonemaps the HDR color (ACES filmic on exposure-scaled linear RGB, then sRGB encode) into
// tightly packed RGBA8 (alpha = 255), row-major top-down. This is the single HDR -> display
// conversion shared by write_polygon_render_ppm and the player's CPU presenter.
[[nodiscard]] bool resolve_polygon_render_rgba8(
    const PolygonRenderTarget& target,
    float exposure,
    std::vector<std::uint8_t>& rgba,
    std::string* error = nullptr);

// Operator-selecting resolve. This CPU path is the reference implementation for
// shaders/tonemap.hlsl: ACES is Narkowicz's fitted approximation, Reinhard is
// x / (1 + x), Clamp saturates; all three run once on exposure-scaled linear RGB
// before the sRGB encode, exactly as the shader does.
[[nodiscard]] bool resolve_polygon_render_rgba8(
    const PolygonRenderTarget& target,
    float exposure,
    TonemapOperator tonemapOperator,
    std::vector<std::uint8_t>& rgba,
    std::string* error = nullptr);

[[nodiscard]] bool write_polygon_render_ppm(
    const std::filesystem::path& path,
    const PolygonRenderTarget& target,
    float exposure = 1.0F,
    std::string* error = nullptr);

} // namespace dve::render
