#pragma once
// Rendering seam of dve_player. The frame loop, content and GameWorld code only see
// IPlayerRenderer; a backend turns a PlayerRenderView (backend-neutral: GameWorld's
// read-only render objects + a metre-space camera + the environment) into pixels and owns
// presentation.
//
//   CPU (today):   make_cpu_player_renderer() traces the view with render_bridge's
//                  ReferenceVoxelRenderer + ReferencePolygonRenderer (multithreaded) into a
//                  PolygonRenderTarget, tonemaps it to RGBA8 and hands it to an IFrameBlitter
//                  (SDL streaming texture in dve_player, memory in tests).
//   GPU (future):  a VulkanPlayerRenderer / D3D12PlayerRenderer implements IPlayerRenderer
//                  directly, creates its swapchain from platform::NativeWindowHandle and
//                  ignores the blitter. Selection is `dve_player --renderer`; only `cpu`
//                  exists now.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "dve/game_world.hpp"
#include "dve/player/frame_image.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render_environment.hpp"

namespace dve::player {

struct PlayerRenderView {
    std::span<const GameRenderObject> objects;   // GameWorld::render_objects()
    render::PolygonCamera camera;                // world space, metres
    RenderEnvironment environment;
    bool polygonFrustumCulling{true};
    float polygonLodBias{};
};

struct PlayerRenderStats {
    double renderMilliseconds{};   // scene trace/raster
    double resolveMilliseconds{};  // HDR -> RGBA8
    double presentMilliseconds{};  // blit / swap
    std::uint64_t voxelInstances{};
    std::uint64_t polygonInstances{};
    std::uint64_t skippedObjects{}; // e.g. voxel size differs from the scene's (CPU path)
    std::uint64_t tracedRays{};
    std::uint64_t hitRays{};
    render::PolygonRenderStats polygons;
    std::uint64_t scaledPolygonAssets{};
    std::uint64_t scaledPolygonCopies{};
};

// Receives finished frames (CPU backends). Implementations: SDL streaming texture, memory.
class IFrameBlitter {
public:
    virtual ~IFrameBlitter() = default;
    [[nodiscard]] virtual bool blit(const Rgba8Image& frame, std::string* error) = 0;
};

class IPlayerRenderer {
public:
    virtual ~IPlayerRenderer() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    // Internal render resolution (the presenter scales it to the window).
    [[nodiscard]] virtual bool resize(std::uint32_t width, std::uint32_t height, std::string* error) = 0;
    [[nodiscard]] virtual bool render(const PlayerRenderView& view, std::string* error) = 0;
    [[nodiscard]] virtual bool present(std::string* error) = 0;
    // Last rendered frame (tests, --hash, screenshots). May be null before the first render.
    [[nodiscard]] virtual const Rgba8Image* readback() const noexcept = 0;
    [[nodiscard]] virtual PlayerRenderStats last_stats() const noexcept = 0;
};

// Lighting cost of the CPU renderer. The reference tracer is a validation path (one DDA per
// instance per ray), so the default trades soft shadows and traced GI for real-time frame rates.
enum class CpuRenderQuality : std::uint32_t {
    // Hard sun shadows (1 ray/pixel), hemisphere ambient instead of traced GI.
    Fast = 0,
    // Hard shadows plus the environment's GI mode, capped at 2 samples.
    Balanced = 1,
    // RenderEnvironment exactly as the scene/script set it (engine default: soft shadows,
    // 4-sample one-bounce GI). Offline/screenshot use.
    Reference = 2,
};

[[nodiscard]] std::string_view cpu_render_quality_name(CpuRenderQuality quality) noexcept;
[[nodiscard]] bool parse_cpu_render_quality(std::string_view text, CpuRenderQuality& quality) noexcept;
// The environment actually traced for `quality`.
[[nodiscard]] RenderEnvironment apply_cpu_render_quality(RenderEnvironment environment,
                                                         CpuRenderQuality quality) noexcept;

struct CpuPlayerRendererOptions {
    CpuRenderQuality quality{CpuRenderQuality::Fast};
    // Worker threads for the voxel trace; 0 = hardware concurrency (capped at 16). The image
    // is identical for any count; pin it anyway for reproducible timing.
    std::uint32_t threadCount{0};
    float exposure{1.0F};
    // Fill the background with a sky gradient derived from RenderEnvironment::skyColor.
    bool skyGradient{true};
};

// `blitter` may be null (headless: render + readback only). It must outlive the renderer.
[[nodiscard]] std::unique_ptr<IPlayerRenderer> make_cpu_player_renderer(
    IFrameBlitter* blitter, CpuPlayerRendererOptions options = {});

// Collects presented frames in memory (tests, headless runs).
class MemoryFrameBlitter final : public IFrameBlitter {
public:
    [[nodiscard]] bool blit(const Rgba8Image& frame, std::string* error) override;
    [[nodiscard]] const Rgba8Image& last() const noexcept { return last_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

private:
    Rgba8Image last_;
    std::uint64_t frames_{};
};

} // namespace dve::player
