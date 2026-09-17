#include "dve/sprite_vertical_slice_presentation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace dve::gameplay {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

[[nodiscard]] Float3 world_from_pixels(TileVec2 point, float z = 0.0F) noexcept {
    return {point.x / 16.0F, point.y / 16.0F, z};
}

[[nodiscard]] Float3 world_from_aabb(const TileAabb& bounds, float z = 0.0F) noexcept {
    return {bounds.center().x / 16.0F,
            bounds.min.y / 16.0F, z};
}

[[nodiscard]] VfxProgram make_burst_program() {
    VfxParticleGraphAsset graph;
    graph.name = "Sprite impact burst";
    graph.maximumParticles = 64U;
    graph.durationSeconds = 1.0F;
    graph.looping = false;
    graph.seed = 0x218U;
    VfxModule burst{1U, VfxModuleKind::SpawnBurst, VfxModulePhase::Spawn};
    burst.parameters[0] = 10.0F;
    VfxModule sphere{2U, VfxModuleKind::InitializeSphere, VfxModulePhase::Spawn};
    sphere.parameters[0] = 0.05F;
    VfxModule velocity{3U, VfxModuleKind::VelocityCone, VfxModulePhase::Spawn};
    velocity.parameters[0] = 1.2F;
    velocity.parameters[1] = 3.14159265F;
    VfxModule lifetime{4U, VfxModuleKind::LifetimeRange, VfxModulePhase::Spawn};
    lifetime.parameters[0] = 0.25F;
    lifetime.parameters[1] = 0.55F;
    VfxModule gravity{5U, VfxModuleKind::Gravity, VfxModulePhase::Update};
    gravity.parameters[1] = -3.0F;
    VfxModule color{6U, VfxModuleKind::ColorOverLife, VfxModulePhase::Update};
    color.parameters = {1.0F, 0.95F, 0.35F, 1.0F, 1.0F, 0.25F, 0.1F, 0.0F};
    VfxModule size{7U, VfxModuleKind::SizeOverLife, VfxModulePhase::Update};
    size.parameters[0] = 1.0F;
    size.parameters[1] = 0.1F;
    VfxModule renderer{8U, VfxModuleKind::BillboardRenderer, VfxModulePhase::Render};
    graph.modules = {burst, sphere, velocity, lifetime, gravity, color, size, renderer};
    const VfxCompileResult compiled = compile_vfx_particle_graph(graph);
    return compiled.program.value_or(VfxProgram{});
}

[[nodiscard]] std::uint64_t hash_float(std::uint64_t hash, float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        hash ^= static_cast<std::uint8_t>((bits >> shift) & 0xFFU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

bool SpriteVerticalSlicePresentation::initialize(
    const std::filesystem::path& projectRoot,
    SpriteVerticalSlicePresentationAssets assets, std::string* error) {
    projectRoot_ = projectRoot;
    assetPaths_ = std::move(assets);
    const SpritePaletteReadResult paletteRead = read_dvepalette(projectRoot_ / assetPaths_.paletteAsset);
    if (!paletteRead) return fail(error, paletteRead.error);
    if (!sprites_.register_palette(assetPaths_.paletteAsset.generic_string(), paletteRead.asset, error))
        return false;
    const SpriteAssetReadResult spriteRead = read_dvesprite(projectRoot_ / assetPaths_.spriteAsset);
    if (!spriteRead) return fail(error, spriteRead.error);
    if (!sprites_.register_asset(1U, spriteRead.asset, error)) return false;
    if (!bind_instances(error)) return false;
    burstProgram_ = make_burst_program();
    if (!burstProgram_.validate(error)) return false;
    burstStyle_.textureAsset = assetPaths_.particleTexture.generic_string();
    burstStyle_.atlasColumns = 4U;
    burstStyle_.atlasRows = 1U;
    burstStyle_.flipbookFramesPerSecond = 18.0F;
    burstStyle_.baseSizePixels = 5.0F;
    burstStyle_.velocityStretch = 0.08F;
    burstStyle_.rotationRateDegrees = 180.0F;
    burstStyle_.blendMode = SpriteBlendMode::Additive;
    if (!burstStyle_.validate(error)) return false;
    initialized_ = true;
    return true;
}

bool SpriteVerticalSlicePresentation::bind_instances(std::string* error) {
    const auto bind = [&](SpriteOwnerId owner, std::string clip, std::int32_t order) {
        SpriteInstanceDesc desc;
        desc.asset = 1U;
        desc.clip = std::move(clip);
        desc.sortingLayer = 2;
        desc.orderInLayer = order;
        desc.pixelSnap = true;
        return sprites_.bind(owner, std::move(desc), error);
    };
    if (!bind(kPlayerOwner, "player_idle", 30) ||
        !bind(kEnemyOwner, "enemy_walk", 25) ||
        !bind(kProjectileOwner, "projectile", 35) ||
        !bind(kCollectibleOwner, "token_spin", 20) ||
        !bind(kCheckpointOwner, "checkpoint_off", 15) ||
        !bind(kHazardOwner, "hazard", 10)) return false;
    if (!sprites_.set_palette_bank(kPlayerOwner, 0U, error) ||
        !sprites_.set_palette_bank(kEnemyOwner, 1U, error) ||
        !sprites_.set_palette_bank(kCollectibleOwner, 2U, error)) return false;
    return true;
}

void SpriteVerticalSlicePresentation::update_clip(SpriteOwnerId owner, std::string_view clip) {
    const SpriteInstanceDesc* desc = sprites_.instance_desc(owner);
    if (desc != nullptr && desc->clip != clip) (void)sprites_.set_clip(owner, clip, true, nullptr);
}

void SpriteVerticalSlicePresentation::sync(const ChiptuneVerticalSlice& slice, float deltaSeconds) {
    if (!initialized_ || !(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds)) return;
    const ChiptuneSliceState& state = slice.state();
    const bool moving = std::fabs(state.velocity.x) > 2.0F;
    update_clip(kPlayerOwner, !state.grounded ? "player_jump" : moving ? "player_run" : "player_idle");
    update_clip(kCheckpointOwner, state.checkpointActive ? "checkpoint_on" : "checkpoint_off");
    (void)sprites_.set_transform(kPlayerOwner,
        make_rigid_transform(world_from_aabb(state.player, 0.0F), {}));
    (void)sprites_.set_transform(kEnemyOwner,
        make_rigid_transform(world_from_aabb(state.enemy, 0.0F), {}));
    (void)sprites_.set_transform(kProjectileOwner,
        make_rigid_transform(world_from_pixels(state.projectile.position, 0.0F), {}));
    (void)sprites_.set_transform(kCollectibleOwner,
        make_rigid_transform(world_from_pixels(state.collectible, 0.0F), {}));
    (void)sprites_.set_transform(kCheckpointOwner,
        make_rigid_transform(world_from_pixels(state.checkpoint, 0.0F), {}));
    (void)sprites_.set_transform(kHazardOwner,
        make_rigid_transform({7.25F, 1.0F, 0.0F}, {}));
    (void)sprites_.set_visible(kEnemyOwner, state.enemyAlive);
    (void)sprites_.set_flip(kPlayerOwner, !state.facingRight, false);
    (void)sprites_.set_flip(kEnemyOwner, state.enemyDirection < 0.0F, false);
    (void)sprites_.set_visible(kProjectileOwner, state.projectile.active);
    (void)sprites_.set_visible(kCollectibleOwner, state.collectibleActive);
    sprites_.advance_palette_ticks(1U);
    sprites_.tick(deltaSeconds);
    process_new_events(slice);
    particles_.step(deltaSeconds);
    shakeSeconds_ = std::max(0.0F, shakeSeconds_ - deltaSeconds);
    ++frameCounter_;
}

void SpriteVerticalSlicePresentation::process_new_events(const ChiptuneVerticalSlice& slice) {
    const auto& events = slice.events();
    while (consumedEvents_ < events.size()) {
        const ChiptuneSliceEvent& event = events[consumedEvents_++];
        Float3 position = world_from_aabb(slice.state().player, 0.0F);
        switch (event.type) {
        case ChiptuneSliceEventType::Shot:
            position = world_from_pixels(slice.state().projectile.position, 0.0F);
            spawn_effect("shot", position);
            shakeAmplitudePixels_ = 1.5F; shakeSeconds_ = 0.08F;
            break;
        case ChiptuneSliceEventType::EnemyDefeated:
            position = world_from_aabb(slice.state().enemy, 0.0F);
            spawn_effect("enemy", position);
            shakeAmplitudePixels_ = 4.0F; shakeSeconds_ = 0.18F;
            break;
        case ChiptuneSliceEventType::Collected:
            position = world_from_pixels(slice.state().collectible, 0.0F);
            spawn_effect("token", position);
            break;
        case ChiptuneSliceEventType::CheckpointActivated:
            position = world_from_pixels(slice.state().checkpoint, 0.0F);
            spawn_effect("checkpoint", position);
            break;
        case ChiptuneSliceEventType::Damaged:
            spawn_effect("damage", position);
            shakeAmplitudePixels_ = 6.0F; shakeSeconds_ = 0.22F;
            break;
        case ChiptuneSliceEventType::Jumped:
            spawn_effect("jump", position);
            break;
        case ChiptuneSliceEventType::Finished:
            spawn_effect("complete", position);
            shakeAmplitudePixels_ = 3.0F; shakeSeconds_ = 0.35F;
            break;
        default: break;
        }
    }
}

void SpriteVerticalSlicePresentation::spawn_effect(std::string_view name, Float3 position) {
    SpriteParticleStyle style = burstStyle_;
    if (name == "damage") style.baseSizePixels = 7.0F;
    if (name == "checkpoint" || name == "complete") style.flipbookFramesPerSecond = 24.0F;
    (void)particles_.spawn(burstProgram_, std::move(style), position, nullptr);
}

SpriteVerticalSlicePresentationFrame SpriteVerticalSlicePresentation::build_frame(
    const ChiptuneVerticalSlice& slice) const {
    SpriteVerticalSlicePresentationFrame frame;
    const ChiptuneSliceState& state = slice.state();
    frame.camera = state.camera;
    const float shake = shakeSeconds_ > 0.0F
        ? shakeAmplitudePixels_ * std::sin(static_cast<float>(frameCounter_) * 2.39996323F)
        : 0.0F;
    frame.screenShakePixels = shake;
    const Float3 cameraOrigin{state.camera.center.x / 16.0F + shake / 16.0F,
                              state.camera.center.y / 16.0F, 0.0F};
    frame.sprites = sprites_.build_render_list(cameraOrigin);
    frame.tiles = build_tile_render_list(slice.map(), state.camera.view_bounds(), state.frame);
    frame.particles = particles_.build_render_packet(cameraOrigin, GameplayPlane2D::XY);
    frame.parallaxOffset = {state.camera.center.x * 0.2F, state.camera.center.y * 0.1F};
    frame.hudText = slice.hud_text();
    frame.frame = state.frame;
    return frame;
}

std::uint64_t SpriteVerticalSlicePresentation::presentation_hash(
    const ChiptuneVerticalSlice& slice) const noexcept {
    const SpriteVerticalSlicePresentationFrame frame = build_frame(slice);
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            hash ^= static_cast<std::uint8_t>((value >> shift) & 0xFFU);
            hash *= 1099511628211ULL;
        }
    };
    mix(frame.frame);
    mix(frame.sprites.items.size());
    mix(frame.tiles.size());
    mix(frame.particles.renderList.items.size());
    hash = hash_float(hash, frame.camera.center.x);
    hash = hash_float(hash, frame.camera.center.y);
    hash = hash_float(hash, frame.screenShakePixels);
    return hash;
}

} // namespace dve::gameplay
