#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dve/sprite_palette.hpp"
#include "dve/transform.hpp"

namespace dve {

struct SpriteVec2 {
    float x{};
    float y{};
};

enum class PixelScaleMode : std::uint8_t {
    IntegerFit,
    FractionalFit,
    Stretch,
    IntegerFill,
};

struct PixelPresentationConfig {
    std::uint32_t logicalWidth{320U};
    std::uint32_t logicalHeight{180U};
    PixelScaleMode scaleMode{PixelScaleMode::IntegerFit};
    bool allowFractionalDownscale{true};
    float pixelsPerWorldUnit{16.0F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct PixelPresentationLayout {
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    std::int32_t viewportX{};
    std::int32_t viewportY{};
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    float scaleX{1.0F};
    float scaleY{1.0F};
    bool integerScale{};
    bool cropped{};

    [[nodiscard]] bool contains_output_pixel(SpriteVec2 outputPixel) const noexcept;
};

[[nodiscard]] std::optional<PixelPresentationLayout> compute_pixel_presentation(
    const PixelPresentationConfig& config,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight,
    std::string* error = nullptr);

[[nodiscard]] std::optional<SpriteVec2> output_to_logical_pixel(
    const PixelPresentationConfig& config,
    const PixelPresentationLayout& layout,
    SpriteVec2 outputPixel,
    bool clampToViewport = false) noexcept;

[[nodiscard]] SpriteVec2 logical_to_output_pixel(
    const PixelPresentationLayout& layout,
    SpriteVec2 logicalPixel) noexcept;

// Platform pointer events are commonly reported in logical window coordinates while the render
// target uses drawable pixels on high-DPI displays. This helper performs that conversion before
// applying the letterbox/crop-aware logical-pixel mapping.
[[nodiscard]] std::optional<SpriteVec2> window_to_logical_pixel(
    const PixelPresentationConfig& config,
    const PixelPresentationLayout& layout,
    SpriteVec2 windowPixel,
    std::uint32_t windowWidth,
    std::uint32_t windowHeight,
    bool clampToViewport = false) noexcept;

enum class GameplayPlane2D : std::uint8_t { XY, XZ };

[[nodiscard]] Float3 snap_world_to_pixel(
    Float3 worldPosition,
    Float3 cameraOrigin,
    float pixelsPerWorldUnit,
    GameplayPlane2D plane = GameplayPlane2D::XY) noexcept;

using SpriteAssetId = std::uint64_t;
using SpriteOwnerId = std::uint64_t;
using SpriteFrameIndex = std::uint32_t;
using SpriteTrackId = std::uint64_t;
using SpriteAttachmentId = std::uint64_t;
constexpr SpriteAssetId kInvalidSpriteAssetId = 0U;
constexpr SpriteOwnerId kInvalidSpriteOwnerId = 0U;
constexpr SpriteFrameIndex kInvalidSpriteFrameIndex = 0xFFFFFFFFU;
constexpr SpriteTrackId kInvalidSpriteTrackId = 0U;
constexpr SpriteAttachmentId kInvalidSpriteAttachmentId = 0U;

enum class SpriteSampling : std::uint8_t { Nearest, Linear };
enum class SpriteLoopMode : std::uint8_t { Once, Loop, PingPong };
enum class SpriteBlendMode : std::uint8_t { Opaque, Alpha, Additive, Multiply };

enum class SpriteTrackInterpolation : std::uint8_t { Step, Linear };
enum class SpriteCombatRole : std::uint8_t {
    Hitbox,
    Hurtbox,
    Grab,
    Throw,
    Armor,
    Invulnerability,
    Trigger,
};
enum class SpriteCombatShape : std::uint8_t { Box, Circle, Capsule };

struct SpriteCombatVolume {
    std::string name;
    SpriteCombatRole role{SpriteCombatRole::Hitbox};
    SpriteCombatShape shape{SpriteCombatShape::Box};
    // Untrimmed frame-space pixels relative to the authored pivot. +X is right and +Y is up.
    SpriteVec2 centerPixels{};
    // Full width/height in pixels. Circle uses radiusPixels; capsule also retains full bounds.
    SpriteVec2 sizePixels{1.0F, 1.0F};
    float radiusPixels{0.5F};
    float rotationDegrees{};
    std::uint32_t attackId{};
    float damage{};
    SpriteVec2 knockbackPixelsPerSecond{};
    std::uint32_t hitStopTicks{};
    std::int32_t priority{};
    std::uint32_t flags{};
};

struct SpriteCombatWindow {
    std::size_t firstSequenceIndex{};
    std::size_t lastSequenceIndex{}; // inclusive
    SpriteCombatVolume volume;
    SpriteTrackId id{kInvalidSpriteTrackId};
};

struct SpriteSocketKey {
    std::string name;
    std::size_t sequenceIndex{};
    SpriteVec2 positionPixels{};
    float rotationDegrees{};
    SpriteVec2 scale{1.0F, 1.0F};
    SpriteTrackInterpolation interpolation{SpriteTrackInterpolation::Step};
    SpriteTrackId id{kInvalidSpriteTrackId};
};

enum class SpritePropertyType : std::uint8_t {
    Boolean,
    Integer,
    Float,
    Vec2,
    String,
    AssetReference,
};

struct SpritePropertyValue {
    SpritePropertyType type{SpritePropertyType::Boolean};
    bool booleanValue{};
    std::int64_t integerValue{};
    float floatValue{};
    SpriteVec2 vec2Value{};
    std::string stringValue;
};

struct SpritePropertyKey {
    std::string name;
    std::size_t sequenceIndex{};
    SpriteTrackInterpolation interpolation{SpriteTrackInterpolation::Step};
    SpritePropertyValue value;
    SpriteTrackId id{kInvalidSpriteTrackId};
};

struct SpriteRootMotionKey {
    std::size_t sequenceIndex{};
    SpriteVec2 deltaPixels{};
    float rotationDegrees{};
    SpriteTrackId id{kInvalidSpriteTrackId};
};

struct SpriteRectPixels {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct SpriteFrame {
    std::string name;
    SpriteRectPixels atlasRect{}; // top-left texture coordinates
    std::uint32_t sourceWidth{};
    std::uint32_t sourceHeight{};
    std::int32_t sourceOffsetX{}; // trimmed rectangle offset from untrimmed bottom-left
    std::int32_t sourceOffsetY{};
    SpriteVec2 pivotPixels{};     // pivot measured from the untrimmed bottom-left
    float durationSeconds{1.0F / 12.0F};
    std::string event;
};

struct SpriteClip {
    std::string name;
    SpriteLoopMode loopMode{SpriteLoopMode::Loop};
    float playbackRate{1.0F};
    std::vector<SpriteFrameIndex> frames;
    std::vector<SpriteCombatWindow> combatWindows;
    std::vector<SpriteSocketKey> socketKeys;
    std::vector<SpritePropertyKey> propertyKeys;
    std::vector<SpriteRootMotionKey> rootMotionKeys;

    SpriteClip() = default;
    SpriteClip(std::string clipName, SpriteLoopMode mode, float rate,
               std::vector<SpriteFrameIndex> clipFrames)
        : name(std::move(clipName)), loopMode(mode), playbackRate(rate),
          frames(std::move(clipFrames)) {}
};

struct SpriteAsset {
    std::string name;
    std::string textureAsset;
    std::uint32_t textureWidth{};
    std::uint32_t textureHeight{};
    float pixelsPerWorldUnit{16.0F};
    SpriteSampling sampling{SpriteSampling::Nearest};
    std::uint64_t materialId{};
    // Empty for ordinary RGBA sprites. Indexed sprites reference a versioned .dvepalette asset.
    std::string paletteAsset;
    std::uint32_t paletteBank{};
    std::vector<SpriteFrame> frames;
    std::vector<SpriteClip> clips;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

[[nodiscard]] std::uint64_t sprite_asset_content_hash(const SpriteAsset& asset) noexcept;
void assign_sprite_track_ids(SpriteAsset& asset) noexcept;
[[nodiscard]] SpriteTrackId next_sprite_track_id(const SpriteAsset& asset) noexcept;
[[nodiscard]] const SpriteClip* find_sprite_clip(
    const SpriteAsset& asset, std::string_view clipName) noexcept;

struct SpriteAssetReadResult {
    SpriteAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] bool write_dvesprite(
    const std::filesystem::path& path,
    const SpriteAsset& asset,
    std::string* error = nullptr);

[[nodiscard]] SpriteAssetReadResult read_dvesprite(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 16ULL * 1024ULL * 1024ULL);

struct SpriteSample {
    SpriteFrameIndex frame{kInvalidSpriteFrameIndex};
    // Playback sequence index. Ping-pong clips may expose the generated reverse leg here.
    std::size_t sequenceIndex{};
    // Authored timeline index in SpriteClip::frames. Metadata tracks use this coordinate.
    std::size_t authoredSequenceIndex{};
    float frameTimeSeconds{};
    float clipTimeSeconds{};
    float normalizedTime{};
    bool finished{};
};

struct SpriteSocketSample {
    SpriteTrackId id{kInvalidSpriteTrackId};
    std::string name;
    SpriteVec2 positionPixels{};
    float rotationDegrees{};
    SpriteVec2 scale{1.0F, 1.0F};
    RigidTransform worldTransform{};
};

struct SpriteCombatVolumeSample {
    SpriteTrackId id{kInvalidSpriteTrackId};
    SpriteCombatVolume volume;
    RigidTransform worldTransform{};
    SpriteVec2 sizeWorld{};
    float radiusWorld{};
};

struct SpritePropertySample {
    SpriteTrackId id{kInvalidSpriteTrackId};
    std::string name;
    SpritePropertyValue value;
};

struct SpriteTrackSample {
    SpriteSample animation;
    std::vector<SpriteCombatVolumeSample> combatVolumes;
    std::vector<SpriteSocketSample> sockets;
    std::vector<SpritePropertySample> properties;
    SpriteVec2 rootMotionDeltaPixels{};
    float rootMotionRotationDegrees{};
    SpriteTrackId rootMotionKeyId{kInvalidSpriteTrackId};
};

[[nodiscard]] std::optional<SpriteSample> sample_sprite_clip(
    const SpriteAsset& asset,
    std::string_view clipName,
    float timeSeconds) noexcept;

[[nodiscard]] std::optional<SpriteTrackSample> sample_sprite_clip_tracks(
    const SpriteAsset& asset,
    std::string_view clipName,
    float timeSeconds) noexcept;


struct SpriteRootMotionDelta {
    SpriteVec2 deltaPixels{};
    float rotationDegrees{};
    Float3 worldTranslation{};
    bool crossedLoopBoundary{};
    std::uint32_t enteredFrames{};
};

struct SpriteGameplayDebugPacket {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteAssetId asset{kInvalidSpriteAssetId};
    std::string clip;
    SpriteFrameIndex frame{kInvalidSpriteFrameIndex};
    std::size_t authoredSequenceIndex{};
    std::vector<SpriteCombatVolumeSample> combatVolumes;
    std::vector<SpriteSocketSample> sockets;
    std::vector<SpritePropertySample> properties;
    SpriteRootMotionDelta pendingRootMotion;
};

struct SpriteSocketAttachmentDesc {
    SpriteAttachmentId id{kInvalidSpriteAttachmentId};
    SpriteOwnerId parent{kInvalidSpriteOwnerId};
    std::string socket;
    RigidTransform localOffset{};
    bool inheritVisibility{true};
};

struct SpriteSocketAttachmentSample {
    SpriteAttachmentId id{kInvalidSpriteAttachmentId};
    SpriteOwnerId parent{kInvalidSpriteOwnerId};
    std::string socket;
    RigidTransform worldTransform{};
    bool visible{};
};

[[nodiscard]] std::optional<SpriteRootMotionDelta> accumulate_sprite_root_motion(
    const SpriteAsset& asset, std::string_view clipName, float fromTimeSeconds,
    float toTimeSeconds, GameplayPlane2D plane = GameplayPlane2D::XY,
    bool flipX = false, bool flipY = false) noexcept;

struct SpriteInstanceDesc {
    SpriteAssetId asset{kInvalidSpriteAssetId};
    std::string clip;
    RigidTransform transform{};
    GameplayPlane2D plane{GameplayPlane2D::XY};
    SpriteBlendMode blendMode{SpriteBlendMode::Alpha};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::int32_t sortingLayer{};
    std::int32_t orderInLayer{};
    float sortDepth{};
    float playbackSpeed{1.0F};
    std::optional<std::uint32_t> paletteBankOverride;
    std::vector<SpritePaletteSwap> paletteSwaps;
    std::uint64_t palettePhaseTicks{};
    bool flipX{};
    bool flipY{};
    bool pixelSnap{true};
    bool visible{true};
    bool playing{true};
};

struct SpriteFrameEvent {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteFrameIndex frame{kInvalidSpriteFrameIndex};
    std::string name;
};

enum class SpriteIntervalEventKind : std::uint8_t {
    FrameEvent,
    CombatActivated,
    CombatDeactivated,
    SocketKey,
    PropertyKey,
    RootMotionKey,
};

struct SpriteIntervalEvent {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteIntervalEventKind kind{SpriteIntervalEventKind::FrameEvent};
    // Absolute clip-clock time at which the boundary was crossed. The event order follows the
    // requested interval direction, so reverse queries return descending times.
    float timeSeconds{};
    bool reverse{};
    std::int64_t cycle{};
    std::size_t playbackSequenceIndex{};
    std::size_t authoredSequenceIndex{};
    SpriteFrameIndex frame{kInvalidSpriteFrameIndex};
    SpriteTrackId trackId{kInvalidSpriteTrackId};
    std::string name;
};

struct SpriteIntervalQueryOptions {
    bool frameEvents{true};
    bool combatBoundaries{true};
    bool socketKeys{true};
    bool propertyKeys{true};
    bool rootMotionKeys{true};
    std::size_t maximumEvents{4096U};
};

struct SpriteIntervalQueryResult {
    std::vector<SpriteIntervalEvent> events;
    bool reverse{};
    bool truncated{};
    bool crossedLoopBoundary{};
    std::uint64_t crossedFrameBoundaries{};
};

// Pure interval query used by runtime, rollback, resimulation, and headless validation. Forward
// queries use (from, to]; reverse queries use [to, from). The initial frame at time zero is not
// emitted unless the interval actually crosses a later loop boundary back into that frame.
[[nodiscard]] bool query_sprite_clip_interval_events(
    const SpriteAsset& asset, std::string_view clipName, float fromTimeSeconds,
    float toTimeSeconds, const SpriteIntervalQueryOptions& options,
    SpriteIntervalQueryResult& out, std::string* error = nullptr);

struct SpriteVertex {
    Float3 position{};
    SpriteVec2 uv{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct SpriteDrawItem {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    SpriteAssetId asset{kInvalidSpriteAssetId};
    SpriteFrameIndex frame{kInvalidSpriteFrameIndex};
    std::string textureAsset;
    std::uint64_t materialId{};
    std::uint32_t paletteBank{};
    std::string paletteAsset;
    std::uint64_t paletteStateHash{};
    std::size_t palettePacket{kInvalidSpritePalettePacket};
    SpriteSampling sampling{SpriteSampling::Nearest};
    SpriteBlendMode blendMode{SpriteBlendMode::Alpha};
    GameplayPlane2D plane{GameplayPlane2D::XY};
    std::int32_t sortingLayer{};
    std::int32_t orderInLayer{};
    float sortDepth{};
    std::array<SpriteVertex, 4> vertices{};
};

struct SpriteBatch {
    std::string textureAsset;
    std::uint64_t materialId{};
    std::uint32_t paletteBank{};
    SpriteSampling sampling{SpriteSampling::Nearest};
    SpriteBlendMode blendMode{SpriteBlendMode::Alpha};
    std::size_t firstItem{};
    std::size_t itemCount{};
    std::string paletteAsset;
    std::uint64_t paletteStateHash{};
    std::size_t palettePacket{kInvalidSpritePalettePacket};
};


struct SpriteBlendRenderOverride {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    std::optional<SpriteSample> source;
    std::optional<SpriteSample> destination;
    float sourceWeight{};
    float destinationWeight{1.0F};
};

struct SpriteRenderList {
    std::vector<SpriteDrawItem> items;
    std::vector<SpriteBatch> batches;
    std::vector<SpritePalettePacket> palettes;
};

enum class Visual2DKind : std::uint8_t { Sprite, FlatMesh, Model3D, Particle };

struct Visual2DSortEntry {
    SpriteOwnerId owner{kInvalidSpriteOwnerId};
    Visual2DKind kind{Visual2DKind::Sprite};
    std::int32_t sortingLayer{};
    std::int32_t orderInLayer{};
    float sortDepth{};
    std::size_t sourceIndex{};
};

[[nodiscard]] bool visual_2d_less(
    const Visual2DSortEntry& left,
    const Visual2DSortEntry& right) noexcept;

[[nodiscard]] std::vector<Visual2DSortEntry> sort_visual_2d_entries(
    std::span<const Visual2DSortEntry> entries);

class SpriteRuntime {
public:
    [[nodiscard]] bool register_asset(
        SpriteAssetId id, SpriteAsset asset, std::string* error = nullptr);
    [[nodiscard]] bool unregister_asset(SpriteAssetId id) noexcept;
    [[nodiscard]] const SpriteAsset* asset(SpriteAssetId id) const noexcept;

    [[nodiscard]] bool register_palette(
        std::string reference, SpritePaletteAsset palette, std::string* error = nullptr);
    [[nodiscard]] bool unregister_palette(std::string_view reference) noexcept;
    [[nodiscard]] const SpritePaletteAsset* palette(std::string_view reference) const noexcept;

    [[nodiscard]] bool bind(
        SpriteOwnerId owner, SpriteInstanceDesc desc, std::string* error = nullptr);
    [[nodiscard]] bool unbind(SpriteOwnerId owner) noexcept;
    [[nodiscard]] bool contains(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] bool set_clip(
        SpriteOwnerId owner, std::string_view clip, bool restart = true,
        std::string* error = nullptr);
    [[nodiscard]] bool set_transform(SpriteOwnerId owner, const RigidTransform& transform) noexcept;
    [[nodiscard]] bool set_visible(SpriteOwnerId owner, bool visible) noexcept;
    [[nodiscard]] bool set_flip(SpriteOwnerId owner, bool flipX, bool flipY) noexcept;
    [[nodiscard]] bool set_playing(SpriteOwnerId owner, bool playing) noexcept;
    [[nodiscard]] bool set_playback_speed(SpriteOwnerId owner, float speed) noexcept;
    [[nodiscard]] float time_seconds(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] const SpriteInstanceDesc* instance_desc(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] bool set_sorting(
        SpriteOwnerId owner, std::int32_t layer, std::int32_t order, float depth) noexcept;
    [[nodiscard]] bool set_palette_bank(
        SpriteOwnerId owner, std::optional<std::uint32_t> bank, std::string* error = nullptr);
    [[nodiscard]] bool set_palette_swaps(
        SpriteOwnerId owner, std::vector<SpritePaletteSwap> swaps, std::string* error = nullptr);
    [[nodiscard]] bool seek(SpriteOwnerId owner, float timeSeconds) noexcept;
    [[nodiscard]] bool bind_socket_attachment(SpriteSocketAttachmentDesc desc,
                                              std::string* error = nullptr);
    [[nodiscard]] bool unbind_socket_attachment(SpriteAttachmentId id) noexcept;
    [[nodiscard]] std::vector<SpriteSocketAttachmentSample> sample_socket_attachments() const;
    [[nodiscard]] std::optional<SpriteRootMotionDelta> pending_root_motion(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] std::optional<SpriteRootMotionDelta> consume_root_motion(SpriteOwnerId owner) noexcept;
    [[nodiscard]] bool set_pending_root_motion(
        SpriteOwnerId owner, const SpriteRootMotionDelta& delta) noexcept;
    [[nodiscard]] std::vector<SpriteGameplayDebugPacket> build_gameplay_debug_packets() const;

    // Exact palette ticks are the deterministic replay/network path. tick() converts wall-clock
    // seconds to this fixed 240 Hz clock while retaining the fractional remainder.
    void advance_palette_ticks(std::uint64_t ticks) noexcept;
    [[nodiscard]] std::uint64_t palette_clock_ticks() const noexcept { return paletteClockTicks_; }
    void tick(float deltaSeconds);
    [[nodiscard]] SpriteRenderList build_render_list(Float3 cameraOrigin = {}) const;
    [[nodiscard]] SpriteRenderList build_render_list(
        Float3 cameraOrigin, std::span<const SpriteBlendRenderOverride> overrides) const;
    [[nodiscard]] std::optional<SpriteSample> sample(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] std::optional<SpriteTrackSample> sample_tracks(SpriteOwnerId owner) const noexcept;
    [[nodiscard]] std::optional<SpriteTrackSample> sample_tracks(
        SpriteOwnerId owner, std::string_view clip, float timeSeconds) const noexcept;
    [[nodiscard]] std::span<const SpriteFrameEvent> events() const noexcept { return events_; }
    [[nodiscard]] std::span<const SpriteIntervalEvent> interval_events() const noexcept {
        return intervalEvents_;
    }
    [[nodiscard]] std::vector<SpriteOwnerId> owners() const;

private:
    struct Instance {
        SpriteInstanceDesc desc;
        float timeSeconds{};
        SpriteFrameIndex previousFrame{kInvalidSpriteFrameIndex};
        SpriteRootMotionDelta pendingRootMotion{};
    };

    std::map<SpriteAssetId, SpriteAsset> assets_;
    std::map<std::string, SpritePaletteAsset, std::less<>> palettes_;
    std::map<SpriteOwnerId, Instance> instances_;
    std::map<SpriteAttachmentId, SpriteSocketAttachmentDesc> attachments_;
    std::vector<SpriteFrameEvent> events_;
    std::vector<SpriteIntervalEvent> intervalEvents_;
    std::uint64_t paletteClockTicks_{};
    double paletteTickRemainder_{};
};

} // namespace dve
