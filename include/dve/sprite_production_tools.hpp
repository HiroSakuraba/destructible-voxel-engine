#pragma once

#include "dve/sprite_authoring.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve::editor {

enum class SpriteTrackPasteMode : std::uint8_t { Merge, ReplaceDestinationRange };
enum class SpriteTrackHandleKind : std::uint8_t {
    Move,
    ResizeLeft,
    ResizeRight,
    ResizeTop,
    ResizeBottom,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeBottomRight,
    SocketScale,
};
enum class SpriteTrackDiagnosticSeverity : std::uint8_t { Info, Warning, Error };
enum class SpriteTrackPresetKind : std::uint8_t {
    MeleeHitbox,
    CharacterHurtbox,
    ProjectileSpawnRegion,
    WeaponSocket,
    FootstepEvent,
    HorizontalRootMotion,
    BooleanProperty,
};
enum class SpriteTrackNumericField : std::uint8_t {
    AttackId,
    Damage,
    KnockbackX,
    KnockbackY,
    HitStopTicks,
    Priority,
    PositionX,
    PositionY,
    Rotation,
    ScaleX,
    ScaleY,
    RootDeltaX,
    RootDeltaY,
    PropertyBoolean,
    PropertyInteger,
    PropertyFloat,
    PropertyVec2X,
    PropertyVec2Y,
};

struct SpriteTrackClipboard {
    std::size_t sourceFirst{};
    std::size_t sourceLast{};
    std::vector<SpriteCombatWindow> combatWindows;
    std::vector<SpriteSocketKey> socketKeys;
    std::vector<SpritePropertyKey> propertyKeys;
    std::vector<SpriteRootMotionKey> rootMotionKeys;
    std::vector<std::pair<std::size_t, std::string>> frameEvents;

    [[nodiscard]] bool empty() const noexcept {
        return combatWindows.empty() && socketKeys.empty() && propertyKeys.empty() &&
            rootMotionKeys.empty() && frameEvents.empty();
    }
    [[nodiscard]] std::size_t length() const noexcept {
        return sourceLast >= sourceFirst ? sourceLast - sourceFirst + 1U : 0U;
    }
};

struct SpriteTrackSearchMatch {
    std::string clip;
    std::size_t sequenceIndex{};
    SpriteTrackItemKind kind{SpriteTrackItemKind::PropertyKey};
    SpriteTrackId id{kInvalidSpriteTrackId};
    std::string field;
    std::string text;
};

struct SpriteTrackDiagnostic {
    SpriteTrackDiagnosticSeverity severity{SpriteTrackDiagnosticSeverity::Info};
    std::string clip;
    std::size_t sequenceIndex{};
    SpriteTrackItemKind kind{SpriteTrackItemKind::PropertyKey};
    SpriteTrackId id{kInvalidSpriteTrackId};
    std::string field;
    std::string message;
};

struct SpriteTrackOverlay {
    SpriteTrackItemKind kind{SpriteTrackItemKind::CombatWindow};
    SpriteTrackId id{kInvalidSpriteTrackId};
    std::size_t sequenceIndex{};
    int relativeFrame{};
    float opacity{1.0F};
    SpriteCombatShape shape{SpriteCombatShape::Box};
    SpriteVec2 centerPixels{};
    SpriteVec2 sizePixels{1.0F, 1.0F};
    float radiusPixels{0.5F};
    float rotationDegrees{};
    std::string label;
};

struct SpriteTrackNumericDescriptor {
    SpriteTrackNumericField field{SpriteTrackNumericField::Damage};
    std::string label;
    float step{1.0F};
    std::string valueText;
};

class SpriteTrackProductionEditor {
public:
    explicit SpriteTrackProductionEditor(SpriteAuthoringSession& session) noexcept : session_(&session) {}

    [[nodiscard]] bool copy_range(std::string_view clipName, std::size_t firstSequenceIndex,
                                  std::size_t lastSequenceIndex, SpriteTrackClipboard& out,
                                  std::string* error = nullptr) const;
    [[nodiscard]] bool paste_range(std::string_view clipName, std::size_t destinationFirst,
                                   const SpriteTrackClipboard& clipboard,
                                   SpriteTrackPasteMode mode = SpriteTrackPasteMode::Merge,
                                   std::string* error = nullptr);
    [[nodiscard]] bool duplicate_range(std::string_view clipName, std::size_t firstSequenceIndex,
                                       std::size_t lastSequenceIndex, std::ptrdiff_t offset,
                                       std::string* error = nullptr);
    [[nodiscard]] bool mirror_range_x(std::string_view clipName, std::size_t firstSequenceIndex,
                                      std::size_t lastSequenceIndex,
                                      std::string* error = nullptr);
    [[nodiscard]] bool bulk_retime(std::string_view clipName, std::size_t sourceFirst,
                                   std::size_t sourceLast, std::size_t destinationFirst,
                                   std::size_t destinationLast,
                                   std::string* error = nullptr);
    [[nodiscard]] bool apply_preset(std::string_view clipName, std::size_t sequenceIndex,
                                    SpriteTrackPresetKind preset,
                                    std::string* error = nullptr);
    [[nodiscard]] bool drag_item(std::string_view clipName, SpriteTrackSelection selection,
                                 SpriteTrackHandleKind handle, SpriteVec2 deltaPixels,
                                 std::string* error = nullptr);
    [[nodiscard]] bool adjust_numeric(std::string_view clipName, SpriteTrackSelection selection,
                                      SpriteTrackNumericField field, float delta,
                                      std::string* error = nullptr);

    [[nodiscard]] std::vector<SpriteTrackSearchMatch> search(
        std::string_view query, std::optional<std::string_view> clipName = std::nullopt) const;
    [[nodiscard]] std::vector<SpriteTrackDiagnostic> diagnostics(
        std::optional<std::string_view> clipName = std::nullopt) const;
    [[nodiscard]] std::vector<SpriteTrackOverlay> onion_overlays(
        std::string_view clipName, std::size_t sequenceIndex, bool previous, bool next,
        float opacity = 0.35F) const;
    [[nodiscard]] std::vector<SpriteTrackNumericDescriptor> numeric_descriptors(
        std::string_view clipName, SpriteTrackSelection selection) const;

private:
    SpriteAuthoringSession* session_{};
};

} // namespace dve::editor
