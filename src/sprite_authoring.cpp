#include "dve/sprite_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace dve::editor {
namespace {

constexpr std::uint32_t kMaximumTextureDimension = 16384U;
constexpr std::size_t kMaximumFrames = 65536U;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool checked_image_bytes(
    std::uint32_t width, std::uint32_t height, std::size_t& bytes) noexcept {
    if (width == 0U || height == 0U || width > kMaximumTextureDimension ||
        height > kMaximumTextureDimension) return false;
    const std::uint64_t count = static_cast<std::uint64_t>(width) * height * 4U;
    if (count > std::numeric_limits<std::size_t>::max()) return false;
    bytes = static_cast<std::size_t>(count);
    return true;
}

bool rect_inside(const SpriteRectPixels& rect, std::uint32_t width, std::uint32_t height) noexcept {
    return rect.width > 0U && rect.height > 0U && rect.x <= width && rect.y <= height &&
        rect.width <= width - rect.x && rect.height <= height - rect.y;
}

std::optional<SpriteFrame> make_frame(
    const SpriteSourceImageView& image,
    const SpriteSliceSpec& slice,
    bool trimTransparent,
    std::uint8_t alphaThreshold,
    bool skipTransparent,
    std::string* error) {
    if (slice.name.empty() || slice.name.size() > 255U ||
        !rect_inside(slice.rect, image.width, image.height) ||
        !std::isfinite(slice.pivotPixels.x) || !std::isfinite(slice.pivotPixels.y) ||
        !std::isfinite(slice.durationSeconds) || slice.durationSeconds <= 0.0F ||
        slice.event.size() > 255U) {
        fail(error, "sprite slice metadata is invalid");
        return std::nullopt;
    }

    std::uint32_t minimumX = slice.rect.width;
    std::uint32_t minimumY = slice.rect.height;
    std::uint32_t maximumX{};
    std::uint32_t maximumY{};
    bool found = false;
    for (std::uint32_t y = 0U; y < slice.rect.height; ++y) {
        for (std::uint32_t x = 0U; x < slice.rect.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(slice.rect.y + y) * image.width + slice.rect.x + x) * 4U;
            if (std::to_integer<std::uint8_t>(image.rgba8[pixel + 3U]) <= alphaThreshold) continue;
            found = true;
            minimumX = std::min(minimumX, x);
            minimumY = std::min(minimumY, y);
            maximumX = std::max(maximumX, x);
            maximumY = std::max(maximumY, y);
        }
    }
    if (!found && skipTransparent) return std::nullopt;
    if (!found || !trimTransparent) {
        minimumX = 0U;
        minimumY = 0U;
        maximumX = slice.rect.width - 1U;
        maximumY = slice.rect.height - 1U;
    }

    SpriteFrame frame;
    frame.name = slice.name;
    frame.atlasRect = {
        slice.rect.x + minimumX,
        slice.rect.y + minimumY,
        maximumX - minimumX + 1U,
        maximumY - minimumY + 1U,
    };
    frame.sourceWidth = slice.rect.width;
    frame.sourceHeight = slice.rect.height;
    frame.sourceOffsetX = static_cast<std::int32_t>(minimumX);
    frame.sourceOffsetY = static_cast<std::int32_t>(
        slice.rect.height - (maximumY + 1U));
    frame.pivotPixels = slice.pivotPixels;
    frame.durationSeconds = slice.durationSeconds;
    frame.event = slice.event;
    return frame;
}

SpriteClip* find_clip(SpriteAsset& asset, std::string_view name) noexcept {
    const auto found = std::find_if(asset.clips.begin(), asset.clips.end(),
        [name](const SpriteClip& clip) { return clip.name == name; });
    return found == asset.clips.end() ? nullptr : &*found;
}

[[nodiscard]] std::size_t remap_sequence_index_after_move(
    std::size_t index, std::size_t from, std::size_t to) noexcept {
    if (from == to) return index;
    if (index == from) return to;
    if (from < to && index > from && index <= to) return index - 1U;
    if (to < from && index >= to && index < from) return index + 1U;
    return index;
}

void remap_clip_metadata_after_move(SpriteClip& clip, std::size_t from, std::size_t to) {
    if (from == to) return;
    std::vector<SpriteCombatWindow> remappedCombat;
    for (const SpriteCombatWindow& window : clip.combatWindows) {
        std::vector<std::size_t> positions;
        positions.reserve(window.lastSequenceIndex - window.firstSequenceIndex + 1U);
        for (std::size_t index = window.firstSequenceIndex;
             index <= window.lastSequenceIndex; ++index) {
            positions.push_back(remap_sequence_index_after_move(index, from, to));
        }
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
        if (positions.empty()) continue;
        std::size_t runFirst = positions.front();
        std::size_t runLast = runFirst;
        bool firstSegment = true;
        for (std::size_t index = 1U; index < positions.size(); ++index) {
            if (positions[index] == runLast + 1U) {
                runLast = positions[index];
                continue;
            }
            remappedCombat.push_back({runFirst, runLast, window.volume,
                firstSegment ? window.id : kInvalidSpriteTrackId});
            firstSegment = false;
            runFirst = runLast = positions[index];
        }
        remappedCombat.push_back({runFirst, runLast, window.volume,
            firstSegment ? window.id : kInvalidSpriteTrackId});
    }
    clip.combatWindows = std::move(remappedCombat);
    for (SpriteSocketKey& key : clip.socketKeys)
        key.sequenceIndex = remap_sequence_index_after_move(key.sequenceIndex, from, to);
    for (SpritePropertyKey& key : clip.propertyKeys)
        key.sequenceIndex = remap_sequence_index_after_move(key.sequenceIndex, from, to);
    for (SpriteRootMotionKey& key : clip.rootMotionKeys)
        key.sequenceIndex = remap_sequence_index_after_move(key.sequenceIndex, from, to);
    std::sort(clip.socketKeys.begin(), clip.socketKeys.end(), [](const SpriteSocketKey& left,
                                                                 const SpriteSocketKey& right) {
        return left.name < right.name || (left.name == right.name &&
                                          left.sequenceIndex < right.sequenceIndex);
    });
    std::sort(clip.propertyKeys.begin(), clip.propertyKeys.end(), [](const SpritePropertyKey& left,
                                                                     const SpritePropertyKey& right) {
        return left.name < right.name || (left.name == right.name &&
                                          left.sequenceIndex < right.sequenceIndex);
    });
    std::sort(clip.rootMotionKeys.begin(), clip.rootMotionKeys.end(),
              [](const SpriteRootMotionKey& left, const SpriteRootMotionKey& right) {
                  return left.sequenceIndex < right.sequenceIndex;
              });
}


void remap_clip_metadata_with_permutation(
    SpriteClip& clip, std::span<const std::size_t> oldToNew) {
    std::vector<SpriteCombatWindow> remappedCombat;
    for (const SpriteCombatWindow& window : clip.combatWindows) {
        std::vector<std::size_t> positions;
        positions.reserve(window.lastSequenceIndex - window.firstSequenceIndex + 1U);
        for (std::size_t index = window.firstSequenceIndex;
             index <= window.lastSequenceIndex; ++index) {
            positions.push_back(oldToNew[index]);
        }
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
        std::size_t runFirst = positions.front();
        std::size_t runLast = runFirst;
        bool firstSegment = true;
        for (std::size_t index = 1U; index < positions.size(); ++index) {
            if (positions[index] == runLast + 1U) {
                runLast = positions[index];
            } else {
                remappedCombat.push_back({runFirst, runLast, window.volume,
                    firstSegment ? window.id : kInvalidSpriteTrackId});
                firstSegment = false;
                runFirst = runLast = positions[index];
            }
        }
        remappedCombat.push_back({runFirst, runLast, window.volume,
            firstSegment ? window.id : kInvalidSpriteTrackId});
    }
    clip.combatWindows = std::move(remappedCombat);
    for (SpriteSocketKey& key : clip.socketKeys) key.sequenceIndex = oldToNew[key.sequenceIndex];
    for (SpritePropertyKey& key : clip.propertyKeys) key.sequenceIndex = oldToNew[key.sequenceIndex];
    for (SpriteRootMotionKey& key : clip.rootMotionKeys) key.sequenceIndex = oldToNew[key.sequenceIndex];
    std::sort(clip.socketKeys.begin(), clip.socketKeys.end(), [](const SpriteSocketKey& left,
                                                                 const SpriteSocketKey& right) {
        return left.name < right.name || (left.name == right.name && left.sequenceIndex < right.sequenceIndex);
    });
    std::sort(clip.propertyKeys.begin(), clip.propertyKeys.end(), [](const SpritePropertyKey& left,
                                                                     const SpritePropertyKey& right) {
        return left.name < right.name || (left.name == right.name && left.sequenceIndex < right.sequenceIndex);
    });
    std::sort(clip.rootMotionKeys.begin(), clip.rootMotionKeys.end(),
              [](const SpriteRootMotionKey& left, const SpriteRootMotionKey& right) {
                  return left.sequenceIndex < right.sequenceIndex;
              });
}

} // namespace

bool SpriteSourceImageView::validate(std::string* error) const {
    std::size_t expected{};
    if (!checked_image_bytes(width, height, expected) || rgba8.size() != expected)
        return fail(error, "sprite source image dimensions or RGBA8 byte count is invalid");
    return true;
}

SpriteAsset make_sprite_authoring_asset(
    std::string name,
    std::string textureAsset,
    std::uint32_t textureWidth,
    std::uint32_t textureHeight,
    float pixelsPerWorldUnit) {
    SpriteAsset asset;
    asset.name = std::move(name);
    asset.textureAsset = std::move(textureAsset);
    asset.textureWidth = textureWidth;
    asset.textureHeight = textureHeight;
    asset.pixelsPerWorldUnit = pixelsPerWorldUnit;
    SpriteFrame frame;
    frame.name = "frame_0000";
    frame.atlasRect = {0U, 0U, textureWidth, textureHeight};
    frame.sourceWidth = textureWidth;
    frame.sourceHeight = textureHeight;
    frame.pivotPixels = {
        static_cast<float>(textureWidth) * 0.5F,
        static_cast<float>(textureHeight) * 0.5F,
    };
    asset.frames.push_back(std::move(frame));
    asset.clips.push_back({"default", SpriteLoopMode::Loop, 1.0F, {0U}});
    asset.recompute_hash();
    return asset;
}

SpriteAuthoringSession::SpriteAuthoringSession(SpriteAsset asset) {
    std::string error;
    if (!asset.validate(&error))
        throw std::invalid_argument("invalid sprite authoring asset: " + error);
    asset.recompute_hash();
    document_.asset = std::move(asset);
    document_.selectedClip = document_.asset.clips.front().name;
    document_.selectedFrame = document_.asset.clips.front().frames.front();
}

bool SpriteAuthoringSession::open(const std::filesystem::path& path, std::string* error) {
    const SpriteAssetReadResult result = read_dvesprite(path);
    if (!result) return fail(error, result.error);
    document_ = {};
    document_.asset = result.asset;
    document_.selectedClip = document_.asset.clips.front().name;
    document_.selectedFrame = document_.asset.clips.front().frames.front();
    path_ = path;
    savedHash_ = document_.asset.contentHash;
    previewTimeSeconds_ = 0.0F;
    previewPlaying_ = true;
    clear_history();
    return true;
}

bool SpriteAuthoringSession::save(const std::filesystem::path& path, std::string* error) {
    document_.asset.recompute_hash();
    if (!write_dvesprite(path, document_.asset, error)) return false;
    path_ = path;
    savedHash_ = document_.asset.contentHash;
    return true;
}

bool SpriteAuthoringSession::replace_asset(
    SpriteAsset replacement, std::string label, std::string* error) {
    return mutate(std::move(label), [replacement = std::move(replacement)](
        SpriteAsset& asset, std::string*) mutable {
        assign_sprite_track_ids(replacement);
        asset = std::move(replacement);
        return true;
    }, error);
}

bool SpriteAuthoringSession::mutate(
    std::string label,
    const std::function<bool(SpriteAsset&, std::string*)>& operation,
    std::string* error) {
    SpriteAsset candidate = document_.asset;
    if (!operation(candidate, error)) return false;
    candidate.recompute_hash();
    if (!candidate.validate(error)) return false;
    if (candidate.contentHash == document_.asset.contentHash) return false;
    undo_.push_back({std::move(label), document_});
    if (undo_.size() > historyLimit_) undo_.erase(undo_.begin());
    redo_.clear();
    document_.asset = std::move(candidate);
    ++document_.revision;
    normalize_selection();
    return true;
}

void SpriteAuthoringSession::normalize_selection() noexcept {
    if (find_sprite_clip(document_.asset, document_.selectedClip) == nullptr)
        document_.selectedClip = document_.asset.clips.front().name;
    if (!document_.selectedFrame || *document_.selectedFrame >= document_.asset.frames.size())
        document_.selectedFrame = document_.asset.clips.front().frames.front();
    if (document_.selectedTrack) {
        const SpriteClip* clip = find_sprite_clip(document_.asset, document_.selectedClip);
        bool exists = false;
        if (clip != nullptr) {
            const SpriteTrackId id = document_.selectedTrack->id;
            switch (document_.selectedTrack->kind) {
            case SpriteTrackItemKind::CombatWindow:
                exists = std::any_of(clip->combatWindows.begin(), clip->combatWindows.end(),
                    [id](const SpriteCombatWindow& item) { return item.id == id; });
                break;
            case SpriteTrackItemKind::SocketKey:
                exists = std::any_of(clip->socketKeys.begin(), clip->socketKeys.end(),
                    [id](const SpriteSocketKey& item) { return item.id == id; });
                break;
            case SpriteTrackItemKind::PropertyKey:
                exists = std::any_of(clip->propertyKeys.begin(), clip->propertyKeys.end(),
                    [id](const SpritePropertyKey& item) { return item.id == id; });
                break;
            case SpriteTrackItemKind::RootMotionKey:
                exists = std::any_of(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
                    [id](const SpriteRootMotionKey& item) { return item.id == id; });
                break;
            }
        }
        if (!exists) document_.selectedTrack.reset();
    }
}

bool SpriteAuthoringSession::slice_grid(
    const SpriteSourceImageView& image,
    const SpriteGridSliceSettings& settings,
    std::string* error) {
    if (!image.validate(error)) return false;
    if (image.width != document_.asset.textureWidth || image.height != document_.asset.textureHeight)
        return fail(error, "sprite source image dimensions differ from the asset texture");
    if (settings.cellWidth == 0U || settings.cellHeight == 0U ||
        settings.originX >= image.width || settings.originY >= image.height ||
        settings.cellWidth > image.width - settings.originX ||
        settings.cellHeight > image.height - settings.originY ||
        settings.frameNamePrefix.empty() || settings.frameNamePrefix.size() > 240U ||
        !std::isfinite(settings.pivotPixels.x) || !std::isfinite(settings.pivotPixels.y) ||
        !std::isfinite(settings.durationSeconds) || settings.durationSeconds <= 0.0F)
        return fail(error, "sprite grid slicing settings are invalid");
    const std::uint64_t stepX = static_cast<std::uint64_t>(settings.cellWidth) + settings.spacingX;
    const std::uint64_t stepY = static_cast<std::uint64_t>(settings.cellHeight) + settings.spacingY;
    const std::uint32_t derivedColumns = 1U + static_cast<std::uint32_t>(
        (image.width - settings.originX - settings.cellWidth) / stepX);
    const std::uint32_t derivedRows = 1U + static_cast<std::uint32_t>(
        (image.height - settings.originY - settings.cellHeight) / stepY);
    const std::uint32_t columns = settings.columns == 0U ? derivedColumns : settings.columns;
    const std::uint32_t rows = settings.rows == 0U ? derivedRows : settings.rows;
    if (columns == 0U || rows == 0U || columns > derivedColumns || rows > derivedRows ||
        static_cast<std::uint64_t>(columns) * rows > kMaximumFrames)
        return fail(error, "sprite grid does not fit the source image or exceeds the frame limit");
    std::vector<SpriteSliceSpec> slices;
    slices.reserve(static_cast<std::size_t>(columns) * rows);
    for (std::uint32_t row = 0U; row < rows; ++row) {
        for (std::uint32_t column = 0U; column < columns; ++column) {
            SpriteSliceSpec slice;
            const std::size_t index = slices.size();
            slice.name = settings.frameNamePrefix + "_" + std::to_string(index);
            slice.rect = {
                settings.originX + static_cast<std::uint32_t>(column * stepX),
                settings.originY + static_cast<std::uint32_t>(row * stepY),
                settings.cellWidth,
                settings.cellHeight,
            };
            slice.pivotPixels = settings.pivotPixels;
            slice.durationSeconds = settings.durationSeconds;
            slices.push_back(std::move(slice));
        }
    }
    return replace_slices(image, slices, settings.trimTransparent, settings.alphaThreshold,
        settings.skipTransparent, error);
}

bool SpriteAuthoringSession::replace_slices(
    const SpriteSourceImageView& image,
    std::span<const SpriteSliceSpec> slices,
    bool trimTransparent,
    std::uint8_t alphaThreshold,
    bool skipTransparent,
    std::string* error) {
    if (!image.validate(error)) return false;
    if (image.width != document_.asset.textureWidth || image.height != document_.asset.textureHeight)
        return fail(error, "sprite source image dimensions differ from the asset texture");
    if (slices.empty() || slices.size() > kMaximumFrames)
        return fail(error, "sprite slice count is invalid");
    std::vector<SpriteFrame> frames;
    frames.reserve(slices.size());
    std::set<std::string, std::less<>> names;
    for (const SpriteSliceSpec& slice : slices) {
        if (!names.insert(slice.name).second)
            return fail(error, "sprite slice names must be unique");
        std::string local;
        std::optional<SpriteFrame> frame = make_frame(
            image, slice, trimTransparent, alphaThreshold, skipTransparent, &local);
        if (!local.empty()) return fail(error, local);
        if (frame) frames.push_back(std::move(*frame));
    }
    if (frames.empty()) return fail(error, "sprite slicing produced no visible frames");
    return mutate("Slice Sprite", [frames = std::move(frames)](
        SpriteAsset& asset, std::string*) mutable {
        asset.frames = std::move(frames);
        SpriteClip clip;
        clip.name = "default";
        clip.frames.resize(asset.frames.size());
        for (std::size_t index = 0U; index < clip.frames.size(); ++index)
            clip.frames[index] = static_cast<SpriteFrameIndex>(index);
        asset.clips = {std::move(clip)};
        return true;
    }, error);
}

bool SpriteAuthoringSession::update_frame(
    SpriteFrameIndex frame, SpriteFrame replacement, std::string* error) {
    return mutate("Edit Sprite Frame", [frame, replacement = std::move(replacement)](
        SpriteAsset& asset, std::string* local) mutable {
        if (frame >= asset.frames.size()) return fail(local, "sprite frame index is invalid");
        asset.frames[frame] = std::move(replacement);
        return true;
    }, error);
}

bool SpriteAuthoringSession::set_frame_pivot(
    SpriteFrameIndex frame, SpriteVec2 pivotPixels, std::string* error) {
    return mutate("Set Sprite Pivot", [frame, pivotPixels](SpriteAsset& asset, std::string* local) {
        if (frame >= asset.frames.size()) return fail(local, "sprite frame index is invalid");
        asset.frames[frame].pivotPixels = pivotPixels;
        return true;
    }, error);
}

bool SpriteAuthoringSession::set_frame_timing_event(
    SpriteFrameIndex frame, float durationSeconds, std::string event, std::string* error) {
    return mutate("Edit Sprite Timeline", [frame, durationSeconds, event = std::move(event)](
        SpriteAsset& asset, std::string* local) mutable {
        if (frame >= asset.frames.size()) return fail(local, "sprite frame index is invalid");
        asset.frames[frame].durationSeconds = durationSeconds;
        asset.frames[frame].event = std::move(event);
        return true;
    }, error);
}

bool SpriteAuthoringSession::set_atlas_layout(
    std::uint32_t textureWidth, std::uint32_t textureHeight,
    std::span<const SpriteRectPixels> frameRects, std::string* error) {
    std::vector<SpriteRectPixels> rects(frameRects.begin(), frameRects.end());
    return mutate("Repack Sprite Atlas", [textureWidth, textureHeight, rects = std::move(rects)](
        SpriteAsset& asset, std::string* local) mutable {
        if (textureWidth == 0U || textureHeight == 0U ||
            rects.size() != asset.frames.size()) {
            return fail(local, "atlas layout dimensions or frame count are invalid");
        }
        for (const SpriteRectPixels& rect : rects) {
            if (!rect_inside(rect, textureWidth, textureHeight)) {
                return fail(local, "atlas layout contains a frame outside the texture");
            }
        }
        asset.textureWidth = textureWidth;
        asset.textureHeight = textureHeight;
        for (std::size_t index = 0U; index < rects.size(); ++index) {
            asset.frames[index].atlasRect = rects[index];
        }
        return true;
    }, error);
}

bool SpriteAuthoringSession::set_texture_source(
    std::string textureAsset, std::uint32_t textureWidth, std::uint32_t textureHeight,
    std::string* error) {
    if (textureAsset.empty() || textureWidth == 0U || textureHeight == 0U) {
        if (error != nullptr) *error = "sprite texture source requires a path and non-zero dimensions";
        return false;
    }
    return mutate("Reimport Sprite Texture", [&](SpriteAsset& asset, std::string* operationError) {
        for (const SpriteFrame& frame : asset.frames) {
            const std::uint64_t maxX = static_cast<std::uint64_t>(frame.atlasRect.x) + frame.atlasRect.width;
            const std::uint64_t maxY = static_cast<std::uint64_t>(frame.atlasRect.y) + frame.atlasRect.height;
            if (maxX > textureWidth || maxY > textureHeight) {
                if (operationError != nullptr) {
                    *operationError = "reimported texture no longer contains every authored frame";
                }
                return false;
            }
        }
        asset.textureAsset = std::move(textureAsset);
        asset.textureWidth = textureWidth;
        asset.textureHeight = textureHeight;
        return true;
    }, error);
}

bool SpriteAuthoringSession::set_palette_source(
    std::string paletteAsset, std::string* error) {
    return mutate("Set Sprite Palette", [paletteAsset = std::move(paletteAsset)](
        SpriteAsset& asset, std::string*) mutable {
        asset.paletteAsset = std::move(paletteAsset);
        return true;
    }, error);
}

bool SpriteAuthoringSession::add_clip(SpriteClip clip, std::string* error) {
    return mutate("Add Sprite Clip", [clip = std::move(clip)](
        SpriteAsset& asset, std::string* local) mutable {
        if (find_clip(asset, clip.name)) return fail(local, "sprite clip name already exists");
        asset.clips.push_back(std::move(clip));
        return true;
    }, error);
}

bool SpriteAuthoringSession::update_clip(
    std::string_view clipName, SpriteClip replacement, std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Clip", [name, replacement = std::move(replacement)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (!clip) return fail(local, "sprite clip does not exist");
        if (replacement.name != name && find_clip(asset, replacement.name))
            return fail(local, "sprite clip name already exists");
        *clip = std::move(replacement);
        return true;
    }, error);
}

bool SpriteAuthoringSession::remove_clip(std::string_view clipName, std::string* error) {
    const std::string name(clipName);
    return mutate("Remove Sprite Clip", [name](SpriteAsset& asset, std::string* local) {
        if (asset.clips.size() <= 1U) return fail(local, "sprite asset must keep one clip");
        const auto found = std::find_if(asset.clips.begin(), asset.clips.end(),
            [&name](const SpriteClip& clip) { return clip.name == name; });
        if (found == asset.clips.end()) return fail(local, "sprite clip does not exist");
        asset.clips.erase(found);
        return true;
    }, error);
}

bool SpriteAuthoringSession::move_clip_frame(
    std::string_view clipName, std::size_t from, std::size_t to, std::string* error) {
    const std::string name(clipName);
    return mutate("Reorder Sprite Timeline", [name, from, to](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (!clip || from >= clip->frames.size() || to >= clip->frames.size())
            return fail(local, "sprite clip timeline index is invalid");
        if (from == to) return true;
        remap_clip_metadata_after_move(*clip, from, to);
        const SpriteFrameIndex value = clip->frames[from];
        clip->frames.erase(clip->frames.begin() + static_cast<std::ptrdiff_t>(from));
        clip->frames.insert(clip->frames.begin() + static_cast<std::ptrdiff_t>(to), value);
        return true;
    }, error);
}

bool SpriteAuthoringSession::reorder_clip_timeline(
    std::string_view clipName, std::span<const std::size_t> oldIndicesInNewOrder,
    std::string* error) {
    const std::string name(clipName);
    std::vector<std::size_t> order(oldIndicesInNewOrder.begin(), oldIndicesInNewOrder.end());
    return mutate("Reorder Sprite Timeline", [name, order = std::move(order)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr || order.size() != clip->frames.size())
            return fail(local, "sprite timeline permutation size is invalid");
        std::vector<bool> seen(order.size(), false);
        std::vector<std::size_t> oldToNew(order.size());
        for (std::size_t newIndex = 0U; newIndex < order.size(); ++newIndex) {
            const std::size_t oldIndex = order[newIndex];
            if (oldIndex >= order.size() || seen[oldIndex])
                return fail(local, "sprite timeline order is not a permutation");
            seen[oldIndex] = true;
            oldToNew[oldIndex] = newIndex;
        }
        std::vector<SpriteFrameIndex> frames;
        frames.reserve(order.size());
        for (const std::size_t oldIndex : order) frames.push_back(clip->frames[oldIndex]);
        remap_clip_metadata_with_permutation(*clip, oldToNew);
        clip->frames = std::move(frames);
        return true;
    }, error);
}

bool SpriteAuthoringSession::add_combat_window(
    std::string_view clipName, SpriteCombatWindow window, std::string* error) {
    const std::string name(clipName);
    return mutate("Add Sprite Combat Window", [name, window = std::move(window)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        if (window.id == kInvalidSpriteTrackId) window.id = next_sprite_track_id(asset);
        clip->combatWindows.push_back(std::move(window));
        return true;
    }, error);
}

bool SpriteAuthoringSession::update_combat_window(
    std::string_view clipName, std::size_t windowIndex, SpriteCombatWindow replacement,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Combat Window", [name, windowIndex,
                                                  replacement = std::move(replacement)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr || windowIndex >= clip->combatWindows.size())
            return fail(local, "sprite combat window index is invalid");
        if (replacement.id == kInvalidSpriteTrackId)
            replacement.id = clip->combatWindows[windowIndex].id;
        clip->combatWindows[windowIndex] = std::move(replacement);
        return true;
    }, error);
}

bool SpriteAuthoringSession::remove_combat_window(
    std::string_view clipName, std::size_t windowIndex, std::string* error) {
    const std::string name(clipName);
    return mutate("Remove Sprite Combat Window", [name, windowIndex](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr || windowIndex >= clip->combatWindows.size())
            return fail(local, "sprite combat window index is invalid");
        clip->combatWindows.erase(
            clip->combatWindows.begin() + static_cast<std::ptrdiff_t>(windowIndex));
        return true;
    }, error);
}

bool SpriteAuthoringSession::upsert_socket_key(
    std::string_view clipName, SpriteSocketKey key, std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Socket", [name, key = std::move(key)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
            [&key](const SpriteSocketKey& candidate) {
                return candidate.name == key.name && candidate.sequenceIndex == key.sequenceIndex;
            });
        if (found == clip->socketKeys.end()) {
            if (key.id == kInvalidSpriteTrackId) key.id = next_sprite_track_id(asset);
            clip->socketKeys.push_back(std::move(key));
        } else {
            if (key.id == kInvalidSpriteTrackId) key.id = found->id;
            *found = std::move(key);
        }
        std::sort(clip->socketKeys.begin(), clip->socketKeys.end(),
            [](const SpriteSocketKey& left, const SpriteSocketKey& right) {
                return left.name < right.name || (left.name == right.name &&
                                                  left.sequenceIndex < right.sequenceIndex);
            });
        return true;
    }, error);
}

bool SpriteAuthoringSession::remove_socket_key(
    std::string_view clipName, std::string_view socketName, std::size_t sequenceIndex,
    std::string* error) {
    const std::string clip(clipName);
    const std::string socket(socketName);
    return mutate("Remove Sprite Socket", [clip, socket, sequenceIndex](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* target = find_clip(asset, clip);
        if (target == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(target->socketKeys.begin(), target->socketKeys.end(),
            [&socket, sequenceIndex](const SpriteSocketKey& key) {
                return key.name == socket && key.sequenceIndex == sequenceIndex;
            });
        if (found == target->socketKeys.end()) return fail(local, "sprite socket key does not exist");
        target->socketKeys.erase(found);
        return true;
    }, error);
}

bool SpriteAuthoringSession::upsert_property_key(
    std::string_view clipName, SpritePropertyKey key, std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Property", [name, key = std::move(key)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [&key](const SpritePropertyKey& candidate) {
                return candidate.name == key.name && candidate.sequenceIndex == key.sequenceIndex;
            });
        if (found == clip->propertyKeys.end()) {
            if (key.id == kInvalidSpriteTrackId) key.id = next_sprite_track_id(asset);
            clip->propertyKeys.push_back(std::move(key));
        } else {
            if (key.id == kInvalidSpriteTrackId) key.id = found->id;
            *found = std::move(key);
        }
        std::sort(clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [](const SpritePropertyKey& left, const SpritePropertyKey& right) {
                return left.name < right.name || (left.name == right.name &&
                                                  left.sequenceIndex < right.sequenceIndex);
            });
        return true;
    }, error);
}

bool SpriteAuthoringSession::remove_property_key(
    std::string_view clipName, std::string_view propertyName, std::size_t sequenceIndex,
    std::string* error) {
    const std::string clip(clipName);
    const std::string property(propertyName);
    return mutate("Remove Sprite Property", [clip, property, sequenceIndex](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* target = find_clip(asset, clip);
        if (target == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(target->propertyKeys.begin(), target->propertyKeys.end(),
            [&property, sequenceIndex](const SpritePropertyKey& key) {
                return key.name == property && key.sequenceIndex == sequenceIndex;
            });
        if (found == target->propertyKeys.end())
            return fail(local, "sprite property key does not exist");
        target->propertyKeys.erase(found);
        return true;
    }, error);
}

bool SpriteAuthoringSession::set_root_motion_key(
    std::string_view clipName, SpriteRootMotionKey key, std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Root Motion", [name, key](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [key](const SpriteRootMotionKey& candidate) {
                return candidate.sequenceIndex == key.sequenceIndex;
            });
        if (found == clip->rootMotionKeys.end()) {
            if (key.id == kInvalidSpriteTrackId) key.id = next_sprite_track_id(asset);
            clip->rootMotionKeys.push_back(key);
        } else {
            if (key.id == kInvalidSpriteTrackId) key.id = found->id;
            *found = key;
        }
        std::sort(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [](const SpriteRootMotionKey& left, const SpriteRootMotionKey& right) {
                return left.sequenceIndex < right.sequenceIndex;
            });
        return true;
    }, error);
}

bool SpriteAuthoringSession::remove_root_motion_key(
    std::string_view clipName, std::size_t sequenceIndex, std::string* error) {
    const std::string name(clipName);
    return mutate("Remove Sprite Root Motion", [name, sequenceIndex](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [sequenceIndex](const SpriteRootMotionKey& key) {
                return key.sequenceIndex == sequenceIndex;
            });
        if (found == clip->rootMotionKeys.end())
            return fail(local, "sprite root-motion key does not exist");
        clip->rootMotionKeys.erase(found);
        return true;
    }, error);
}


bool SpriteAuthoringSession::select_track(
    SpriteTrackItemKind kind, SpriteTrackId id) noexcept {
    if (id == kInvalidSpriteTrackId) return false;
    const SpriteClip* clip = find_sprite_clip(document_.asset, document_.selectedClip);
    if (clip == nullptr) return false;
    bool found = false;
    switch (kind) {
    case SpriteTrackItemKind::CombatWindow:
        found = std::any_of(clip->combatWindows.begin(), clip->combatWindows.end(),
            [id](const SpriteCombatWindow& item) { return item.id == id; });
        break;
    case SpriteTrackItemKind::SocketKey:
        found = std::any_of(clip->socketKeys.begin(), clip->socketKeys.end(),
            [id](const SpriteSocketKey& item) { return item.id == id; });
        break;
    case SpriteTrackItemKind::PropertyKey:
        found = std::any_of(clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [id](const SpritePropertyKey& item) { return item.id == id; });
        break;
    case SpriteTrackItemKind::RootMotionKey:
        found = std::any_of(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [id](const SpriteRootMotionKey& item) { return item.id == id; });
        break;
    }
    if (!found) return false;
    document_.selectedTrack = SpriteTrackSelection{kind, id};
    return true;
}

bool SpriteAuthoringSession::update_combat_window_by_id(
    std::string_view clipName, SpriteTrackId id, SpriteCombatWindow replacement,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Combat Window", [name, id, replacement = std::move(replacement)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [id](const SpriteCombatWindow& item) { return item.id == id; });
        if (found == clip->combatWindows.end())
            return fail(local, "sprite combat window ID does not exist");
        replacement.id = id;
        *found = std::move(replacement);
        return true;
    }, error);
}

bool SpriteAuthoringSession::remove_track_by_id(
    std::string_view clipName, SpriteTrackItemKind kind, SpriteTrackId id,
    std::string* error) {
    const std::string name(clipName);
    const bool removed = mutate("Remove Sprite Track Item", [name, kind, id](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        switch (kind) {
        case SpriteTrackItemKind::CombatWindow: {
            const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
                [id](const SpriteCombatWindow& item) { return item.id == id; });
            if (found == clip->combatWindows.end())
                return fail(local, "sprite combat window ID does not exist");
            clip->combatWindows.erase(found);
            return true;
        }
        case SpriteTrackItemKind::SocketKey: {
            const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
                [id](const SpriteSocketKey& item) { return item.id == id; });
            if (found == clip->socketKeys.end())
                return fail(local, "sprite socket key ID does not exist");
            clip->socketKeys.erase(found);
            return true;
        }
        case SpriteTrackItemKind::PropertyKey: {
            const auto found = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
                [id](const SpritePropertyKey& item) { return item.id == id; });
            if (found == clip->propertyKeys.end())
                return fail(local, "sprite property key ID does not exist");
            clip->propertyKeys.erase(found);
            return true;
        }
        case SpriteTrackItemKind::RootMotionKey: {
            const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
                [id](const SpriteRootMotionKey& item) { return item.id == id; });
            if (found == clip->rootMotionKeys.end())
                return fail(local, "sprite root-motion key ID does not exist");
            clip->rootMotionKeys.erase(found);
            return true;
        }
        }
        return fail(local, "sprite track item type is invalid");
    }, error);
    if (removed && document_.selectedTrack && document_.selectedTrack->id == id)
        document_.selectedTrack.reset();
    return removed;
}

bool SpriteAuthoringSession::translate_combat_window(
    std::string_view clipName, SpriteTrackId id, SpriteVec2 deltaPixels,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Translate Sprite Combat Window", [name, id, deltaPixels](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [id](const SpriteCombatWindow& item) { return item.id == id; });
        if (found == clip->combatWindows.end())
            return fail(local, "sprite combat window ID does not exist");
        found->volume.centerPixels.x += deltaPixels.x;
        found->volume.centerPixels.y += deltaPixels.y;
        return true;
    }, error);
}

bool SpriteAuthoringSession::resize_combat_window(
    std::string_view clipName, SpriteTrackId id, SpriteVec2 sizePixels,
    float radiusPixels, std::string* error) {
    const std::string name(clipName);
    return mutate("Resize Sprite Combat Window", [name, id, sizePixels, radiusPixels](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [id](const SpriteCombatWindow& item) { return item.id == id; });
        if (found == clip->combatWindows.end())
            return fail(local, "sprite combat window ID does not exist");
        found->volume.sizePixels = sizePixels;
        found->volume.radiusPixels = radiusPixels;
        if (found->volume.shape == SpriteCombatShape::Circle) {
            found->volume.sizePixels = {radiusPixels * 2.0F, radiusPixels * 2.0F};
        }
        return true;
    }, error);
}

bool SpriteAuthoringSession::rotate_combat_window(
    std::string_view clipName, SpriteTrackId id, float rotationDegrees,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Rotate Sprite Combat Window", [name, id, rotationDegrees](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [id](const SpriteCombatWindow& item) { return item.id == id; });
        if (found == clip->combatWindows.end())
            return fail(local, "sprite combat window ID does not exist");
        found->volume.rotationDegrees = rotationDegrees;
        return true;
    }, error);
}

bool SpriteAuthoringSession::set_combat_window_range(
    std::string_view clipName, SpriteTrackId id, std::size_t firstSequenceIndex,
    std::size_t lastSequenceIndex, std::string* error) {
    const std::string name(clipName);
    return mutate("Set Sprite Combat Window Range", [name, id, firstSequenceIndex, lastSequenceIndex](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
            [id](const SpriteCombatWindow& item) { return item.id == id; });
        if (found == clip->combatWindows.end())
            return fail(local, "sprite combat window ID does not exist");
        found->firstSequenceIndex = firstSequenceIndex;
        found->lastSequenceIndex = lastSequenceIndex;
        return true;
    }, error);
}

bool SpriteAuthoringSession::update_socket_key_by_id(
    std::string_view clipName, SpriteTrackId id, SpriteSocketKey replacement,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Socket", [name, id, replacement = std::move(replacement)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
            [id](const SpriteSocketKey& item) { return item.id == id; });
        if (found == clip->socketKeys.end()) return fail(local, "sprite socket key ID does not exist");
        replacement.id = id;
        *found = std::move(replacement);
        return true;
    }, error);
}

bool SpriteAuthoringSession::update_property_key_by_id(
    std::string_view clipName, SpriteTrackId id, SpritePropertyKey replacement,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Property", [name, id, replacement = std::move(replacement)](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
            [id](const SpritePropertyKey& item) { return item.id == id; });
        if (found == clip->propertyKeys.end()) return fail(local, "sprite property key ID does not exist");
        replacement.id = id;
        *found = std::move(replacement);
        return true;
    }, error);
}

bool SpriteAuthoringSession::update_root_motion_key_by_id(
    std::string_view clipName, SpriteTrackId id, SpriteRootMotionKey replacement,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Edit Sprite Root Motion", [name, id, replacement](
        SpriteAsset& asset, std::string* local) mutable {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
            [id](const SpriteRootMotionKey& item) { return item.id == id; });
        if (found == clip->rootMotionKeys.end())
            return fail(local, "sprite root-motion key ID does not exist");
        replacement.id = id;
        *found = replacement;
        return true;
    }, error);
}

bool SpriteAuthoringSession::duplicate_track_item(
    std::string_view clipName, SpriteTrackItemKind kind, SpriteTrackId id,
    std::ptrdiff_t sequenceOffset, SpriteTrackId* duplicateId, std::string* error) {
    const std::string name(clipName);
    SpriteTrackId created = kInvalidSpriteTrackId;
    const bool result = mutate("Duplicate Sprite Track Item", [name, kind, id, sequenceOffset, &created](
        SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        created = next_sprite_track_id(asset);
        const auto shifted = [sequenceOffset, clip](std::size_t value, std::size_t& output) {
            const std::ptrdiff_t candidate = static_cast<std::ptrdiff_t>(value) + sequenceOffset;
            if (candidate < 0 || static_cast<std::size_t>(candidate) >= clip->frames.size()) return false;
            output = static_cast<std::size_t>(candidate);
            return true;
        };
        switch (kind) {
        case SpriteTrackItemKind::CombatWindow: {
            const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
                [id](const SpriteCombatWindow& item) { return item.id == id; });
            if (found == clip->combatWindows.end()) return fail(local, "sprite combat window ID does not exist");
            SpriteCombatWindow copy = *found;
            if (!shifted(copy.firstSequenceIndex, copy.firstSequenceIndex) ||
                !shifted(copy.lastSequenceIndex, copy.lastSequenceIndex))
                return fail(local, "duplicated combat range falls outside the clip");
            copy.id = created;
            copy.volume.name += "_copy";
            clip->combatWindows.push_back(std::move(copy));
            return true;
        }
        case SpriteTrackItemKind::SocketKey: {
            const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
                [id](const SpriteSocketKey& item) { return item.id == id; });
            if (found == clip->socketKeys.end()) return fail(local, "sprite socket key ID does not exist");
            SpriteSocketKey copy = *found;
            if (!shifted(copy.sequenceIndex, copy.sequenceIndex))
                return fail(local, "duplicated socket key falls outside the clip");
            copy.id = created;
            copy.name += "_copy";
            clip->socketKeys.push_back(std::move(copy));
            return true;
        }
        case SpriteTrackItemKind::PropertyKey: {
            const auto found = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
                [id](const SpritePropertyKey& item) { return item.id == id; });
            if (found == clip->propertyKeys.end()) return fail(local, "sprite property key ID does not exist");
            SpritePropertyKey copy = *found;
            if (!shifted(copy.sequenceIndex, copy.sequenceIndex))
                return fail(local, "duplicated property key falls outside the clip");
            copy.id = created;
            copy.name += "_copy";
            clip->propertyKeys.push_back(std::move(copy));
            return true;
        }
        case SpriteTrackItemKind::RootMotionKey: {
            const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
                [id](const SpriteRootMotionKey& item) { return item.id == id; });
            if (found == clip->rootMotionKeys.end()) return fail(local, "sprite root-motion key ID does not exist");
            SpriteRootMotionKey copy = *found;
            if (!shifted(copy.sequenceIndex, copy.sequenceIndex))
                return fail(local, "duplicated root-motion key falls outside the clip");
            copy.id = created;
            clip->rootMotionKeys.push_back(copy);
            return true;
        }
        }
        return fail(local, "sprite track item type is invalid");
    }, error);
    if (result) {
        if (duplicateId) *duplicateId = created;
        document_.selectedTrack = SpriteTrackSelection{kind, created};
    }
    return result;
}

bool SpriteAuthoringSession::mirror_track_item_x(
    std::string_view clipName, SpriteTrackItemKind kind, SpriteTrackId id,
    std::string* error) {
    const std::string name(clipName);
    return mutate("Mirror Sprite Track Item", [name, kind, id](SpriteAsset& asset, std::string* local) {
        SpriteClip* clip = find_clip(asset, name);
        if (clip == nullptr) return fail(local, "sprite clip does not exist");
        switch (kind) {
        case SpriteTrackItemKind::CombatWindow: {
            const auto found = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
                [id](const SpriteCombatWindow& item) { return item.id == id; });
            if (found == clip->combatWindows.end()) return fail(local, "sprite combat window ID does not exist");
            found->volume.centerPixels.x = -found->volume.centerPixels.x;
            found->volume.rotationDegrees = -found->volume.rotationDegrees;
            found->volume.knockbackPixelsPerSecond.x = -found->volume.knockbackPixelsPerSecond.x;
            return true;
        }
        case SpriteTrackItemKind::SocketKey: {
            const auto found = std::find_if(clip->socketKeys.begin(), clip->socketKeys.end(),
                [id](const SpriteSocketKey& item) { return item.id == id; });
            if (found == clip->socketKeys.end()) return fail(local, "sprite socket key ID does not exist");
            found->positionPixels.x = -found->positionPixels.x;
            found->rotationDegrees = -found->rotationDegrees;
            return true;
        }
        case SpriteTrackItemKind::PropertyKey: {
            const auto found = std::find_if(clip->propertyKeys.begin(), clip->propertyKeys.end(),
                [id](const SpritePropertyKey& item) { return item.id == id; });
            if (found == clip->propertyKeys.end()) return fail(local, "sprite property key ID does not exist");
            if (found->value.type != SpritePropertyType::Vec2)
                return fail(local, "only Vec2 sprite properties can be mirrored");
            found->value.vec2Value.x = -found->value.vec2Value.x;
            return true;
        }
        case SpriteTrackItemKind::RootMotionKey: {
            const auto found = std::find_if(clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
                [id](const SpriteRootMotionKey& item) { return item.id == id; });
            if (found == clip->rootMotionKeys.end()) return fail(local, "sprite root-motion key ID does not exist");
            found->deltaPixels.x = -found->deltaPixels.x;
            found->rotationDegrees = -found->rotationDegrees;
            return true;
        }
        }
        return fail(local, "sprite track item type is invalid");
    }, error);
}

bool SpriteAuthoringSession::select_clip(std::string_view clipName) noexcept {
    if (find_sprite_clip(document_.asset, clipName) == nullptr) return false;
    document_.selectedClip.assign(clipName);
    document_.selectedTrack.reset();
    previewTimeSeconds_ = 0.0F;
    return true;
}

bool SpriteAuthoringSession::select_frame(SpriteFrameIndex frame) noexcept {
    if (frame >= document_.asset.frames.size()) return false;
    document_.selectedFrame = frame;
    previewPlaying_ = false;
    return true;
}

bool SpriteAuthoringSession::set_preview_playing(bool playing) noexcept {
    const bool changed = previewPlaying_ != playing;
    previewPlaying_ = playing;
    return changed;
}

bool SpriteAuthoringSession::seek_preview(float timeSeconds) noexcept {
    if (!std::isfinite(timeSeconds) || timeSeconds < 0.0F) return false;
    previewTimeSeconds_ = timeSeconds;
    return true;
}

bool SpriteAuthoringSession::tick_preview(float deltaSeconds) noexcept {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F) return false;
    if (!previewPlaying_) return true;
    if (previewTimeSeconds_ > 1000000.0F) previewTimeSeconds_ = 0.0F;
    previewTimeSeconds_ += deltaSeconds;
    return std::isfinite(previewTimeSeconds_);
}

std::optional<SpriteAuthoringPreview> SpriteAuthoringSession::preview(
    std::string* error) const {
    SpriteAsset previewAsset = document_.asset;
    std::string previewClip = document_.selectedClip;
    float previewTime = previewTimeSeconds_;
    if (!previewPlaying_ && document_.selectedFrame) {
        previewClip = "__dve_editor_selected_frame";
        while (find_sprite_clip(previewAsset, previewClip) != nullptr) previewClip.push_back('_');
        previewAsset.clips.push_back({previewClip, SpriteLoopMode::Once, 1.0F,
            {*document_.selectedFrame}});
        previewAsset.recompute_hash();
        previewTime = 0.0F;
    }
    const auto sample = sample_sprite_clip(
        previewAsset, previewClip, previewTime);
    if (!sample) {
        fail(error, "sprite authoring preview could not sample the selected clip");
        return std::nullopt;
    }
    SpriteRuntime runtime;
    if (!runtime.register_asset(1U, std::move(previewAsset), error)) return std::nullopt;
    SpriteInstanceDesc instance;
    instance.asset = 1U;
    instance.clip = previewClip;
    instance.pixelSnap = false;
    if (!runtime.bind(1U, instance, error) || !runtime.seek(1U, previewTime))
        return std::nullopt;
    SpriteAuthoringPreview result;
    result.sample = *sample;
    result.frame = document_.asset.frames[sample->frame];
    result.renderList = runtime.build_render_list();
    result.clip = document_.selectedClip;
    result.timeSeconds = previewTimeSeconds_;
    result.playing = previewPlaying_;
    return result;
}

std::string_view SpriteAuthoringSession::undo_label() const noexcept {
    return can_undo() ? undo_.back().label : std::string_view{};
}

std::string_view SpriteAuthoringSession::redo_label() const noexcept {
    return can_redo() ? redo_.back().label : std::string_view{};
}

bool SpriteAuthoringSession::undo() noexcept {
    if (!can_undo()) return false;
    HistoryEntry entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({entry.label, document_});
    const std::uint64_t nextRevision = document_.revision + 1U;
    document_ = std::move(entry.state);
    document_.revision = nextRevision;
    normalize_selection();
    return true;
}

bool SpriteAuthoringSession::redo() noexcept {
    if (!can_redo()) return false;
    HistoryEntry entry = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({entry.label, document_});
    const std::uint64_t nextRevision = document_.revision + 1U;
    document_ = std::move(entry.state);
    document_.revision = nextRevision;
    normalize_selection();
    return true;
}

void SpriteAuthoringSession::clear_history() noexcept {
    undo_.clear();
    redo_.clear();
}

} // namespace dve::editor
