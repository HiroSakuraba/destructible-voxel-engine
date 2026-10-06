#include "dve/player/player_renderer.hpp"

#include "dve/polygon_asset.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace dve::player {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Palette for voxel objects without a cooked material table (create_object/spawn_box
// objects): stable, readable colors per material id instead of the magenta diagnostic.
const std::array<VoxelMaterialDefinition, 256>& default_palette() {
    static const std::array<VoxelMaterialDefinition, 256> palette = [] {
        std::array<VoxelMaterialDefinition, 256> result{};
        for (std::size_t id = 0; id < result.size(); ++id) {
            const float hue = static_cast<float>((id * 47U) % 360U) / 60.0F;
            const float x = 1.0F - std::abs(std::fmod(hue, 2.0F) - 1.0F);
            Float3 rgb{};
            if (hue < 1) rgb = {1, x, 0};
            else if (hue < 2) rgb = {x, 1, 0};
            else if (hue < 3) rgb = {0, 1, x};
            else if (hue < 4) rgb = {0, x, 1};
            else if (hue < 5) rgb = {x, 0, 1};
            else rgb = {1, 0, x};
            result[id].name = "default";
            result[id].baseColor = {0.25F + 0.5F * rgb.x, 0.25F + 0.5F * rgb.y, 0.25F + 0.5F * rgb.z, 1.0F};
            result[id].roughness = 0.8F;
        }
        return result;
    }();
    return palette;
}

class CpuPlayerRenderer final : public IPlayerRenderer {
public:
    CpuPlayerRenderer(IFrameBlitter* blitter, CpuPlayerRendererOptions options)
        : blitter_(blitter), options_(options) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "cpu"; }

    bool resize(std::uint32_t width, std::uint32_t height, std::string* error) override {
        if (width == 0U || height == 0U || width > 8192U || height > 8192U) {
            if (error) *error = "CPU render size must be between 1x1 and 8192x8192";
            return false;
        }
        target_.resize(width, height);
        frame_.width = width;
        frame_.height = height;
        frame_.pixels.assign(static_cast<std::size_t>(width) * height * 4U, 0U);
        return true;
    }

    bool render(const PlayerRenderView& view, std::string* error) override {
        if (!target_.valid()) {
            if (error) *error = "renderer has no size; call resize() first";
            return false;
        }
        stats_ = {};
        const auto start = Clock::now();

        // The reference tracer works in voxel-index units (1 unit = 1 voxel, rigid
        // transforms). Pick the scene's voxel size from the first voxel object and convert
        // every metre-space transform and the camera with it.
        float voxelSize = 0.0F;
        for (const GameRenderObject& object : view.objects) {
            if (object.enabled && object.kind == GameGeometryKind::Voxel && object.voxels != nullptr &&
                object.voxelSizeMeters > 0.0F) {
                voxelSize = object.voxelSizeMeters;
                break;
            }
        }
        if (!(voxelSize > 0.0F)) voxelSize = kDefaultMetersPerVoxel;
        const float toUnits = 1.0F / voxelSize;

        voxels_.clear();
        polygons_.clear();
        for (const GameRenderObject& object : view.objects) {
            if (!object.enabled) continue;
            RigidTransform transform = object.transform;
            transform.position = multiply(transform.position, toUnits);
            if (object.kind == GameGeometryKind::Voxel && object.voxels != nullptr) {
                if (std::abs(object.voxelSizeMeters - voxelSize) > 1.0e-6F * voxelSize) {
                    ++stats_.skippedObjects; // mixed voxel sizes need per-instance scale (GPU path)
                    continue;
                }
                std::span<const VoxelMaterialDefinition> materials = object.materials;
                if (materials.empty()) materials = default_palette();
                voxels_.push_back({object.id, object.voxels, transform, materials, true});
            } else if (object.kind == GameGeometryKind::Polygon && object.polygon != nullptr) {
                const CookedPolygonAsset* scaled = scaled_polygon(object.id, *object.polygon, toUnits);
                polygons_.push_back({object.id, scaled, transform, {}, {1, 1, 1, 1}, true});
            }
        }
        prune_polygon_cache(view.objects);
        stats_.voxelInstances = voxels_.size();
        stats_.polygonInstances = polygons_.size();

        render::PolygonCamera camera = view.camera;
        camera.position = multiply(camera.position, toUnits);
        camera.target = multiply(camera.target, toUnits);
        camera.nearPlane *= toUnits;
        camera.farPlane *= toUnits;

        const RenderEnvironment environment = apply_cpu_render_quality(view.environment, options_.quality);
        clear_background(environment);
        render::ReferenceVoxelRenderer voxelRenderer;
        voxelRenderer.metersPerVoxel = voxelSize;
        voxelRenderer.threadCount = options_.threadCount;
        voxelRenderer.radianceCascades.threadCount = options_.threadCount;
        const auto voxelStats = voxelRenderer.render(voxels_, camera, environment, target_, true);
        stats_.tracedRays = voxelStats.tracedRays;
        stats_.hitRays = voxelStats.hitRays;
        if (!polygons_.empty()) {
            render::PolygonRenderOptions polygonOptions;
            polygonOptions.preserveExistingDepth = true;
            (void)render::ReferencePolygonRenderer{}.render(polygons_, camera, environment, target_,
                                                            polygonOptions);
        }
        stats_.renderMilliseconds = milliseconds_since(start);

        const auto resolveStart = Clock::now();
        const float exposure = options_.exposure * (environment.exposure > 0.0F ? environment.exposure : 1.0F);
        if (!render::resolve_polygon_render_rgba8(target_, exposure, frame_.pixels, error)) return false;
        stats_.resolveMilliseconds = milliseconds_since(resolveStart);
        rendered_ = true;
        return true;
    }

    bool present(std::string* error) override {
        if (!rendered_) {
            if (error) *error = "nothing rendered yet";
            return false;
        }
        const auto start = Clock::now();
        const bool ok = blitter_ == nullptr || blitter_->blit(frame_, error);
        stats_.presentMilliseconds = milliseconds_since(start);
        return ok;
    }

    [[nodiscard]] const Rgba8Image* readback() const noexcept override { return rendered_ ? &frame_ : nullptr; }
    [[nodiscard]] PlayerRenderStats last_stats() const noexcept override { return stats_; }

private:
    struct ScaledPolygon {
        const CookedPolygonAsset* source{};
        std::uint64_t contentHash{};
        float scale{};
        CookedPolygonAsset asset;
    };

    const CookedPolygonAsset* scaled_polygon(GameObjectId id, const CookedPolygonAsset& source, float scale) {
        ScaledPolygon& entry = polygonCache_[id];
        if (entry.source != &source || entry.contentHash != source.contentHash || entry.scale != scale) {
            entry.source = &source;
            entry.contentHash = source.contentHash;
            entry.scale = scale;
            entry.asset = source;
            for (PolygonVertex& vertex : entry.asset.vertices) vertex.position = multiply(vertex.position, scale);
            entry.asset.bounds.minimum = multiply(entry.asset.bounds.minimum, scale);
            entry.asset.bounds.maximum = multiply(entry.asset.bounds.maximum, scale);
            // The renderer validates every instance, including the content hash: re-hash the
            // scaled copy, or every polygon is culled as corrupt (nothing drew before this).
            entry.asset.contentHash = polygon_asset_content_hash(entry.asset);
        }
        return &entry.asset;
    }

    void prune_polygon_cache(std::span<const GameRenderObject> objects) {
        if (polygonCache_.size() <= polygons_.size()) return;
        for (auto it = polygonCache_.begin(); it != polygonCache_.end();) {
            const bool live = std::any_of(objects.begin(), objects.end(), [&](const GameRenderObject& object) {
                return object.id == it->first && object.polygon != nullptr;
            });
            it = live ? std::next(it) : polygonCache_.erase(it);
        }
    }

    void clear_background(const RenderEnvironment& environment) {
        target_.clear({environment.skyColor.x, environment.skyColor.y, environment.skyColor.z, 1.0F}, 1.0F);
        if (!options_.skyGradient) return;
        for (std::uint32_t y = 0; y < target_.height; ++y) {
            const float t = static_cast<float>(y) / static_cast<float>(std::max(1U, target_.height - 1U));
            const float zenith = 1.6F - 0.9F * t; // brighter toward the top
            const Float4 color{environment.skyColor.x * zenith, environment.skyColor.y * zenith,
                               environment.skyColor.z * (zenith + 0.15F), 1.0F};
            std::fill_n(target_.hdrColor.begin() + static_cast<std::ptrdiff_t>(y) * target_.width,
                        target_.width, color);
        }
    }

    IFrameBlitter* blitter_{};
    CpuPlayerRendererOptions options_{};
    render::PolygonRenderTarget target_;
    Rgba8Image frame_;
    bool rendered_{};
    PlayerRenderStats stats_{};
    std::vector<render::VoxelReferenceInstance> voxels_;
    std::vector<render::PolygonRenderInstance> polygons_;
    std::unordered_map<GameObjectId, ScaledPolygon> polygonCache_;
};

} // namespace

std::string_view cpu_render_quality_name(CpuRenderQuality quality) noexcept {
    switch (quality) {
    case CpuRenderQuality::Fast: return "fast";
    case CpuRenderQuality::Balanced: return "balanced";
    case CpuRenderQuality::Reference: return "reference";
    }
    return "unknown";
}

bool parse_cpu_render_quality(std::string_view text, CpuRenderQuality& quality) noexcept {
    for (CpuRenderQuality candidate : {CpuRenderQuality::Fast, CpuRenderQuality::Balanced,
                                       CpuRenderQuality::Reference}) {
        if (text == cpu_render_quality_name(candidate)) {
            quality = candidate;
            return true;
        }
    }
    return false;
}

RenderEnvironment apply_cpu_render_quality(RenderEnvironment environment, CpuRenderQuality quality) noexcept {
    if (quality == CpuRenderQuality::Reference) return environment;
    if (environment.shadowMode != ShadowMode::Off) environment.shadowMode = ShadowMode::Hard;
    if (quality == CpuRenderQuality::Fast) {
        if (environment.globalIlluminationMode != GlobalIlluminationMode::Off)
            environment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    } else {
        environment.globalIlluminationSamples = std::min(environment.globalIlluminationSamples, 2U);
    }
    return environment;
}

std::unique_ptr<IPlayerRenderer> make_cpu_player_renderer(IFrameBlitter* blitter, CpuPlayerRendererOptions options) {
    return std::make_unique<CpuPlayerRenderer>(blitter, options);
}

bool MemoryFrameBlitter::blit(const Rgba8Image& frame, std::string* error) {
    if (!frame.valid()) {
        if (error) *error = "invalid frame";
        return false;
    }
    last_ = frame;
    ++frames_;
    return true;
}

} // namespace dve::player
