#include "dve/sprite_production_tools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace dve::editor {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

[[nodiscard]] SpriteClip* find_clip(SpriteAsset& asset, std::string_view name) noexcept {
    const auto found = std::find_if(asset.clips.begin(), asset.clips.end(),
        [name](const SpriteClip& clip) { return clip.name == name; });
    return found == asset.clips.end() ? nullptr : &*found;
}

[[nodiscard]] const SpriteClip* find_clip(const SpriteAsset& asset, std::string_view name) noexcept {
    return find_sprite_clip(asset, name);
}

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

[[nodiscard]] std::size_t remap_index(std::size_t value, std::size_t sourceFirst,
                                      std::size_t sourceLast, std::size_t destinationFirst,
                                      std::size_t destinationLast) noexcept {
    if (sourceLast <= sourceFirst) return destinationFirst;
    const double t = static_cast<double>(value - sourceFirst) /
        static_cast<double>(sourceLast - sourceFirst);
    const double mapped = static_cast<double>(destinationFirst) +
        t * static_cast<double>(destinationLast - destinationFirst);
    return static_cast<std::size_t>(std::llround(mapped));
}

[[nodiscard]] std::string format_float(float value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

[[nodiscard]] bool sequence_in_range(std::size_t value, std::size_t first,
                                     std::size_t last) noexcept {
    return value >= first && value <= last;
}

[[nodiscard]] bool window_overlaps(const SpriteCombatWindow& window, std::size_t first,
                                   std::size_t last) noexcept {
    return window.lastSequenceIndex >= first && window.firstSequenceIndex <= last;
}

} // namespace

bool SpriteTrackProductionEditor::copy_range(
    std::string_view clipName, std::size_t first, std::size_t last,
    SpriteTrackClipboard& out, std::string* error) const {
    if (session_ == nullptr) return fail(error, "sprite track editor is not bound");
    const SpriteClip* clip = find_clip(session_->asset(), clipName);
    if (clip == nullptr) return fail(error, "sprite clip does not exist");
    if (first > last || last >= clip->frames.size())
        return fail(error, "sprite track copy range is outside the clip");
    out = {};
    out.sourceFirst = first;
    out.sourceLast = last;
    for (const SpriteCombatWindow& item : clip->combatWindows) {
        if (!window_overlaps(item, first, last)) continue;
        SpriteCombatWindow copy = item;
        copy.firstSequenceIndex = std::max(copy.firstSequenceIndex, first) - first;
        copy.lastSequenceIndex = std::min(copy.lastSequenceIndex, last) - first;
        copy.id = kInvalidSpriteTrackId;
        out.combatWindows.push_back(std::move(copy));
    }
    for (const SpriteSocketKey& item : clip->socketKeys) {
        if (!sequence_in_range(item.sequenceIndex, first, last)) continue;
        SpriteSocketKey copy = item;
        copy.sequenceIndex -= first;
        copy.id = kInvalidSpriteTrackId;
        out.socketKeys.push_back(std::move(copy));
    }
    for (const SpritePropertyKey& item : clip->propertyKeys) {
        if (!sequence_in_range(item.sequenceIndex, first, last)) continue;
        SpritePropertyKey copy = item;
        copy.sequenceIndex -= first;
        copy.id = kInvalidSpriteTrackId;
        out.propertyKeys.push_back(std::move(copy));
    }
    for (const SpriteRootMotionKey& item : clip->rootMotionKeys) {
        if (!sequence_in_range(item.sequenceIndex, first, last)) continue;
        SpriteRootMotionKey copy = item;
        copy.sequenceIndex -= first;
        copy.id = kInvalidSpriteTrackId;
        out.rootMotionKeys.push_back(copy);
    }
    for (std::size_t sequence = first; sequence <= last; ++sequence) {
        const SpriteFrame& frame = session_->asset().frames[clip->frames[sequence]];
        if (!frame.event.empty()) out.frameEvents.emplace_back(sequence - first, frame.event);
    }
    return true;
}

bool SpriteTrackProductionEditor::paste_range(
    std::string_view clipName, std::size_t destinationFirst,
    const SpriteTrackClipboard& clipboard, SpriteTrackPasteMode mode, std::string* error) {
    if (session_ == nullptr) return fail(error, "sprite track editor is not bound");
    if (clipboard.empty() || clipboard.length() == 0U)
        return fail(error, "sprite track clipboard is empty");

    SpriteAsset replacementAsset = session_->asset();
    SpriteClip* replacement = find_clip(replacementAsset, clipName);
    if (replacement == nullptr) return fail(error, "sprite clip does not exist");
    if (destinationFirst >= replacement->frames.size() ||
        clipboard.length() > replacement->frames.size() - destinationFirst)
        return fail(error, "sprite track paste range is outside the clip");

    const std::size_t destinationLast = destinationFirst + clipboard.length() - 1U;
    if (mode == SpriteTrackPasteMode::ReplaceDestinationRange) {
        std::erase_if(replacement->combatWindows, [destinationFirst, destinationLast](const auto& item) {
            return window_overlaps(item, destinationFirst, destinationLast);
        });
        std::erase_if(replacement->socketKeys, [destinationFirst, destinationLast](const auto& item) {
            return sequence_in_range(item.sequenceIndex, destinationFirst, destinationLast);
        });
        std::erase_if(replacement->propertyKeys, [destinationFirst, destinationLast](const auto& item) {
            return sequence_in_range(item.sequenceIndex, destinationFirst, destinationLast);
        });
        std::erase_if(replacement->rootMotionKeys, [destinationFirst, destinationLast](const auto& item) {
            return sequence_in_range(item.sequenceIndex, destinationFirst, destinationLast);
        });
        for (std::size_t sequence = destinationFirst; sequence <= destinationLast; ++sequence)
            replacementAsset.frames[replacement->frames[sequence]].event.clear();
    }

    assign_sprite_track_ids(replacementAsset);
    SpriteTrackId id = next_sprite_track_id(replacementAsset);
    for (SpriteCombatWindow item : clipboard.combatWindows) {
        item.firstSequenceIndex += destinationFirst;
        item.lastSequenceIndex += destinationFirst;
        item.id = id++;
        replacement->combatWindows.push_back(std::move(item));
    }
    for (SpriteSocketKey item : clipboard.socketKeys) {
        item.sequenceIndex += destinationFirst;
        item.id = id++;
        replacement->socketKeys.push_back(std::move(item));
    }
    for (SpritePropertyKey item : clipboard.propertyKeys) {
        item.sequenceIndex += destinationFirst;
        item.id = id++;
        replacement->propertyKeys.push_back(std::move(item));
    }
    for (SpriteRootMotionKey item : clipboard.rootMotionKeys) {
        item.sequenceIndex += destinationFirst;
        item.id = id++;
        replacement->rootMotionKeys.push_back(item);
    }
    for (const auto& [relative, event] : clipboard.frameEvents) {
        const std::size_t sequence = destinationFirst + relative;
        replacementAsset.frames[replacement->frames[sequence]].event = event;
    }
    return session_->replace_asset(std::move(replacementAsset), "Paste sprite track range", error);
}

bool SpriteTrackProductionEditor::duplicate_range(
    std::string_view clipName, std::size_t first, std::size_t last,
    std::ptrdiff_t offset, std::string* error) {
    SpriteTrackClipboard clipboard;
    if (!copy_range(clipName, first, last, clipboard, error)) return false;
    if (offset < 0 && static_cast<std::size_t>(-offset) > first)
        return fail(error, "duplicated sprite track range starts before the clip");
    const std::ptrdiff_t destination = static_cast<std::ptrdiff_t>(first) + offset;
    if (destination < 0) return fail(error, "duplicated sprite track range starts before the clip");
    return paste_range(clipName, static_cast<std::size_t>(destination), clipboard,
                       SpriteTrackPasteMode::Merge, error);
}

bool SpriteTrackProductionEditor::mirror_range_x(
    std::string_view clipName, std::size_t first, std::size_t last, std::string* error) {
    if (session_ == nullptr) return fail(error, "sprite track editor is not bound");
    const SpriteClip* source = find_clip(session_->asset(), clipName);
    if (source == nullptr) return fail(error, "sprite clip does not exist");
    if (first > last || last >= source->frames.size())
        return fail(error, "sprite track mirror range is outside the clip");
    SpriteClip replacement = *source;
    for (SpriteCombatWindow& item : replacement.combatWindows) {
        if (!window_overlaps(item, first, last)) continue;
        item.volume.centerPixels.x = -item.volume.centerPixels.x;
        item.volume.rotationDegrees = -item.volume.rotationDegrees;
        item.volume.knockbackPixelsPerSecond.x = -item.volume.knockbackPixelsPerSecond.x;
    }
    for (SpriteSocketKey& item : replacement.socketKeys) {
        if (!sequence_in_range(item.sequenceIndex, first, last)) continue;
        item.positionPixels.x = -item.positionPixels.x;
        item.rotationDegrees = -item.rotationDegrees;
    }
    for (SpritePropertyKey& item : replacement.propertyKeys) {
        if (sequence_in_range(item.sequenceIndex, first, last) &&
            item.value.type == SpritePropertyType::Vec2)
            item.value.vec2Value.x = -item.value.vec2Value.x;
    }
    for (SpriteRootMotionKey& item : replacement.rootMotionKeys) {
        if (!sequence_in_range(item.sequenceIndex, first, last)) continue;
        item.deltaPixels.x = -item.deltaPixels.x;
        item.rotationDegrees = -item.rotationDegrees;
    }
    return session_->update_clip(clipName, std::move(replacement), error);
}

bool SpriteTrackProductionEditor::bulk_retime(
    std::string_view clipName, std::size_t sourceFirst, std::size_t sourceLast,
    std::size_t destinationFirst, std::size_t destinationLast, std::string* error) {
    if (session_ == nullptr) return fail(error, "sprite track editor is not bound");
    const SpriteClip* source = find_clip(session_->asset(), clipName);
    if (source == nullptr) return fail(error, "sprite clip does not exist");
    if (sourceFirst > sourceLast || destinationFirst > destinationLast ||
        sourceLast >= source->frames.size() || destinationLast >= source->frames.size())
        return fail(error, "sprite track retime range is outside the clip");
    SpriteClip replacement = *source;
    for (SpriteCombatWindow& item : replacement.combatWindows) {
        if (!window_overlaps(item, sourceFirst, sourceLast)) continue;
        item.firstSequenceIndex = remap_index(std::clamp(item.firstSequenceIndex, sourceFirst, sourceLast),
            sourceFirst, sourceLast, destinationFirst, destinationLast);
        item.lastSequenceIndex = remap_index(std::clamp(item.lastSequenceIndex, sourceFirst, sourceLast),
            sourceFirst, sourceLast, destinationFirst, destinationLast);
        if (item.firstSequenceIndex > item.lastSequenceIndex)
            std::swap(item.firstSequenceIndex, item.lastSequenceIndex);
    }
    for (SpriteSocketKey& item : replacement.socketKeys)
        if (sequence_in_range(item.sequenceIndex, sourceFirst, sourceLast))
            item.sequenceIndex = remap_index(item.sequenceIndex, sourceFirst, sourceLast,
                                             destinationFirst, destinationLast);
    for (SpritePropertyKey& item : replacement.propertyKeys)
        if (sequence_in_range(item.sequenceIndex, sourceFirst, sourceLast))
            item.sequenceIndex = remap_index(item.sequenceIndex, sourceFirst, sourceLast,
                                             destinationFirst, destinationLast);
    for (SpriteRootMotionKey& item : replacement.rootMotionKeys)
        if (sequence_in_range(item.sequenceIndex, sourceFirst, sourceLast))
            item.sequenceIndex = remap_index(item.sequenceIndex, sourceFirst, sourceLast,
                                             destinationFirst, destinationLast);
    return session_->update_clip(clipName, std::move(replacement), error);
}

bool SpriteTrackProductionEditor::apply_preset(
    std::string_view clipName, std::size_t sequenceIndex,
    SpriteTrackPresetKind preset, std::string* error) {
    if (session_ == nullptr) return fail(error, "sprite track editor is not bound");
    const SpriteClip* clip = find_clip(session_->asset(), clipName);
    if (clip == nullptr || sequenceIndex >= clip->frames.size())
        return fail(error, "sprite track preset frame is outside the clip");
    switch (preset) {
    case SpriteTrackPresetKind::MeleeHitbox: {
        SpriteCombatWindow item;
        item.firstSequenceIndex = sequenceIndex;
        item.lastSequenceIndex = sequenceIndex;
        item.volume.name = "melee_hit";
        item.volume.role = SpriteCombatRole::Hitbox;
        item.volume.sizePixels = {12.0F, 10.0F};
        item.volume.centerPixels = {8.0F, 6.0F};
        item.volume.attackId = 1U;
        item.volume.damage = 10.0F;
        item.volume.knockbackPixelsPerSecond = {96.0F, 48.0F};
        item.volume.hitStopTicks = 4U;
        return session_->add_combat_window(clipName, std::move(item), error);
    }
    case SpriteTrackPresetKind::CharacterHurtbox: {
        SpriteCombatWindow item;
        item.firstSequenceIndex = sequenceIndex;
        item.lastSequenceIndex = sequenceIndex;
        item.volume.name = "body_hurt";
        item.volume.role = SpriteCombatRole::Hurtbox;
        item.volume.sizePixels = {12.0F, 22.0F};
        item.volume.centerPixels = {0.0F, 11.0F};
        return session_->add_combat_window(clipName, std::move(item), error);
    }
    case SpriteTrackPresetKind::ProjectileSpawnRegion: {
        SpriteCombatWindow item;
        item.firstSequenceIndex = sequenceIndex;
        item.lastSequenceIndex = sequenceIndex;
        item.volume.name = "projectile_spawn";
        item.volume.role = SpriteCombatRole::Trigger;
        item.volume.sizePixels = {4.0F, 4.0F};
        item.volume.centerPixels = {10.0F, 8.0F};
        item.volume.flags = 1U;
        return session_->add_combat_window(clipName, std::move(item), error);
    }
    case SpriteTrackPresetKind::WeaponSocket: {
        SpriteSocketKey key;
        key.name = "weapon";
        key.sequenceIndex = sequenceIndex;
        key.positionPixels = {8.0F, 8.0F};
        return session_->upsert_socket_key(clipName, std::move(key), error);
    }
    case SpriteTrackPresetKind::FootstepEvent: {
        SpriteFrame frame = session_->asset().frames[clip->frames[sequenceIndex]];
        frame.event = "footstep";
        return session_->update_frame(clip->frames[sequenceIndex], std::move(frame), error);
    }
    case SpriteTrackPresetKind::HorizontalRootMotion: {
        SpriteRootMotionKey key;
        key.sequenceIndex = sequenceIndex;
        key.deltaPixels = {2.0F, 0.0F};
        return session_->set_root_motion_key(clipName, key, error);
    }
    case SpriteTrackPresetKind::BooleanProperty: {
        SpritePropertyKey key;
        key.name = "enabled";
        key.sequenceIndex = sequenceIndex;
        key.value.type = SpritePropertyType::Boolean;
        key.value.booleanValue = true;
        return session_->upsert_property_key(clipName, std::move(key), error);
    }
    }
    return fail(error, "sprite track preset is invalid");
}

bool SpriteTrackProductionEditor::drag_item(
    std::string_view clipName, SpriteTrackSelection selection,
    SpriteTrackHandleKind handle, SpriteVec2 delta, std::string* error) {
    if (session_ == nullptr) return fail(error, "sprite track editor is not bound");
    const SpriteClip* clip = find_clip(session_->asset(), clipName);
    if (clip == nullptr) return fail(error, "sprite clip does not exist");
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y))
        return fail(error, "sprite track drag delta is invalid");
    if (selection.kind == SpriteTrackItemKind::CombatWindow) {
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [selection](const auto& item) { return item.id == selection.id; });
        if (found == clip->combatWindows.end()) return fail(error, "sprite combat window does not exist");
        SpriteCombatWindow item = *found;
        if (handle == SpriteTrackHandleKind::Move) {
            item.volume.centerPixels.x += delta.x;
            item.volume.centerPixels.y += delta.y;
        } else {
            const bool left = handle == SpriteTrackHandleKind::ResizeLeft ||
                handle == SpriteTrackHandleKind::ResizeTopLeft ||
                handle == SpriteTrackHandleKind::ResizeBottomLeft;
            const bool right = handle == SpriteTrackHandleKind::ResizeRight ||
                handle == SpriteTrackHandleKind::ResizeTopRight ||
                handle == SpriteTrackHandleKind::ResizeBottomRight;
            const bool top = handle == SpriteTrackHandleKind::ResizeTop ||
                handle == SpriteTrackHandleKind::ResizeTopLeft ||
                handle == SpriteTrackHandleKind::ResizeTopRight;
            const bool bottom = handle == SpriteTrackHandleKind::ResizeBottom ||
                handle == SpriteTrackHandleKind::ResizeBottomLeft ||
                handle == SpriteTrackHandleKind::ResizeBottomRight;
            if (left || right) {
                item.volume.sizePixels.x = std::max(1.0F, item.volume.sizePixels.x +
                    (right ? delta.x : -delta.x));
                item.volume.centerPixels.x += delta.x * 0.5F;
            }
            if (top || bottom) {
                item.volume.sizePixels.y = std::max(1.0F, item.volume.sizePixels.y +
                    (top ? delta.y : -delta.y));
                item.volume.centerPixels.y += delta.y * 0.5F;
            }
            item.volume.radiusPixels = std::max(0.5F, std::min(item.volume.sizePixels.x,
                item.volume.sizePixels.y) * 0.5F);
        }
        return session_->update_combat_window_by_id(clipName, selection.id, std::move(item), error);
    }
    if (selection.kind == SpriteTrackItemKind::SocketKey) {
        const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
            [selection](const auto& item) { return item.id == selection.id; });
        if (found == clip->socketKeys.end()) return fail(error, "sprite socket key does not exist");
        SpriteSocketKey item = *found;
        if (handle == SpriteTrackHandleKind::SocketScale) {
            item.scale.x = std::max(0.01F, item.scale.x + delta.x * 0.05F);
            item.scale.y = std::max(0.01F, item.scale.y + delta.y * 0.05F);
        } else {
            item.positionPixels.x += delta.x;
            item.positionPixels.y += delta.y;
        }
        return session_->update_socket_key_by_id(clipName, selection.id, std::move(item), error);
    }
    return fail(error, "mouse handles are available for combat volumes and sockets");
}

bool SpriteTrackProductionEditor::adjust_numeric(
    std::string_view clipName, SpriteTrackSelection selection,
    SpriteTrackNumericField field, float delta, std::string* error) {
    if (session_ == nullptr || !std::isfinite(delta))
        return fail(error, "sprite numeric edit is invalid");
    const SpriteClip* clip = find_clip(session_->asset(), clipName);
    if (clip == nullptr) return fail(error, "sprite clip does not exist");
    if (selection.kind == SpriteTrackItemKind::CombatWindow) {
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [selection](const auto& item) { return item.id == selection.id; });
        if (found == clip->combatWindows.end()) return fail(error, "sprite combat window does not exist");
        SpriteCombatWindow item = *found;
        switch (field) {
        case SpriteTrackNumericField::AttackId:
            item.volume.attackId = static_cast<std::uint32_t>(std::max(0.0F,
                static_cast<float>(item.volume.attackId) + delta)); break;
        case SpriteTrackNumericField::Damage: item.volume.damage = std::max(0.0F, item.volume.damage + delta); break;
        case SpriteTrackNumericField::KnockbackX: item.volume.knockbackPixelsPerSecond.x += delta; break;
        case SpriteTrackNumericField::KnockbackY: item.volume.knockbackPixelsPerSecond.y += delta; break;
        case SpriteTrackNumericField::HitStopTicks:
            item.volume.hitStopTicks = static_cast<std::uint32_t>(std::max(0.0F,
                static_cast<float>(item.volume.hitStopTicks) + delta)); break;
        case SpriteTrackNumericField::Priority:
            item.volume.priority = static_cast<std::int32_t>(std::clamp(
                static_cast<double>(item.volume.priority) + static_cast<double>(delta),
                static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                static_cast<double>(std::numeric_limits<std::int32_t>::max()))); break;
        case SpriteTrackNumericField::PositionX: item.volume.centerPixels.x += delta; break;
        case SpriteTrackNumericField::PositionY: item.volume.centerPixels.y += delta; break;
        case SpriteTrackNumericField::Rotation: item.volume.rotationDegrees += delta; break;
        default: return fail(error, "numeric field does not apply to a combat volume");
        }
        return session_->update_combat_window_by_id(clipName, selection.id, std::move(item), error);
    }
    if (selection.kind == SpriteTrackItemKind::SocketKey) {
        const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
            [selection](const auto& item) { return item.id == selection.id; });
        if (found == clip->socketKeys.end()) return fail(error, "sprite socket key does not exist");
        SpriteSocketKey item = *found;
        switch (field) {
        case SpriteTrackNumericField::PositionX: item.positionPixels.x += delta; break;
        case SpriteTrackNumericField::PositionY: item.positionPixels.y += delta; break;
        case SpriteTrackNumericField::Rotation: item.rotationDegrees += delta; break;
        case SpriteTrackNumericField::ScaleX: item.scale.x = std::max(0.01F, item.scale.x + delta); break;
        case SpriteTrackNumericField::ScaleY: item.scale.y = std::max(0.01F, item.scale.y + delta); break;
        default: return fail(error, "numeric field does not apply to a socket");
        }
        return session_->update_socket_key_by_id(clipName, selection.id, std::move(item), error);
    }
    if (selection.kind == SpriteTrackItemKind::RootMotionKey) {
        const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [selection](const auto& item) { return item.id == selection.id; });
        if (found == clip->rootMotionKeys.end()) return fail(error, "sprite root-motion key does not exist");
        SpriteRootMotionKey item = *found;
        switch (field) {
        case SpriteTrackNumericField::RootDeltaX: item.deltaPixels.x += delta; break;
        case SpriteTrackNumericField::RootDeltaY: item.deltaPixels.y += delta; break;
        case SpriteTrackNumericField::Rotation: item.rotationDegrees += delta; break;
        default: return fail(error, "numeric field does not apply to root motion");
        }
        return session_->update_root_motion_key_by_id(clipName, selection.id, item, error);
    }
    if (selection.kind == SpriteTrackItemKind::PropertyKey) {
        const auto found = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [selection](const auto& item) { return item.id == selection.id; });
        if (found == clip->propertyKeys.end()) return fail(error, "sprite property key does not exist");
        SpritePropertyKey item = *found;
        switch (field) {
        case SpriteTrackNumericField::PropertyBoolean: item.value.booleanValue = !item.value.booleanValue; break;
        case SpriteTrackNumericField::PropertyInteger: item.value.integerValue += static_cast<std::int64_t>(delta); break;
        case SpriteTrackNumericField::PropertyFloat: item.value.floatValue += delta; break;
        case SpriteTrackNumericField::PropertyVec2X: item.value.vec2Value.x += delta; break;
        case SpriteTrackNumericField::PropertyVec2Y: item.value.vec2Value.y += delta; break;
        default: return fail(error, "numeric field does not apply to a property");
        }
        return session_->update_property_key_by_id(clipName, selection.id, std::move(item), error);
    }
    return fail(error, "sprite track type is invalid");
}

std::vector<SpriteTrackSearchMatch> SpriteTrackProductionEditor::search(
    std::string_view query, std::optional<std::string_view> clipName) const {
    std::vector<SpriteTrackSearchMatch> result;
    if (session_ == nullptr || query.empty()) return result;
    for (const SpriteClip& clip : session_->asset().clips) {
        if (clipName && clip.name != *clipName) continue;
        for (std::size_t sequence = 0U; sequence < clip.frames.size(); ++sequence) {
            const SpriteFrame& frame = session_->asset().frames[clip.frames[sequence]];
            if (contains_ci(frame.event, query))
                result.push_back({clip.name, sequence, SpriteTrackItemKind::PropertyKey,
                                  kInvalidSpriteTrackId, "event", frame.event});
        }
        for (const SpriteCombatWindow& item : clip.combatWindows) {
            if (contains_ci(item.volume.name, query))
                result.push_back({clip.name, item.firstSequenceIndex,
                    SpriteTrackItemKind::CombatWindow, item.id, "name", item.volume.name});
        }
        for (const SpriteSocketKey& item : clip.socketKeys) {
            if (contains_ci(item.name, query))
                result.push_back({clip.name, item.sequenceIndex, SpriteTrackItemKind::SocketKey,
                                  item.id, "socket", item.name});
        }
        for (const SpritePropertyKey& item : clip.propertyKeys) {
            std::string value = item.name + " " + item.value.stringValue;
            if (contains_ci(value, query))
                result.push_back({clip.name, item.sequenceIndex, SpriteTrackItemKind::PropertyKey,
                                  item.id, "property", value});
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.clip != right.clip) return left.clip < right.clip;
        if (left.sequenceIndex != right.sequenceIndex) return left.sequenceIndex < right.sequenceIndex;
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.id < right.id;
    });
    return result;
}

std::vector<SpriteTrackDiagnostic> SpriteTrackProductionEditor::diagnostics(
    std::optional<std::string_view> clipName) const {
    std::vector<SpriteTrackDiagnostic> result;
    if (session_ == nullptr) return result;
    for (const SpriteClip& clip : session_->asset().clips) {
        if (clipName && clip.name != *clipName) continue;
        for (const SpriteCombatWindow& item : clip.combatWindows) {
            const auto push = [&](SpriteTrackDiagnosticSeverity severity, std::string field,
                                  std::string message) {
                result.push_back({severity, clip.name, item.firstSequenceIndex,
                    SpriteTrackItemKind::CombatWindow, item.id, std::move(field), std::move(message)});
            };
            if (item.volume.role == SpriteCombatRole::Hitbox && item.volume.attackId == 0U)
                push(SpriteTrackDiagnosticSeverity::Warning, "attackId",
                     "Hitbox has no attack ID; hit de-duplication may merge unrelated attacks");
            if (item.volume.role == SpriteCombatRole::Hitbox && item.volume.damage <= 0.0F)
                push(SpriteTrackDiagnosticSeverity::Warning, "damage",
                     "Hitbox damage is zero");
            if (item.volume.sizePixels.x < 2.0F || item.volume.sizePixels.y < 2.0F)
                push(SpriteTrackDiagnosticSeverity::Warning, "size",
                     "Combat volume is smaller than two pixels");
            if (item.volume.hitStopTicks > 60U)
                push(SpriteTrackDiagnosticSeverity::Warning, "hitStopTicks",
                     "Hit stop exceeds one second at 60 Hz");
            if (item.volume.role == SpriteCombatRole::Trigger &&
                item.volume.name.find("spawn") == std::string::npos)
                push(SpriteTrackDiagnosticSeverity::Info, "name",
                     "Trigger volumes used as spawn regions should include 'spawn' in the name");
        }
        for (const SpriteSocketKey& item : clip.socketKeys) {
            if (item.name.empty())
                result.push_back({SpriteTrackDiagnosticSeverity::Error, clip.name,
                    item.sequenceIndex, SpriteTrackItemKind::SocketKey, item.id, "name",
                    "Socket name is empty"});
            if (std::fabs(item.scale.x) < 0.05F || std::fabs(item.scale.y) < 0.05F)
                result.push_back({SpriteTrackDiagnosticSeverity::Warning, clip.name,
                    item.sequenceIndex, SpriteTrackItemKind::SocketKey, item.id, "scale",
                    "Socket scale is nearly zero"});
        }
        for (const SpritePropertyKey& item : clip.propertyKeys) {
            if (item.value.type == SpritePropertyType::AssetReference && item.value.stringValue.empty())
                result.push_back({SpriteTrackDiagnosticSeverity::Warning, clip.name,
                    item.sequenceIndex, SpriteTrackItemKind::PropertyKey, item.id, "assetReference",
                    "Asset-reference property is empty"});
        }
        std::set<std::pair<std::string, std::size_t>> sockets;
        for (const SpriteSocketKey& item : clip.socketKeys) {
            if (!sockets.emplace(item.name, item.sequenceIndex).second)
                result.push_back({SpriteTrackDiagnosticSeverity::Error, clip.name,
                    item.sequenceIndex, SpriteTrackItemKind::SocketKey, item.id, "sequenceIndex",
                    "Duplicate socket key at the same frame"});
        }
    }
    return result;
}

std::vector<SpriteTrackOverlay> SpriteTrackProductionEditor::onion_overlays(
    std::string_view clipName, std::size_t sequenceIndex, bool previous, bool next,
    float opacity) const {
    std::vector<SpriteTrackOverlay> result;
    if (session_ == nullptr || !std::isfinite(opacity)) return result;
    const SpriteClip* clip = find_clip(session_->asset(), clipName);
    if (clip == nullptr || clip->frames.empty() || sequenceIndex >= clip->frames.size()) return result;
    const auto append = [&](std::size_t sequence, int relative) {
        for (const SpriteCombatWindow& item : clip->combatWindows) {
            if (sequence < item.firstSequenceIndex || sequence > item.lastSequenceIndex) continue;
            result.push_back({SpriteTrackItemKind::CombatWindow, item.id, sequence, relative,
                std::clamp(opacity, 0.0F, 1.0F), item.volume.shape, item.volume.centerPixels,
                item.volume.sizePixels, item.volume.radiusPixels, item.volume.rotationDegrees,
                item.volume.name});
        }
        for (const SpriteSocketKey& item : clip->socketKeys) {
            if (item.sequenceIndex != sequence) continue;
            result.push_back({SpriteTrackItemKind::SocketKey, item.id, sequence, relative,
                std::clamp(opacity, 0.0F, 1.0F), SpriteCombatShape::Circle, item.positionPixels,
                {4.0F, 4.0F}, 2.0F, item.rotationDegrees, item.name});
        }
    };
    if (previous && sequenceIndex > 0U) append(sequenceIndex - 1U, -1);
    if (next && sequenceIndex + 1U < clip->frames.size()) append(sequenceIndex + 1U, 1);
    return result;
}

std::vector<SpriteTrackNumericDescriptor> SpriteTrackProductionEditor::numeric_descriptors(
    std::string_view clipName, SpriteTrackSelection selection) const {
    std::vector<SpriteTrackNumericDescriptor> result;
    if (session_ == nullptr) return result;
    const SpriteClip* clip = find_clip(session_->asset(), clipName);
    if (clip == nullptr) return result;
    if (selection.kind == SpriteTrackItemKind::CombatWindow) {
        const auto item = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [selection](const auto& value) { return value.id == selection.id; });
        if (item == clip->combatWindows.end()) return result;
        result = {
            {SpriteTrackNumericField::AttackId, "Attack ID", 1.0F, std::to_string(item->volume.attackId)},
            {SpriteTrackNumericField::Damage, "Damage", 1.0F, format_float(item->volume.damage)},
            {SpriteTrackNumericField::KnockbackX, "Knockback X", 8.0F, format_float(item->volume.knockbackPixelsPerSecond.x)},
            {SpriteTrackNumericField::KnockbackY, "Knockback Y", 8.0F, format_float(item->volume.knockbackPixelsPerSecond.y)},
            {SpriteTrackNumericField::HitStopTicks, "Hit-stop", 1.0F, std::to_string(item->volume.hitStopTicks)},
            {SpriteTrackNumericField::Priority, "Priority", 1.0F, std::to_string(item->volume.priority)},
        };
    } else if (selection.kind == SpriteTrackItemKind::SocketKey) {
        const auto item = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
            [selection](const auto& value) { return value.id == selection.id; });
        if (item == clip->socketKeys.end()) return result;
        result = {
            {SpriteTrackNumericField::PositionX, "Position X", 1.0F, format_float(item->positionPixels.x)},
            {SpriteTrackNumericField::PositionY, "Position Y", 1.0F, format_float(item->positionPixels.y)},
            {SpriteTrackNumericField::Rotation, "Rotation", 5.0F, format_float(item->rotationDegrees)},
            {SpriteTrackNumericField::ScaleX, "Scale X", 0.1F, format_float(item->scale.x)},
            {SpriteTrackNumericField::ScaleY, "Scale Y", 0.1F, format_float(item->scale.y)},
        };
    } else if (selection.kind == SpriteTrackItemKind::RootMotionKey) {
        const auto item = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [selection](const auto& value) { return value.id == selection.id; });
        if (item == clip->rootMotionKeys.end()) return result;
        result = {
            {SpriteTrackNumericField::RootDeltaX, "Delta X", 1.0F, format_float(item->deltaPixels.x)},
            {SpriteTrackNumericField::RootDeltaY, "Delta Y", 1.0F, format_float(item->deltaPixels.y)},
            {SpriteTrackNumericField::Rotation, "Rotation", 5.0F, format_float(item->rotationDegrees)},
        };
    } else if (selection.kind == SpriteTrackItemKind::PropertyKey) {
        const auto item = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [selection](const auto& value) { return value.id == selection.id; });
        if (item == clip->propertyKeys.end()) return result;
        switch (item->value.type) {
        case SpritePropertyType::Boolean:
            result.push_back({SpriteTrackNumericField::PropertyBoolean, "Boolean", 1.0F,
                              item->value.booleanValue ? "true" : "false"}); break;
        case SpritePropertyType::Integer:
            result.push_back({SpriteTrackNumericField::PropertyInteger, "Integer", 1.0F,
                              std::to_string(item->value.integerValue)}); break;
        case SpritePropertyType::Float:
            result.push_back({SpriteTrackNumericField::PropertyFloat, "Float", 0.1F,
                              format_float(item->value.floatValue)}); break;
        case SpritePropertyType::Vec2:
            result.push_back({SpriteTrackNumericField::PropertyVec2X, "Vec2 X", 0.1F,
                              format_float(item->value.vec2Value.x)});
            result.push_back({SpriteTrackNumericField::PropertyVec2Y, "Vec2 Y", 0.1F,
                              format_float(item->value.vec2Value.y)}); break;
        case SpritePropertyType::String:
        case SpritePropertyType::AssetReference:
            break;
        }
    }
    return result;
}

} // namespace dve::editor
