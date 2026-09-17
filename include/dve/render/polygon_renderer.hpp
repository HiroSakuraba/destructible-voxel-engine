#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "dve/material_decal.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render_environment.hpp"
#include "dve/transform.hpp"

namespace dve::render {

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

struct PolygonLodLevel {
    const CookedPolygonAsset* asset{};
    // Select this level when projected diameter is at least this many pixels.
    float minimumProjectedDiameterPixels{};
};

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
};

struct PolygonTextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8;
};

struct PolygonTextureMipChain {
    std::vector<PolygonTextureMip> levels;
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

[[nodiscard]] bool write_polygon_render_ppm(
    const std::filesystem::path& path,
    const PolygonRenderTarget& target,
    float exposure = 1.0F,
    std::string* error = nullptr);

} // namespace dve::render
