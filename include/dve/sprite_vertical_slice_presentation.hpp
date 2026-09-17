#pragma once

#include "dve/chiptune_vertical_slice.hpp"
#include "dve/sprite_animation.hpp"
#include "dve/sprite_particle_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dve::gameplay {

struct SpriteVerticalSlicePresentationAssets {
    std::filesystem::path spriteAsset{"assets/sprites/v218_sun_route_cast.dvesprite"};
    std::filesystem::path paletteAsset{"assets/sprites/v218_sun_route_palette.dvepalette"};
    std::filesystem::path particleTexture{"assets/sprites/v218_sun_route_particles.png"};
};

struct SpriteVerticalSlicePresentationFrame {
    SpriteRenderList sprites;
    std::vector<TileRenderItem> tiles;
    SpriteParticleRenderPacket particles;
    Camera2D camera{};
    SpriteVec2 parallaxOffset{};
    std::string hudText;
    float screenShakePixels{};
    std::uint64_t frame{};
};

class SpriteVerticalSlicePresentation {
public:
    [[nodiscard]] bool initialize(const std::filesystem::path& projectRoot,
                                  SpriteVerticalSlicePresentationAssets assets = {},
                                  std::string* error = nullptr);
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    void sync(const ChiptuneVerticalSlice& slice, float deltaSeconds);
    [[nodiscard]] SpriteVerticalSlicePresentationFrame build_frame(
        const ChiptuneVerticalSlice& slice) const;

    [[nodiscard]] const SpriteRuntime& sprites() const noexcept { return sprites_; }
    [[nodiscard]] const SpriteParticleSystem& particle_system() const noexcept { return particles_; }
    [[nodiscard]] std::uint64_t presentation_hash(const ChiptuneVerticalSlice& slice) const noexcept;

private:
    [[nodiscard]] bool bind_instances(std::string* error);
    void update_clip(SpriteOwnerId owner, std::string_view clip);
    void process_new_events(const ChiptuneVerticalSlice& slice);
    void spawn_effect(std::string_view name, Float3 position);

    static constexpr SpriteOwnerId kPlayerOwner = 1001U;
    static constexpr SpriteOwnerId kEnemyOwner = 1002U;
    static constexpr SpriteOwnerId kProjectileOwner = 1003U;
    static constexpr SpriteOwnerId kCollectibleOwner = 1004U;
    static constexpr SpriteOwnerId kCheckpointOwner = 1005U;
    static constexpr SpriteOwnerId kHazardOwner = 1006U;

    std::filesystem::path projectRoot_;
    SpriteVerticalSlicePresentationAssets assetPaths_{};
    SpriteRuntime sprites_{};
    SpriteParticleSystem particles_{128U};
    VfxProgram burstProgram_{};
    SpriteParticleStyle burstStyle_{};
    std::size_t consumedEvents_{};
    float shakeSeconds_{};
    float shakeAmplitudePixels_{};
    std::uint64_t frameCounter_{};
    bool initialized_{};
};

} // namespace dve::gameplay
