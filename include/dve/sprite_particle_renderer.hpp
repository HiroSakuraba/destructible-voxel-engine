#pragma once

#include "dve/sprite2d.hpp"
#include "dve/vfx_particle_graph.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dve {

using SpriteParticleEmitterId = std::uint64_t;

struct SpriteParticleStyle {
    std::string textureAsset;
    std::uint32_t atlasColumns{1U};
    std::uint32_t atlasRows{1U};
    float flipbookFramesPerSecond{12.0F};
    float pixelsPerWorldUnit{16.0F};
    float baseSizePixels{8.0F};
    float velocityStretch{0.0F};
    float rotationRateDegrees{0.0F};
    SpriteBlendMode blendMode{SpriteBlendMode::Alpha};
    SpriteSampling sampling{SpriteSampling::Nearest};
    std::int32_t sortingLayer{5};
    std::int32_t orderInLayer{};
    bool sortBackToFront{true};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct SpriteParticleTrailPoint {
    Float3 position{};
    float age{};
};

struct SpriteParticleTrailDesc {
    SpriteParticleEmitterId owner{};
    std::string textureAsset;
    std::vector<SpriteParticleTrailPoint> points;
    float widthPixels{4.0F};
    float pixelsPerWorldUnit{16.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::int32_t sortingLayer{4};
    std::int32_t orderInLayer{};
};

struct SpriteParticleBeamDesc {
    SpriteParticleEmitterId owner{};
    std::string textureAsset;
    Float3 start{};
    Float3 end{};
    float widthPixels{3.0F};
    float pixelsPerWorldUnit{16.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::int32_t sortingLayer{4};
    std::int32_t orderInLayer{};
};

struct SpriteParticleRenderStats {
    std::uint64_t emitters{};
    std::uint64_t particles{};
    std::uint64_t trailSegments{};
    std::uint64_t beams{};
    std::uint64_t batches{};
};

struct SpriteParticleRenderPacket {
    SpriteRenderList renderList;
    SpriteParticleRenderStats stats;
};

struct SpriteParticleReferenceImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> rgba8;
    std::uint64_t contentHash{};
};

class SpriteParticleSystem {
public:
    explicit SpriteParticleSystem(std::size_t maximumEmitters = 256U) : maximumEmitters_(maximumEmitters) {}

    [[nodiscard]] std::optional<SpriteParticleEmitterId> spawn(
        VfxProgram program, SpriteParticleStyle style, Float3 origin,
        std::string* error = nullptr);
    [[nodiscard]] bool remove(SpriteParticleEmitterId id) noexcept;
    [[nodiscard]] bool set_origin(SpriteParticleEmitterId id, Float3 origin) noexcept;
    [[nodiscard]] bool set_active(SpriteParticleEmitterId id, bool active) noexcept;
    [[nodiscard]] bool contains(SpriteParticleEmitterId id) const noexcept;

    void step(float deltaSeconds);
    [[nodiscard]] SpriteParticleRenderPacket build_render_packet(
        Float3 cameraOrigin = {}, GameplayPlane2D plane = GameplayPlane2D::XY) const;

    void set_trails(std::vector<SpriteParticleTrailDesc> trails) { trails_ = std::move(trails); }
    void set_beams(std::vector<SpriteParticleBeamDesc> beams) { beams_ = std::move(beams); }
    void clear_trails() noexcept { trails_.clear(); }
    void clear_beams() noexcept { beams_.clear(); }

    [[nodiscard]] std::vector<SpriteParticleEmitterId> spawn_from_events(
        std::span<const SpriteIntervalEvent> events,
        const std::map<std::string, std::pair<VfxProgram, SpriteParticleStyle>, std::less<>>& registry,
        const std::map<SpriteOwnerId, Float3>& ownerPositions,
        std::string* error = nullptr);
    [[nodiscard]] std::vector<SpriteParticleEmitterId> spawn_from_sockets(
        std::span<const SpriteSocketAttachmentSample> sockets,
        std::string_view socketName, const VfxProgram& program, const SpriteParticleStyle& style,
        std::string* error = nullptr);

private:
    struct Emitter {
        SpriteParticleEmitterId id{};
        VfxCpuRuntime runtime;
        SpriteParticleStyle style;
        Float3 origin{};
        bool active{true};
        explicit Emitter(SpriteParticleEmitterId value, VfxProgram program,
                         SpriteParticleStyle styleValue, Float3 originValue)
            : id(value), runtime(std::move(program)), style(std::move(styleValue)), origin(originValue) {}
    };

    std::map<SpriteParticleEmitterId, Emitter> emitters_;
    std::vector<SpriteParticleTrailDesc> trails_;
    std::vector<SpriteParticleBeamDesc> beams_;
    std::size_t maximumEmitters_{256U};
    SpriteParticleEmitterId nextId_{1U};
};

[[nodiscard]] SpriteParticleReferenceImage rasterize_sprite_particles_reference(
    const SpriteParticleRenderPacket& packet, std::uint32_t width, std::uint32_t height,
    float pixelsPerWorldUnit = 16.0F, Float3 cameraOrigin = {});

} // namespace dve
