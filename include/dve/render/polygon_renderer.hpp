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
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/surface_shading.hpp"
#include "dve/transform.hpp"

namespace dve {
struct PolygonBvhHit;
}

namespace dve::render {
class SceneLighting;

class PolygonRenderCache;

struct PolygonCamera {
    Float3 position{0.0F, 1.5F, 4.0F};
    Float3 target{0.0F, 0.5F, 0.0F};
    Float3 up{0.0F, 1.0F, 0.0F};
    float verticalFieldOfViewRadians{1.0471975512F};
    float nearPlane{0.05F};
    float farPlane{1000.0F};
};

// Render passes (AOVs), filled by both reference renderers when enabled on the target.
//
// Light passes: at every pixel they sum to hdrColor (rgb), so a compositor can grade, tint or
// drop any of them and rebuild the image. Translucent surfaces blend every light pass by their
// alpha, exactly as they blend hdrColor; background is what the target was cleared to (the sky)
// and fog what apply_distance_fog added.
//
// Data passes (not part of the sum), from the nearest opaque surface:
//   shadow        - the direct sun light the shadows removed: image + shadow = unshadowed image
//                   (tint it to colour shadows). Zero on polygons (the polygon reference casts no
//                   sun shadows) and where nothing was drawn.
//   sunVisibility - 1 lit .. 0 fully shadowed (shadow strength applied); 1 where nothing was drawn.
//   normal        - world-space shading normal; zero where nothing was drawn.
//   depthMeters   - camera-space Z in metres; +infinity where nothing was drawn.
struct RenderPassBuffers {
    bool enabled{};
    std::vector<Float3> directDiffuse;
    std::vector<Float3> directSpecular;
    std::vector<Float3> indirectDiffuse;
    std::vector<Float3> indirectSpecular;
    std::vector<Float3> emission;
    std::vector<Float3> transmission;
    std::vector<Float3> background;
    std::vector<Float3> fog;
    std::vector<Float3> shadow;
    std::vector<float> sunVisibility;
    std::vector<Float3> normal;
    std::vector<float> depthMeters;

    // The light passes summed (equals hdrColor.rgb).
    [[nodiscard]] Float3 light_sum(std::size_t index) const noexcept;
};

// What one surface sample contributes to the passes.
struct RenderPassSample {
    LightPasses light{};
    Float3 shadow{};
    float sunVisibility{1.0F};
    Float3 normal{};
    float depthMeters{};
};

struct PolygonRenderTarget {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Float4> hdrColor;
    std::vector<float> depth;
    std::vector<std::uint64_t> objectId;
    std::vector<std::uint32_t> materialIndex;
    RenderPassBuffers passes;   // empty unless enable_passes()

    void resize(std::uint32_t newWidth, std::uint32_t newHeight);
    void clear(Float4 color = {0.02F, 0.03F, 0.05F, 1.0F}, float depthValue = 1.0F);
    // Allocates (or frees) the render passes; cleared to an empty frame.
    void enable_passes(bool enabled = true);
    // After writing hdrColor directly for the background (a sky gradient): the background pass
    // takes hdrColor wherever nothing was drawn.
    void sync_background_pass();
    [[nodiscard]] bool valid() const noexcept;
    // Writes a surface's passes at a pixel: opaque replaces them, alpha < 1 blends the light
    // passes over what is there (data passes are left to the opaque surface behind).
    void write_passes(std::size_t index, const RenderPassSample& sample, float alpha, bool opaque) noexcept;
};

// Exponential distance fog (RenderEnvironment::fogDensityPerMeter), applied after the frame is
// drawn, from its depth: every drawn pixel keeps T = exp(-density x max(0, distance - start))
// of its light and gains fogColor x (1 - T), distance being metres along the view ray. Light
// passes are scaled by T and the fog pass gets the rest, so they still sum to the image. The
// background (nothing drawn) is left alone. Returns the pixels fogged.
std::uint64_t apply_distance_fog(PolygonRenderTarget& target, const PolygonCamera& camera,
                                 const RenderEnvironment& environment, float metersPerUnit);

// Physical sky (RenderEnvironment::skyMode Physical; both do nothing in Colors mode):
//
// paint_sky_background fills every pixel nothing has been drawn in (depth 1) with the sky seen
// along its ray and the sun disc (render::background_radiance) and sets the background pass to
// it. Paint it before translucent surfaces are drawn, so they blend over the sky. Returns the
// pixels painted.
//
// apply_aerial_perspective adds the air between the camera and each drawn pixel: the pixel keeps
// the air's transmittance T of its light and gains the light the air scatters toward the camera
// (AtmosphereModel::segment over the distance along the ray x aerialPerspectiveScale, the camera
// at observerAltitudeMeters + its y in metres). Light passes are scaled by T and the scattered
// light goes to the fog pass, so the passes still sum. Returns the pixels changed.
std::uint64_t paint_sky_background(PolygonRenderTarget& target, const PolygonCamera& camera, const RenderEnvironment& environment);
std::uint64_t apply_aerial_perspective(PolygonRenderTarget& target, const PolygonCamera& camera,
                                       const RenderEnvironment& environment, float metersPerUnit);

// Writes beauty and every pass as PFM images (32-bit float, little-endian): beauty, the light
// passes, shadow, normal (rgb), sun_visibility and depth (greyscale, metres; empty pixels hold
// the far plane in metres as PFM has no infinity convention). File names are
// <prefix><pass>.pfm. Fails when passes are not enabled.
[[nodiscard]] bool write_render_passes_pfm(const PolygonRenderTarget& target, const std::filesystem::path& directory,
                                           const std::string& prefix, float farPlaneMeters, std::string* error = nullptr);

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

struct CpuSurfaceDetailSettings {
    float heightBumpStrength{};
    bool parallaxSelfShadow{};
    std::uint32_t shadowSteps{12};
    // Explicit physical shell depth, separate from UV parallax scale. Opt-in;
    // raster coverage/silhouettes remain those of the base mesh.
    float reliefDepthMeters{};
    std::uint32_t maximumParallaxSteps{128};
};

struct PolygonRenderOptions {
    const SceneLighting* sceneLighting{}; // shared frame integrator; outlives render
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
    // World units to metres, for metre-authored material distances (subsurface scatter
    // distance). The hybrid reference passes its metres per voxel.
    float metersPerUnit{kDefaultMetersPerVoxel};
    // Subsurface diffusion samples per Subsurface fragment (front-lit light spreading under the
    // surface with Burley's profile). 0 = off. Off as well when the subsurface max distance is 0.
    std::uint32_t subsurfaceDiffusionSamples{16};
    // Ambient occlusion of the polygon ambient (RenderEnvironment::ambientOcclusionSamples, same
    // rays and weighting as the voxel reference and the GPU passes). Rays go to occlusionRay
    // when set (world units; the hybrid reference and the CPU player trace voxels and polygons
    // together), otherwise to the polygons being rendered.
    AmbientOcclusionRay occlusionRay{};
    CpuSurfaceDetailSettings surfaceDetail{};
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
    std::uint64_t parallaxFallbacks{}, parallaxShadowSamples{}, bumpSamples{}, reliefDepthWrites{};
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
    // Subsurface translucency marches from shaded-side fragments, and those that left the mesh
    // within the subsurface max distance.
    std::uint64_t subsurfaceMarches{};
    std::uint64_t subsurfaceExits{};
    std::uint64_t subsurfaceDiffusionProbes{};
    std::uint64_t ambientOcclusionRays{};
};

struct PolygonTextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8;
};

struct PolygonTextureMipChain {
    std::vector<PolygonTextureMip> levels;
    Float2 heightBounds{0,1};
    std::vector<PolygonTextureMip> normalLevels;
    std::vector<PolygonTextureMip> srgbLevels;
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

[[nodiscard]] PolygonTextureMipChain generate_texture_mips(const PolygonImage& image,
    bool normalMap = false, bool srgb = false);

// Shared ray-hit evaluation of the authored base/MR/normal/emissive/coverage/detail/layer
// maps. Uses triangle metric derivatives and a ray-cone footprint for mip selection.
// Exact destination-pixel differentials for UV baking; coordinates use world/asset units.
struct PolygonSurfaceDifferentials { Float3 worldDx{},worldDy{}; };
struct PolygonHitSurface { VoxelMaterialDefinition material; Float3 normal{0,0,1}; };
[[nodiscard]] PolygonHitSurface sample_polygon_hit_material(const CookedPolygonAsset&,const PolygonBvhHit&,
    const RigidTransform&,Float3 worldNormal,const std::vector<PolygonTextureMipChain>&,
    float metersPerUnit=1,float footprintMeters=0,float viewDistanceMeters=0,
    const PolygonSurfaceDifferentials* differentials=nullptr) noexcept;

// Opacity of a polygon surface at a BVH ray hit, as the reference rasterizer computes it for its
// alpha test: material alpha x vertex colour alpha x base-colour texture alpha x opacity texture
// x tint alpha, at the hit's interpolated texture coordinates (finest mip; triplanar mappings at
// the hit's object/world position). `mips` holds one chain per asset image
// (generate_texture_mips). For shadow rays through Masked surfaces.
[[nodiscard]] float polygon_hit_opacity(const CookedPolygonAsset& asset, const PolygonBvhHit& hit, Float3 worldPosition,
                                        Float3 worldNormal, const std::vector<PolygonTextureMipChain>& mips,
                                        float tintAlpha = 1.0F) noexcept;

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
