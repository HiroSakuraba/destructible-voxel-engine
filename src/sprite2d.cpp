#include "dve/sprite2d.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <queue>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace dve {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint32_t kMaximumTextureDimension = 16384U;
constexpr std::size_t kMaximumFrames = 65536U;
constexpr std::size_t kMaximumClips = 4096U;
constexpr std::size_t kMaximumTrackEntries = 262144U;
constexpr float kMaximumTrackMagnitude = 1000000.0F;
constexpr float kDegreesToRadians = 0.01745329251994329577F;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

[[nodiscard]] bool finite(SpriteVec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finite(Quaternion value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] bool finite_transform(const RigidTransform& value) noexcept {
    if (!finite(value.position) || !finite(value.rotation)) return false;
    const float normSquared = value.rotation.x * value.rotation.x +
        value.rotation.y * value.rotation.y + value.rotation.z * value.rotation.z +
        value.rotation.w * value.rotation.w;
    return std::isfinite(normSquared) && normSquared > 1.0e-12F;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template<class T> void hash_integer(std::uint64_t& hash, T value) noexcept {
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        hash_byte(hash, static_cast<std::uint8_t>(
            (bits >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
    }
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (const char raw : value)
        hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(raw)));
}

[[nodiscard]] bool write_atomic(
    const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create sprite asset directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary sprite asset");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete sprite asset");
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (!ec) return true;
    std::filesystem::remove(temporary, ec);
    return fail(error, "could not publish sprite asset: " + ec.message());
}

[[nodiscard]] std::size_t sequence_count(const SpriteClip& clip) noexcept {
    if (clip.loopMode != SpriteLoopMode::PingPong || clip.frames.size() <= 1U)
        return clip.frames.size();
    return clip.frames.size() * 2U - 2U;
}

[[nodiscard]] SpriteFrameIndex sequence_frame(
    const SpriteClip& clip, std::size_t sequenceIndex) noexcept {
    if (clip.loopMode != SpriteLoopMode::PingPong || clip.frames.size() <= 1U ||
        sequenceIndex < clip.frames.size()) return clip.frames[sequenceIndex];
    return clip.frames[clip.frames.size() * 2U - 2U - sequenceIndex];
}

[[nodiscard]] float sequence_duration(
    const SpriteAsset& asset, const SpriteClip& clip) noexcept {
    float duration = 0.0F;
    for (std::size_t index = 0U; index < sequence_count(clip); ++index)
        duration += asset.frames[sequence_frame(clip, index)].durationSeconds;
    return duration;
}

[[nodiscard]] bool same_batch(const SpriteDrawItem& item, const SpriteBatch& batch) noexcept {
    return item.textureAsset == batch.textureAsset && item.materialId == batch.materialId &&
        item.paletteBank == batch.paletteBank && item.paletteAsset == batch.paletteAsset &&
        item.paletteStateHash == batch.paletteStateHash &&
        item.palettePacket == batch.palettePacket && item.sampling == batch.sampling &&
        item.blendMode == batch.blendMode;
}

[[nodiscard]] bool same_palette_packet(
    const SpritePalettePacket& left, const SpritePalettePacket& right) noexcept {
    return left.paletteAsset == right.paletteAsset && left.bank == right.bank &&
        left.paletteContentHash == right.paletteContentHash &&
        left.stateHash == right.stateHash && left.colors == right.colors;
}

[[nodiscard]] std::uint64_t sprite_asset_content_hash_v1(const SpriteAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, asset.name);
    hash_string(hash, asset.textureAsset);
    hash_integer(hash, asset.textureWidth);
    hash_integer(hash, asset.textureHeight);
    hash_float(hash, asset.pixelsPerWorldUnit);
    hash_byte(hash, static_cast<std::uint8_t>(asset.sampling));
    hash_integer(hash, asset.materialId);
    hash_integer(hash, asset.paletteBank);
    hash_integer(hash, static_cast<std::uint64_t>(asset.frames.size()));
    for (const SpriteFrame& frame : asset.frames) {
        hash_string(hash, frame.name);
        hash_integer(hash, frame.atlasRect.x); hash_integer(hash, frame.atlasRect.y);
        hash_integer(hash, frame.atlasRect.width); hash_integer(hash, frame.atlasRect.height);
        hash_integer(hash, frame.sourceWidth); hash_integer(hash, frame.sourceHeight);
        hash_integer(hash, frame.sourceOffsetX); hash_integer(hash, frame.sourceOffsetY);
        hash_float(hash, frame.pivotPixels.x); hash_float(hash, frame.pivotPixels.y);
        hash_float(hash, frame.durationSeconds); hash_string(hash, frame.event);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.clips.size()));
    for (const SpriteClip& clip : asset.clips) {
        hash_string(hash, clip.name);
        hash_byte(hash, static_cast<std::uint8_t>(clip.loopMode));
        hash_float(hash, clip.playbackRate);
        hash_integer(hash, static_cast<std::uint64_t>(clip.frames.size()));
        for (const SpriteFrameIndex frame : clip.frames) hash_integer(hash, frame);
    }
    return hash;
}


[[nodiscard]] std::uint64_t sprite_asset_content_hash_v2(const SpriteAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, asset.name);
    hash_string(hash, asset.textureAsset);
    hash_integer(hash, asset.textureWidth);
    hash_integer(hash, asset.textureHeight);
    hash_float(hash, asset.pixelsPerWorldUnit);
    hash_byte(hash, static_cast<std::uint8_t>(asset.sampling));
    hash_integer(hash, asset.materialId);
    hash_string(hash, asset.paletteAsset);
    hash_integer(hash, asset.paletteBank);
    hash_integer(hash, static_cast<std::uint64_t>(asset.frames.size()));
    for (const SpriteFrame& frame : asset.frames) {
        hash_string(hash, frame.name);
        hash_integer(hash, frame.atlasRect.x); hash_integer(hash, frame.atlasRect.y);
        hash_integer(hash, frame.atlasRect.width); hash_integer(hash, frame.atlasRect.height);
        hash_integer(hash, frame.sourceWidth); hash_integer(hash, frame.sourceHeight);
        hash_integer(hash, frame.sourceOffsetX); hash_integer(hash, frame.sourceOffsetY);
        hash_float(hash, frame.pivotPixels.x); hash_float(hash, frame.pivotPixels.y);
        hash_float(hash, frame.durationSeconds); hash_string(hash, frame.event);
    }
    hash_integer(hash, static_cast<std::uint64_t>(asset.clips.size()));
    for (const SpriteClip& clip : asset.clips) {
        hash_string(hash, clip.name);
        hash_byte(hash, static_cast<std::uint8_t>(clip.loopMode));
        hash_float(hash, clip.playbackRate);
        hash_integer(hash, static_cast<std::uint64_t>(clip.frames.size()));
        for (const SpriteFrameIndex frame : clip.frames) hash_integer(hash, frame);
    }
    return hash;
}

void hash_property_value(std::uint64_t& hash, const SpritePropertyValue& value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(value.type));
    hash_byte(hash, value.booleanValue ? 1U : 0U);
    hash_integer(hash, value.integerValue);
    hash_float(hash, value.floatValue);
    hash_float(hash, value.vec2Value.x);
    hash_float(hash, value.vec2Value.y);
    hash_string(hash, value.stringValue);
}

[[nodiscard]] std::size_t authored_sequence_index(
    const SpriteClip& clip, std::size_t playbackSequenceIndex) noexcept {
    if (clip.loopMode != SpriteLoopMode::PingPong || clip.frames.size() <= 1U ||
        playbackSequenceIndex < clip.frames.size()) return playbackSequenceIndex;
    return clip.frames.size() * 2U - 2U - playbackSequenceIndex;
}

[[nodiscard]] std::size_t next_playback_sequence_index(
    const SpriteClip& clip, std::size_t playbackSequenceIndex, bool finished) noexcept {
    const std::size_t count = sequence_count(clip);
    if (count <= 1U || finished) return playbackSequenceIndex;
    if (playbackSequenceIndex + 1U < count) return playbackSequenceIndex + 1U;
    return clip.loopMode == SpriteLoopMode::Once ? playbackSequenceIndex : 0U;
}

[[nodiscard]] float lerp(float left, float right, float alpha) noexcept {
    return left + (right - left) * alpha;
}

[[nodiscard]] SpriteVec2 lerp(SpriteVec2 left, SpriteVec2 right, float alpha) noexcept {
    return {lerp(left.x, right.x, alpha), lerp(left.y, right.y, alpha)};
}

[[nodiscard]] SpritePropertyValue interpolate_property(
    const SpritePropertyValue& left,
    const SpritePropertyValue& right,
    float alpha) noexcept {
    if (left.type != right.type) return left;
    SpritePropertyValue result = left;
    if (left.type == SpritePropertyType::Float) {
        result.floatValue = lerp(left.floatValue, right.floatValue, alpha);
    } else if (left.type == SpritePropertyType::Vec2) {
        result.vec2Value = lerp(left.vec2Value, right.vec2Value, alpha);
    }
    return result;
}

[[nodiscard]] float mirrored_rotation(float degrees, bool flipX, bool flipY) noexcept {
    float result = degrees;
    if (flipX) result = 180.0F - result;
    if (flipY) result = -result;
    return result;
}

[[nodiscard]] SpriteVec2 mirrored_point(SpriteVec2 value, bool flipX, bool flipY) noexcept {
    if (flipX) value.x = -value.x;
    if (flipY) value.y = -value.y;
    return value;
}

[[nodiscard]] RigidTransform local_2d_transform(
    SpriteVec2 pixels,
    float rotationDegrees,
    float pixelsPerWorldUnit,
    GameplayPlane2D plane) noexcept {
    const float radians = rotationDegrees * kDegreesToRadians;
    if (plane == GameplayPlane2D::XY) {
        return make_rigid_transform(
            {pixels.x / pixelsPerWorldUnit, pixels.y / pixelsPerWorldUnit, 0.0F},
            quaternion_from_axis_angle({0.0F, 0.0F, 1.0F}, radians));
    }
    return make_rigid_transform(
        {pixels.x / pixelsPerWorldUnit, 0.0F, pixels.y / pixelsPerWorldUnit},
        quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, radians));
}


[[nodiscard]] SpriteTrackId deterministic_track_id(
    std::string_view clipName, std::uint8_t kind, std::size_t ordinal,
    const std::set<SpriteTrackId>& used) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, clipName);
    hash_byte(hash, kind);
    hash_integer(hash, static_cast<std::uint64_t>(ordinal));
    if (hash == kInvalidSpriteTrackId) hash = 1U;
    while (used.contains(hash) || hash == kInvalidSpriteTrackId) {
        hash ^= 0x9E3779B97F4A7C15ULL;
        hash *= kFnvPrime;
        if (hash == kInvalidSpriteTrackId) hash = 1U;
    }
    return hash;
}

[[nodiscard]] const SpriteRootMotionKey* root_key_at(
    const SpriteClip& clip, std::size_t authoredSequenceIndex) noexcept {
    const auto found = std::find_if(clip.rootMotionKeys.begin(), clip.rootMotionKeys.end(),
        [authoredSequenceIndex](const SpriteRootMotionKey& key) {
            return key.sequenceIndex == authoredSequenceIndex;
        });
    return found == clip.rootMotionKeys.end() ? nullptr : &*found;
}

[[nodiscard]] Float3 root_pixels_to_world(
    SpriteVec2 pixels, float pixelsPerWorldUnit, GameplayPlane2D plane) noexcept {
    if (plane == GameplayPlane2D::XY)
        return {pixels.x / pixelsPerWorldUnit, pixels.y / pixelsPerWorldUnit, 0.0F};
    return {pixels.x / pixelsPerWorldUnit, 0.0F, pixels.y / pixelsPerWorldUnit};
}

} // namespace

bool PixelPresentationConfig::validate(std::string* error) const {
    if (logicalWidth == 0U || logicalHeight == 0U ||
        logicalWidth > kMaximumTextureDimension || logicalHeight > kMaximumTextureDimension)
        return fail(error, "logical pixel resolution is out of range");
    if (static_cast<unsigned>(scaleMode) > static_cast<unsigned>(PixelScaleMode::IntegerFill))
        return fail(error, "pixel scale mode is invalid");
    if (!std::isfinite(pixelsPerWorldUnit) || pixelsPerWorldUnit <= 0.0F ||
        pixelsPerWorldUnit > 65536.0F)
        return fail(error, "pixels-per-world-unit is invalid");
    return true;
}

bool PixelPresentationLayout::contains_output_pixel(SpriteVec2 outputPixel) const noexcept {
    if (!finite(outputPixel)) return false;
    return outputPixel.x >= static_cast<float>(viewportX) &&
        outputPixel.y >= static_cast<float>(viewportY) &&
        outputPixel.x < static_cast<float>(viewportX) + static_cast<float>(viewportWidth) &&
        outputPixel.y < static_cast<float>(viewportY) + static_cast<float>(viewportHeight);
}

std::optional<PixelPresentationLayout> compute_pixel_presentation(
    const PixelPresentationConfig& config,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight,
    std::string* error) {
    if (!config.validate(error)) return std::nullopt;
    if (outputWidth == 0U || outputHeight == 0U ||
        outputWidth > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        outputHeight > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        fail(error, "output pixel resolution is empty");
        return std::nullopt;
    }
    const float ratioX = static_cast<float>(outputWidth) /
        static_cast<float>(config.logicalWidth);
    const float ratioY = static_cast<float>(outputHeight) /
        static_cast<float>(config.logicalHeight);
    PixelPresentationLayout result;
    result.outputWidth = outputWidth;
    result.outputHeight = outputHeight;

    if (config.scaleMode == PixelScaleMode::Stretch) {
        result.viewportWidth = outputWidth;
        result.viewportHeight = outputHeight;
        result.scaleX = ratioX;
        result.scaleY = ratioY;
    } else {
        float scale = std::min(ratioX, ratioY);
        if (config.scaleMode == PixelScaleMode::IntegerFit) {
            const float integer = std::floor(scale);
            if (integer >= 1.0F) {
                scale = integer;
                result.integerScale = true;
            } else if (!config.allowFractionalDownscale) {
                fail(error, "output is smaller than the logical resolution");
                return std::nullopt;
            }
        } else if (config.scaleMode == PixelScaleMode::IntegerFill) {
            scale = std::max(1.0F, std::ceil(std::max(ratioX, ratioY)));
            result.integerScale = true;
            result.cropped = true;
        }
        const double scaledWidth = static_cast<double>(config.logicalWidth) * scale;
        const double scaledHeight = static_cast<double>(config.logicalHeight) * scale;
        if (scaledWidth > static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
            scaledHeight > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            fail(error, "scaled pixel viewport is too large");
            return std::nullopt;
        }
        result.viewportWidth = std::max(1U, static_cast<std::uint32_t>(std::lround(scaledWidth)));
        result.viewportHeight = std::max(1U, static_cast<std::uint32_t>(std::lround(scaledHeight)));
        result.scaleX = static_cast<float>(result.viewportWidth) /
            static_cast<float>(config.logicalWidth);
        result.scaleY = static_cast<float>(result.viewportHeight) /
            static_cast<float>(config.logicalHeight);
    }
    result.viewportX = static_cast<std::int32_t>((static_cast<std::int64_t>(outputWidth) -
        static_cast<std::int64_t>(result.viewportWidth)) / 2);
    result.viewportY = static_cast<std::int32_t>((static_cast<std::int64_t>(outputHeight) -
        static_cast<std::int64_t>(result.viewportHeight)) / 2);
    result.cropped = result.cropped || result.viewportX < 0 || result.viewportY < 0;
    return result;
}

std::optional<SpriteVec2> output_to_logical_pixel(
    const PixelPresentationConfig& config,
    const PixelPresentationLayout& layout,
    SpriteVec2 outputPixel,
    bool clampToViewport) noexcept {
    if (!finite(outputPixel) || layout.scaleX <= 0.0F || layout.scaleY <= 0.0F)
        return std::nullopt;
    if (!layout.contains_output_pixel(outputPixel) && !clampToViewport) return std::nullopt;
    const float minimumX = static_cast<float>(layout.viewportX);
    const float minimumY = static_cast<float>(layout.viewportY);
    const float maximumX = minimumX + static_cast<float>(layout.viewportWidth);
    const float maximumY = minimumY + static_cast<float>(layout.viewportHeight);
    if (clampToViewport) {
        outputPixel.x = std::clamp(outputPixel.x, minimumX, std::nextafter(maximumX, minimumX));
        outputPixel.y = std::clamp(outputPixel.y, minimumY, std::nextafter(maximumY, minimumY));
    }
    return SpriteVec2{
        std::clamp((outputPixel.x - minimumX) / layout.scaleX, 0.0F,
            std::nextafter(static_cast<float>(config.logicalWidth), 0.0F)),
        std::clamp((outputPixel.y - minimumY) / layout.scaleY, 0.0F,
            std::nextafter(static_cast<float>(config.logicalHeight), 0.0F)),
    };
}

SpriteVec2 logical_to_output_pixel(
    const PixelPresentationLayout& layout,
    SpriteVec2 logicalPixel) noexcept {
    return {
        static_cast<float>(layout.viewportX) + logicalPixel.x * layout.scaleX,
        static_cast<float>(layout.viewportY) + logicalPixel.y * layout.scaleY,
    };
}

std::optional<SpriteVec2> window_to_logical_pixel(
    const PixelPresentationConfig& config,
    const PixelPresentationLayout& layout,
    SpriteVec2 windowPixel,
    std::uint32_t windowWidth,
    std::uint32_t windowHeight,
    bool clampToViewport) noexcept {
    if (!finite(windowPixel) || windowWidth == 0U || windowHeight == 0U ||
        layout.outputWidth == 0U || layout.outputHeight == 0U) return std::nullopt;
    const SpriteVec2 outputPixel{
        windowPixel.x * static_cast<float>(layout.outputWidth) /
            static_cast<float>(windowWidth),
        windowPixel.y * static_cast<float>(layout.outputHeight) /
            static_cast<float>(windowHeight),
    };
    return output_to_logical_pixel(config, layout, outputPixel, clampToViewport);
}

Float3 snap_world_to_pixel(
    Float3 worldPosition,
    Float3 cameraOrigin,
    float pixelsPerWorldUnit,
    GameplayPlane2D plane) noexcept {
    if (!finite(worldPosition) || !finite(cameraOrigin) || !std::isfinite(pixelsPerWorldUnit) ||
        pixelsPerWorldUnit <= 0.0F) return worldPosition;
    const auto snap = [pixelsPerWorldUnit](float value, float origin) {
        return origin + std::round((value - origin) * pixelsPerWorldUnit) / pixelsPerWorldUnit;
    };
    worldPosition.x = snap(worldPosition.x, cameraOrigin.x);
    if (plane == GameplayPlane2D::XY)
        worldPosition.y = snap(worldPosition.y, cameraOrigin.y);
    else
        worldPosition.z = snap(worldPosition.z, cameraOrigin.z);
    return worldPosition;
}

bool SpriteAsset::validate(std::string* error) const {
    if (name.empty() || name.size() > 255U || textureAsset.empty() || textureAsset.size() > 4096U ||
        paletteAsset.size() > 4096U)
        return fail(error, "sprite asset name, texture reference, or palette reference is invalid");
    if (textureWidth == 0U || textureHeight == 0U ||
        textureWidth > kMaximumTextureDimension || textureHeight > kMaximumTextureDimension)
        return fail(error, "sprite texture dimensions are invalid");
    if (!std::isfinite(pixelsPerWorldUnit) || pixelsPerWorldUnit <= 0.0F ||
        pixelsPerWorldUnit > 65536.0F)
        return fail(error, "sprite pixels-per-world-unit is invalid");
    if (static_cast<unsigned>(sampling) > static_cast<unsigned>(SpriteSampling::Linear) ||
        (!paletteAsset.empty() && sampling != SpriteSampling::Nearest) ||
        frames.empty() || frames.size() > kMaximumFrames || clips.empty() ||
        clips.size() > kMaximumClips)
        return fail(error, "sprite sampling, indexed-palette mode, frame count, or clip count is invalid");

    std::set<std::string, std::less<>> frameNames;
    for (const SpriteFrame& frame : frames) {
        const std::uint64_t right = static_cast<std::uint64_t>(frame.atlasRect.x) +
            frame.atlasRect.width;
        const std::uint64_t bottom = static_cast<std::uint64_t>(frame.atlasRect.y) +
            frame.atlasRect.height;
        const std::int64_t sourceRight = static_cast<std::int64_t>(frame.sourceOffsetX) +
            frame.atlasRect.width;
        const std::int64_t sourceTop = static_cast<std::int64_t>(frame.sourceOffsetY) +
            frame.atlasRect.height;
        if (frame.name.empty() || frame.name.size() > 255U ||
            !frameNames.insert(frame.name).second || frame.atlasRect.width == 0U ||
            frame.atlasRect.height == 0U || right > textureWidth || bottom > textureHeight ||
            frame.sourceWidth == 0U || frame.sourceHeight == 0U ||
            frame.sourceOffsetX < 0 || frame.sourceOffsetY < 0 || sourceRight > frame.sourceWidth ||
            sourceTop > frame.sourceHeight || !finite(frame.pivotPixels) ||
            !std::isfinite(frame.durationSeconds) || frame.durationSeconds <= 0.0F ||
            frame.durationSeconds > 3600.0F || frame.event.size() > 255U)
            return fail(error, "sprite frame is invalid");
    }

    std::set<std::string, std::less<>> clipNames;
    for (const SpriteClip& clip : clips) {
        if (clip.name.empty() || clip.name.size() > 255U || !clipNames.insert(clip.name).second ||
            static_cast<unsigned>(clip.loopMode) > static_cast<unsigned>(SpriteLoopMode::PingPong) ||
            !std::isfinite(clip.playbackRate) || clip.playbackRate <= 0.0F ||
            clip.playbackRate > 1024.0F || clip.frames.empty() || clip.frames.size() > kMaximumFrames ||
            clip.combatWindows.size() > kMaximumTrackEntries ||
            clip.socketKeys.size() > kMaximumTrackEntries ||
            clip.propertyKeys.size() > kMaximumTrackEntries ||
            clip.rootMotionKeys.size() > kMaximumTrackEntries)
            return fail(error, "sprite clip or metadata track count is invalid");
        for (const SpriteFrameIndex frame : clip.frames)
            if (frame >= frames.size()) return fail(error, "sprite clip references an invalid frame");

        std::set<SpriteTrackId> trackIds;
        for (const SpriteCombatWindow& window : clip.combatWindows) {
            const SpriteCombatVolume& volume = window.volume;
            if (window.id == kInvalidSpriteTrackId || !trackIds.insert(window.id).second ||
                window.firstSequenceIndex > window.lastSequenceIndex ||
                window.lastSequenceIndex >= clip.frames.size() || volume.name.empty() ||
                volume.name.size() > 255U ||
                static_cast<unsigned>(volume.role) >
                    static_cast<unsigned>(SpriteCombatRole::Trigger) ||
                static_cast<unsigned>(volume.shape) >
                    static_cast<unsigned>(SpriteCombatShape::Capsule) ||
                !finite(volume.centerPixels) || !finite(volume.sizePixels) ||
                volume.sizePixels.x <= 0.0F || volume.sizePixels.y <= 0.0F ||
                volume.sizePixels.x > kMaximumTrackMagnitude ||
                volume.sizePixels.y > kMaximumTrackMagnitude ||
                !std::isfinite(volume.radiusPixels) || volume.radiusPixels <= 0.0F ||
                volume.radiusPixels > kMaximumTrackMagnitude ||
                !std::isfinite(volume.rotationDegrees) ||
                std::abs(volume.rotationDegrees) > kMaximumTrackMagnitude ||
                !std::isfinite(volume.damage) || volume.damage < 0.0F ||
                volume.damage > kMaximumTrackMagnitude ||
                !finite(volume.knockbackPixelsPerSecond) ||
                std::abs(volume.knockbackPixelsPerSecond.x) > kMaximumTrackMagnitude ||
                std::abs(volume.knockbackPixelsPerSecond.y) > kMaximumTrackMagnitude ||
                volume.hitStopTicks > 86400000U) {
                return fail(error, "sprite combat window is invalid");
            }
            if (volume.shape == SpriteCombatShape::Circle &&
                (std::abs(volume.sizePixels.x - volume.radiusPixels * 2.0F) > 0.001F ||
                 std::abs(volume.sizePixels.y - volume.radiusPixels * 2.0F) > 0.001F)) {
                return fail(error, "sprite circle combat volume size must equal diameter");
            }
            if (volume.shape == SpriteCombatShape::Capsule &&
                volume.radiusPixels * 2.0F > std::min(volume.sizePixels.x, volume.sizePixels.y) + 0.001F) {
                return fail(error, "sprite capsule radius exceeds its bounds");
            }
        }

        std::set<std::pair<std::string, std::size_t>> socketCoordinates;
        for (const SpriteSocketKey& key : clip.socketKeys) {
            if (key.id == kInvalidSpriteTrackId || !trackIds.insert(key.id).second ||
                key.name.empty() || key.name.size() > 255U ||
                key.sequenceIndex >= clip.frames.size() ||
                !socketCoordinates.emplace(key.name, key.sequenceIndex).second ||
                !finite(key.positionPixels) || !std::isfinite(key.rotationDegrees) ||
                std::abs(key.rotationDegrees) > kMaximumTrackMagnitude || !finite(key.scale) ||
                key.scale.x == 0.0F || key.scale.y == 0.0F ||
                std::abs(key.scale.x) > 1024.0F || std::abs(key.scale.y) > 1024.0F ||
                static_cast<unsigned>(key.interpolation) >
                    static_cast<unsigned>(SpriteTrackInterpolation::Linear)) {
                return fail(error, "sprite socket key is invalid or duplicated");
            }
        }

        std::set<std::pair<std::string, std::size_t>> propertyCoordinates;
        for (const SpritePropertyKey& key : clip.propertyKeys) {
            const SpritePropertyValue& value = key.value;
            if (key.id == kInvalidSpriteTrackId || !trackIds.insert(key.id).second ||
                key.name.empty() || key.name.size() > 255U ||
                key.sequenceIndex >= clip.frames.size() ||
                !propertyCoordinates.emplace(key.name, key.sequenceIndex).second ||
                static_cast<unsigned>(key.interpolation) >
                    static_cast<unsigned>(SpriteTrackInterpolation::Linear) ||
                static_cast<unsigned>(value.type) >
                    static_cast<unsigned>(SpritePropertyType::AssetReference) ||
                !std::isfinite(value.floatValue) || !finite(value.vec2Value) ||
                std::abs(value.floatValue) > kMaximumTrackMagnitude ||
                std::abs(value.vec2Value.x) > kMaximumTrackMagnitude ||
                std::abs(value.vec2Value.y) > kMaximumTrackMagnitude ||
                value.stringValue.size() > 4096U) {
                return fail(error, "sprite property key is invalid or duplicated");
            }
            if (key.interpolation == SpriteTrackInterpolation::Linear &&
                value.type != SpritePropertyType::Float && value.type != SpritePropertyType::Vec2) {
                return fail(error, "only float and vec2 sprite properties support linear interpolation");
            }
        }

        std::set<std::size_t> rootCoordinates;
        for (const SpriteRootMotionKey& key : clip.rootMotionKeys) {
            if (key.id == kInvalidSpriteTrackId || !trackIds.insert(key.id).second ||
                key.sequenceIndex >= clip.frames.size() ||
                !rootCoordinates.insert(key.sequenceIndex).second || !finite(key.deltaPixels) ||
                std::abs(key.deltaPixels.x) > kMaximumTrackMagnitude ||
                std::abs(key.deltaPixels.y) > kMaximumTrackMagnitude ||
                !std::isfinite(key.rotationDegrees) ||
                std::abs(key.rotationDegrees) > kMaximumTrackMagnitude) {
                return fail(error, "sprite root-motion key is invalid or duplicated");
            }
        }
    }
    return true;
}

void assign_sprite_track_ids(SpriteAsset& asset) noexcept {
    std::set<SpriteTrackId> used;
    for (SpriteClip& clip : asset.clips) {
        for (const SpriteCombatWindow& window : clip.combatWindows)
            if (window.id != kInvalidSpriteTrackId) used.insert(window.id);
        for (const SpriteSocketKey& key : clip.socketKeys)
            if (key.id != kInvalidSpriteTrackId) used.insert(key.id);
        for (const SpritePropertyKey& key : clip.propertyKeys)
            if (key.id != kInvalidSpriteTrackId) used.insert(key.id);
        for (const SpriteRootMotionKey& key : clip.rootMotionKeys)
            if (key.id != kInvalidSpriteTrackId) used.insert(key.id);
    }
    for (SpriteClip& clip : asset.clips) {
        std::size_t ordinal = 0U;
        for (SpriteCombatWindow& window : clip.combatWindows) {
            if (window.id == kInvalidSpriteTrackId) {
                window.id = deterministic_track_id(clip.name, 1U, ordinal, used);
                used.insert(window.id);
            }
            ++ordinal;
        }
        ordinal = 0U;
        for (SpriteSocketKey& key : clip.socketKeys) {
            if (key.id == kInvalidSpriteTrackId) {
                key.id = deterministic_track_id(clip.name, 2U, ordinal, used);
                used.insert(key.id);
            }
            ++ordinal;
        }
        ordinal = 0U;
        for (SpritePropertyKey& key : clip.propertyKeys) {
            if (key.id == kInvalidSpriteTrackId) {
                key.id = deterministic_track_id(clip.name, 3U, ordinal, used);
                used.insert(key.id);
            }
            ++ordinal;
        }
        ordinal = 0U;
        for (SpriteRootMotionKey& key : clip.rootMotionKeys) {
            if (key.id == kInvalidSpriteTrackId) {
                key.id = deterministic_track_id(clip.name, 4U, ordinal, used);
                used.insert(key.id);
            }
            ++ordinal;
        }
    }
}

SpriteTrackId next_sprite_track_id(const SpriteAsset& asset) noexcept {
    SpriteTrackId maximum = 0U;
    for (const SpriteClip& clip : asset.clips) {
        for (const SpriteCombatWindow& item : clip.combatWindows) maximum = std::max(maximum, item.id);
        for (const SpriteSocketKey& item : clip.socketKeys) maximum = std::max(maximum, item.id);
        for (const SpritePropertyKey& item : clip.propertyKeys) maximum = std::max(maximum, item.id);
        for (const SpriteRootMotionKey& item : clip.rootMotionKeys) maximum = std::max(maximum, item.id);
    }
    return maximum == std::numeric_limits<SpriteTrackId>::max() ? 1U : maximum + 1U;
}

void SpriteAsset::recompute_hash() noexcept {
    assign_sprite_track_ids(*this);
    contentHash = sprite_asset_content_hash(*this);
}

[[nodiscard]] std::uint64_t sprite_asset_content_hash_v3(const SpriteAsset& asset) noexcept {
    std::uint64_t hash = sprite_asset_content_hash_v2(asset);
    for (const SpriteClip& clip : asset.clips) {
        hash_integer(hash, static_cast<std::uint64_t>(clip.combatWindows.size()));
        for (const SpriteCombatWindow& window : clip.combatWindows) {
            hash_integer(hash, static_cast<std::uint64_t>(window.firstSequenceIndex));
            hash_integer(hash, static_cast<std::uint64_t>(window.lastSequenceIndex));
            const SpriteCombatVolume& volume = window.volume;
            hash_string(hash, volume.name);
            hash_byte(hash, static_cast<std::uint8_t>(volume.role));
            hash_byte(hash, static_cast<std::uint8_t>(volume.shape));
            hash_float(hash, volume.centerPixels.x); hash_float(hash, volume.centerPixels.y);
            hash_float(hash, volume.sizePixels.x); hash_float(hash, volume.sizePixels.y);
            hash_float(hash, volume.radiusPixels); hash_float(hash, volume.rotationDegrees);
            hash_integer(hash, volume.attackId); hash_float(hash, volume.damage);
            hash_float(hash, volume.knockbackPixelsPerSecond.x);
            hash_float(hash, volume.knockbackPixelsPerSecond.y);
            hash_integer(hash, volume.hitStopTicks); hash_integer(hash, volume.priority);
            hash_integer(hash, volume.flags);
        }
        hash_integer(hash, static_cast<std::uint64_t>(clip.socketKeys.size()));
        for (const SpriteSocketKey& key : clip.socketKeys) {
            hash_string(hash, key.name);
            hash_integer(hash, static_cast<std::uint64_t>(key.sequenceIndex));
            hash_float(hash, key.positionPixels.x); hash_float(hash, key.positionPixels.y);
            hash_float(hash, key.rotationDegrees);
            hash_float(hash, key.scale.x); hash_float(hash, key.scale.y);
            hash_byte(hash, static_cast<std::uint8_t>(key.interpolation));
        }
        hash_integer(hash, static_cast<std::uint64_t>(clip.propertyKeys.size()));
        for (const SpritePropertyKey& key : clip.propertyKeys) {
            hash_string(hash, key.name);
            hash_integer(hash, static_cast<std::uint64_t>(key.sequenceIndex));
            hash_byte(hash, static_cast<std::uint8_t>(key.interpolation));
            hash_property_value(hash, key.value);
        }
        hash_integer(hash, static_cast<std::uint64_t>(clip.rootMotionKeys.size()));
        for (const SpriteRootMotionKey& key : clip.rootMotionKeys) {
            hash_integer(hash, static_cast<std::uint64_t>(key.sequenceIndex));
            hash_float(hash, key.deltaPixels.x); hash_float(hash, key.deltaPixels.y);
            hash_float(hash, key.rotationDegrees);
        }
    }
    return hash;
}

std::uint64_t sprite_asset_content_hash(const SpriteAsset& asset) noexcept {
    std::uint64_t hash = sprite_asset_content_hash_v3(asset);
    for (const SpriteClip& clip : asset.clips) {
        for (const SpriteCombatWindow& item : clip.combatWindows) hash_integer(hash, item.id);
        for (const SpriteSocketKey& item : clip.socketKeys) hash_integer(hash, item.id);
        for (const SpritePropertyKey& item : clip.propertyKeys) hash_integer(hash, item.id);
        for (const SpriteRootMotionKey& item : clip.rootMotionKeys) hash_integer(hash, item.id);
    }
    return hash;
}

const SpriteClip* find_sprite_clip(
    const SpriteAsset& asset, std::string_view clipName) noexcept {
    const auto found = std::find_if(asset.clips.begin(), asset.clips.end(),
        [clipName](const SpriteClip& clip) { return clip.name == clipName; });
    return found == asset.clips.end() ? nullptr : &*found;
}

bool write_dvesprite(
    const std::filesystem::path& path,
    const SpriteAsset& asset,
    std::string* error) {
    SpriteAsset normalized = asset;
    assign_sprite_track_ids(normalized);
    if (!normalized.validate(error)) return false;
    const std::uint64_t hash = sprite_asset_content_hash(normalized);
    if (asset.contentHash != 0U && asset.contentHash != hash)
        return fail(error, "sprite asset content hash is stale");
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    stream << "DVE_SPRITE 4\n";
    stream << "asset " << std::quoted(normalized.name) << ' ' << std::quoted(normalized.textureAsset) << ' '
           << normalized.textureWidth << ' ' << normalized.textureHeight << ' ' << normalized.pixelsPerWorldUnit << ' '
           << static_cast<unsigned>(normalized.sampling) << ' ' << normalized.materialId << ' '
           << std::quoted(normalized.paletteAsset) << ' ' << normalized.paletteBank << ' '
           << normalized.frames.size() << ' ' << normalized.clips.size() << "\n";
    for (const SpriteFrame& frame : normalized.frames) {
        stream << "frame " << std::quoted(frame.name) << ' ' << frame.atlasRect.x << ' '
               << frame.atlasRect.y << ' ' << frame.atlasRect.width << ' ' << frame.atlasRect.height << ' '
               << frame.sourceWidth << ' ' << frame.sourceHeight << ' ' << frame.sourceOffsetX << ' '
               << frame.sourceOffsetY << ' ' << frame.pivotPixels.x << ' ' << frame.pivotPixels.y << ' '
               << frame.durationSeconds << ' ' << std::quoted(frame.event) << "\n";
    }
    for (const SpriteClip& clip : normalized.clips) {
        stream << "clip " << std::quoted(clip.name) << ' ' << static_cast<unsigned>(clip.loopMode) << ' '
               << clip.playbackRate << ' ' << clip.frames.size() << ' '
               << clip.combatWindows.size() << ' ' << clip.socketKeys.size() << ' '
               << clip.propertyKeys.size() << ' ' << clip.rootMotionKeys.size();
        for (const SpriteFrameIndex frame : clip.frames) stream << ' ' << frame;
        stream << "\n";
        for (const SpriteCombatWindow& window : clip.combatWindows) {
            const SpriteCombatVolume& volume = window.volume;
            stream << "combat " << window.firstSequenceIndex << ' ' << window.lastSequenceIndex << ' '
                   << std::quoted(volume.name) << ' ' << static_cast<unsigned>(volume.role) << ' '
                   << static_cast<unsigned>(volume.shape) << ' ' << volume.centerPixels.x << ' '
                   << volume.centerPixels.y << ' ' << volume.sizePixels.x << ' ' << volume.sizePixels.y << ' '
                   << volume.radiusPixels << ' ' << volume.rotationDegrees << ' ' << volume.attackId << ' '
                   << volume.damage << ' ' << volume.knockbackPixelsPerSecond.x << ' '
                   << volume.knockbackPixelsPerSecond.y << ' ' << volume.hitStopTicks << ' '
                   << volume.priority << ' ' << volume.flags << ' ' << window.id << "\n";
        }
        for (const SpriteSocketKey& key : clip.socketKeys) {
            stream << "socket " << std::quoted(key.name) << ' ' << key.sequenceIndex << ' '
                   << key.positionPixels.x << ' ' << key.positionPixels.y << ' '
                   << key.rotationDegrees << ' ' << key.scale.x << ' ' << key.scale.y << ' '
                   << static_cast<unsigned>(key.interpolation) << ' ' << key.id << "\n";
        }
        for (const SpritePropertyKey& key : clip.propertyKeys) {
            const SpritePropertyValue& value = key.value;
            stream << "property " << std::quoted(key.name) << ' ' << key.sequenceIndex << ' '
                   << static_cast<unsigned>(key.interpolation) << ' '
                   << static_cast<unsigned>(value.type) << ' ' << (value.booleanValue ? 1U : 0U) << ' '
                   << value.integerValue << ' ' << value.floatValue << ' ' << value.vec2Value.x << ' '
                   << value.vec2Value.y << ' ' << std::quoted(value.stringValue) << ' '
                   << key.id << "\n";
        }
        for (const SpriteRootMotionKey& key : clip.rootMotionKeys) {
            stream << "root " << key.sequenceIndex << ' ' << key.deltaPixels.x << ' '
                   << key.deltaPixels.y << ' ' << key.rotationDegrees << ' ' << key.id << "\n";
        }
    }
    stream << "hash " << hash << "\n";
    return write_atomic(path, stream.str(), error);
}

SpriteAssetReadResult read_dvesprite(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes) {
    SpriteAssetReadResult result;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) {
        result.error = ec ? "could not stat sprite asset" : "sprite asset exceeds byte limit";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { result.error = "could not open sprite asset"; return result; }
    std::string magic, token;
    unsigned version{}, sampling{};
    std::size_t frameCount{}, clipCount{};
    if (!(stream >> magic >> version) || magic != "DVE_SPRITE" ||
        (version != 1U && version != 2U && version != 3U && version != 4U) ||
        !(stream >> token) || token != "asset" ||
        !(stream >> std::quoted(result.asset.name) >> std::quoted(result.asset.textureAsset) >>
          result.asset.textureWidth >> result.asset.textureHeight >> result.asset.pixelsPerWorldUnit >>
          sampling >> result.asset.materialId)) {
        result.error = "unsupported or invalid sprite asset header";
        return result;
    }
    if (version >= 2U) {
        if (!(stream >> std::quoted(result.asset.paletteAsset) >> result.asset.paletteBank >>
              frameCount >> clipCount)) {
            result.error = "invalid indexed sprite asset header";
            return result;
        }
    } else if (!(stream >> result.asset.paletteBank >> frameCount >> clipCount)) {
        result.error = "invalid legacy sprite asset header";
        return result;
    }
    if (sampling > static_cast<unsigned>(SpriteSampling::Linear) || frameCount == 0U ||
        frameCount > kMaximumFrames || clipCount == 0U || clipCount > kMaximumClips) {
        result.error = "unsupported or invalid sprite asset header";
        return result;
    }
    result.asset.sampling = static_cast<SpriteSampling>(sampling);
    result.asset.frames.resize(frameCount);
    for (SpriteFrame& frame : result.asset.frames) {
        if (!(stream >> token) || token != "frame" ||
            !(stream >> std::quoted(frame.name) >> frame.atlasRect.x >> frame.atlasRect.y >>
              frame.atlasRect.width >> frame.atlasRect.height >> frame.sourceWidth >> frame.sourceHeight >>
              frame.sourceOffsetX >> frame.sourceOffsetY >> frame.pivotPixels.x >> frame.pivotPixels.y >>
              frame.durationSeconds >> std::quoted(frame.event))) {
            result.error = "invalid sprite frame record";
            return result;
        }
    }
    result.asset.clips.resize(clipCount);
    for (SpriteClip& clip : result.asset.clips) {
        unsigned loopMode{};
        std::size_t count{};
        std::size_t combatCount{}, socketCount{}, propertyCount{}, rootCount{};
        if (!(stream >> token) || token != "clip" ||
            !(stream >> std::quoted(clip.name) >> loopMode >> clip.playbackRate >> count)) {
            result.error = "invalid sprite clip record";
            return result;
        }
        if (version >= 3U &&
            !(stream >> combatCount >> socketCount >> propertyCount >> rootCount)) {
            result.error = "invalid sprite metadata track counts";
            return result;
        }
        if (loopMode > static_cast<unsigned>(SpriteLoopMode::PingPong) || count == 0U ||
            count > kMaximumFrames || combatCount > kMaximumTrackEntries ||
            socketCount > kMaximumTrackEntries || propertyCount > kMaximumTrackEntries ||
            rootCount > kMaximumTrackEntries) {
            result.error = "invalid sprite clip or metadata track count";
            return result;
        }
        clip.loopMode = static_cast<SpriteLoopMode>(loopMode);
        clip.frames.resize(count);
        for (SpriteFrameIndex& frame : clip.frames) {
            if (!(stream >> frame)) { result.error = "invalid sprite clip frame"; return result; }
        }
        clip.combatWindows.resize(combatCount);
        for (SpriteCombatWindow& window : clip.combatWindows) {
            unsigned role{}, shape{};
            SpriteCombatVolume& volume = window.volume;
            if (!(stream >> token) || token != "combat" ||
                !(stream >> window.firstSequenceIndex >> window.lastSequenceIndex >>
                  std::quoted(volume.name) >> role >> shape >> volume.centerPixels.x >>
                  volume.centerPixels.y >> volume.sizePixels.x >> volume.sizePixels.y >>
                  volume.radiusPixels >> volume.rotationDegrees >> volume.attackId >> volume.damage >>
                  volume.knockbackPixelsPerSecond.x >> volume.knockbackPixelsPerSecond.y >>
                  volume.hitStopTicks >> volume.priority >> volume.flags) ||
                (version >= 4U && !(stream >> window.id)) ||
                role > static_cast<unsigned>(SpriteCombatRole::Trigger) ||
                shape > static_cast<unsigned>(SpriteCombatShape::Capsule)) {
                result.error = "invalid sprite combat window record";
                return result;
            }
            volume.role = static_cast<SpriteCombatRole>(role);
            volume.shape = static_cast<SpriteCombatShape>(shape);
        }
        clip.socketKeys.resize(socketCount);
        for (SpriteSocketKey& key : clip.socketKeys) {
            unsigned interpolation{};
            if (!(stream >> token) || token != "socket" ||
                !(stream >> std::quoted(key.name) >> key.sequenceIndex >> key.positionPixels.x >>
                  key.positionPixels.y >> key.rotationDegrees >> key.scale.x >> key.scale.y >>
                  interpolation) ||
                (version >= 4U && !(stream >> key.id)) ||
                interpolation > static_cast<unsigned>(SpriteTrackInterpolation::Linear)) {
                result.error = "invalid sprite socket key record";
                return result;
            }
            key.interpolation = static_cast<SpriteTrackInterpolation>(interpolation);
        }
        clip.propertyKeys.resize(propertyCount);
        for (SpritePropertyKey& key : clip.propertyKeys) {
            unsigned interpolation{}, type{}, booleanValue{};
            SpritePropertyValue& value = key.value;
            if (!(stream >> token) || token != "property" ||
                !(stream >> std::quoted(key.name) >> key.sequenceIndex >> interpolation >> type >>
                  booleanValue >> value.integerValue >> value.floatValue >> value.vec2Value.x >>
                  value.vec2Value.y >> std::quoted(value.stringValue)) ||
                (version >= 4U && !(stream >> key.id)) || booleanValue > 1U ||
                interpolation > static_cast<unsigned>(SpriteTrackInterpolation::Linear) ||
                type > static_cast<unsigned>(SpritePropertyType::AssetReference)) {
                result.error = "invalid sprite property key record";
                return result;
            }
            key.interpolation = static_cast<SpriteTrackInterpolation>(interpolation);
            value.type = static_cast<SpritePropertyType>(type);
            value.booleanValue = booleanValue != 0U;
        }
        clip.rootMotionKeys.resize(rootCount);
        for (SpriteRootMotionKey& key : clip.rootMotionKeys) {
            if (!(stream >> token) || token != "root" ||
                !(stream >> key.sequenceIndex >> key.deltaPixels.x >> key.deltaPixels.y >>
                  key.rotationDegrees) || (version >= 4U && !(stream >> key.id))) {
                result.error = "invalid sprite root-motion key record";
                return result;
            }
        }
    }
    if (!(stream >> token >> result.asset.contentHash) || token != "hash") {
        result.error = "missing sprite asset content hash";
        return result;
    }
    stream >> std::ws;
    if (!stream.eof()) { result.error = "trailing sprite asset data"; return result; }
    if (version < 4U) assign_sprite_track_ids(result.asset);
    if (!result.asset.validate(&result.error)) return result;
    const std::uint64_t storedHash = result.asset.contentHash;
    if (version == 1U) {
        if (storedHash != sprite_asset_content_hash_v1(result.asset)) {
            result.error = "sprite asset content hash mismatch";
            return result;
        }
        result.asset.recompute_hash();
    } else if (version == 2U) {
        if (storedHash != sprite_asset_content_hash_v2(result.asset)) {
            result.error = "sprite asset content hash mismatch";
            return result;
        }
        result.asset.recompute_hash();
    } else if (version == 3U) {
        if (storedHash != sprite_asset_content_hash_v3(result.asset)) {
            result.error = "sprite asset content hash mismatch";
            return result;
        }
        result.asset.recompute_hash();
    } else if (storedHash != sprite_asset_content_hash(result.asset)) {
        result.error = "sprite asset content hash mismatch";
    }
    return result;
}

std::optional<SpriteSample> sample_sprite_clip(
    const SpriteAsset& asset,
    std::string_view clipName,
    float timeSeconds) noexcept {
    const SpriteClip* clip = find_sprite_clip(asset, clipName);
    if (clip == nullptr || !std::isfinite(timeSeconds) || timeSeconds < 0.0F ||
        clip->frames.empty()) return std::nullopt;
    const float duration = sequence_duration(asset, *clip);
    if (!(duration > 0.0F) || !std::isfinite(duration)) return std::nullopt;
    const float effectiveTime = timeSeconds * clip->playbackRate;
    float localTime = effectiveTime;
    bool finished = false;
    if (clip->loopMode == SpriteLoopMode::Once) {
        if (localTime >= duration) {
            localTime = std::nextafter(duration, 0.0F);
            finished = true;
        }
    } else {
        localTime = std::fmod(localTime, duration);
    }
    float cursor = 0.0F;
    const std::size_t count = sequence_count(*clip);
    for (std::size_t index = 0U; index < count; ++index) {
        const SpriteFrameIndex frameIndex = sequence_frame(*clip, index);
        const float frameDuration = asset.frames[frameIndex].durationSeconds;
        if (localTime < cursor + frameDuration || index + 1U == count) {
            SpriteSample sample;
            sample.frame = frameIndex;
            sample.sequenceIndex = index;
            sample.authoredSequenceIndex = authored_sequence_index(*clip, index);
            sample.frameTimeSeconds = std::max(0.0F, localTime - cursor) / clip->playbackRate;
            sample.clipTimeSeconds = localTime / clip->playbackRate;
            sample.normalizedTime = std::clamp(localTime / duration, 0.0F, 1.0F);
            sample.finished = finished;
            return sample;
        }
        cursor += frameDuration;
    }
    return std::nullopt;
}

std::optional<SpriteTrackSample> sample_sprite_clip_tracks(
    const SpriteAsset& asset,
    std::string_view clipName,
    float timeSeconds) noexcept {
    const SpriteClip* clip = find_sprite_clip(asset, clipName);
    const std::optional<SpriteSample> animation = sample_sprite_clip(asset, clipName, timeSeconds);
    if (clip == nullptr || !animation) return std::nullopt;

    SpriteTrackSample result;
    result.animation = *animation;
    const std::size_t authored = animation->authoredSequenceIndex;
    const SpriteFrame& frame = asset.frames[animation->frame];
    const float framePlaybackDuration = frame.durationSeconds / clip->playbackRate;
    const float progress = framePlaybackDuration > 0.0F
        ? std::clamp(animation->frameTimeSeconds / framePlaybackDuration, 0.0F, 1.0F)
        : 0.0F;
    const std::size_t nextPlayback = next_playback_sequence_index(
        *clip, animation->sequenceIndex, animation->finished);
    const std::size_t nextAuthored = authored_sequence_index(*clip, nextPlayback);
    const std::ptrdiff_t authoredDelta = static_cast<std::ptrdiff_t>(nextAuthored) -
        static_cast<std::ptrdiff_t>(authored);
    const bool adjacent = std::abs(authoredDelta) <= 1;
    const float coordinate = adjacent
        ? static_cast<float>(authored) + static_cast<float>(authoredDelta) * progress
        : static_cast<float>(authored);

    for (const SpriteCombatWindow& window : clip->combatWindows) {
        if (authored < window.firstSequenceIndex || authored > window.lastSequenceIndex) continue;
        SpriteCombatVolumeSample sampled;
        sampled.id = window.id;
        sampled.volume = window.volume;
        sampled.worldTransform = local_2d_transform(
            window.volume.centerPixels, window.volume.rotationDegrees,
            asset.pixelsPerWorldUnit, GameplayPlane2D::XY);
        sampled.sizeWorld = {
            window.volume.sizePixels.x / asset.pixelsPerWorldUnit,
            window.volume.sizePixels.y / asset.pixelsPerWorldUnit,
        };
        sampled.radiusWorld = window.volume.radiusPixels / asset.pixelsPerWorldUnit;
        result.combatVolumes.push_back(std::move(sampled));
    }

    std::set<std::string, std::less<>> socketNames;
    for (const SpriteSocketKey& key : clip->socketKeys) socketNames.insert(key.name);
    for (const std::string& name : socketNames) {
        const SpriteSocketKey* stepKey = nullptr;
        const SpriteSocketKey* lower = nullptr;
        const SpriteSocketKey* upper = nullptr;
        for (const SpriteSocketKey& key : clip->socketKeys) {
            if (key.name != name) continue;
            if (key.sequenceIndex <= authored &&
                (stepKey == nullptr || key.sequenceIndex > stepKey->sequenceIndex)) stepKey = &key;
            const float keyCoordinate = static_cast<float>(key.sequenceIndex);
            if (keyCoordinate <= coordinate &&
                (lower == nullptr || key.sequenceIndex > lower->sequenceIndex)) lower = &key;
            if (keyCoordinate >= coordinate &&
                (upper == nullptr || key.sequenceIndex < upper->sequenceIndex)) upper = &key;
        }
        if (stepKey == nullptr) continue;
        SpriteSocketSample sampled;
        sampled.id = stepKey->id;
        sampled.name = name;
        sampled.positionPixels = stepKey->positionPixels;
        sampled.rotationDegrees = stepKey->rotationDegrees;
        sampled.scale = stepKey->scale;
        if (lower != nullptr && upper != nullptr && lower != upper &&
            lower->interpolation == SpriteTrackInterpolation::Linear) {
            sampled.id = lower->id;
            const float span = static_cast<float>(upper->sequenceIndex - lower->sequenceIndex);
            const float alpha = span > 0.0F
                ? std::clamp((coordinate - static_cast<float>(lower->sequenceIndex)) / span,
                             0.0F, 1.0F)
                : 0.0F;
            sampled.positionPixels = lerp(lower->positionPixels, upper->positionPixels, alpha);
            sampled.rotationDegrees = lerp(lower->rotationDegrees, upper->rotationDegrees, alpha);
            sampled.scale = lerp(lower->scale, upper->scale, alpha);
        }
        sampled.worldTransform = local_2d_transform(
            sampled.positionPixels, sampled.rotationDegrees,
            asset.pixelsPerWorldUnit, GameplayPlane2D::XY);
        result.sockets.push_back(std::move(sampled));
    }

    std::set<std::string, std::less<>> propertyNames;
    for (const SpritePropertyKey& key : clip->propertyKeys) propertyNames.insert(key.name);
    for (const std::string& name : propertyNames) {
        const SpritePropertyKey* stepKey = nullptr;
        const SpritePropertyKey* lower = nullptr;
        const SpritePropertyKey* upper = nullptr;
        for (const SpritePropertyKey& key : clip->propertyKeys) {
            if (key.name != name) continue;
            if (key.sequenceIndex <= authored &&
                (stepKey == nullptr || key.sequenceIndex > stepKey->sequenceIndex)) stepKey = &key;
            const float keyCoordinate = static_cast<float>(key.sequenceIndex);
            if (keyCoordinate <= coordinate &&
                (lower == nullptr || key.sequenceIndex > lower->sequenceIndex)) lower = &key;
            if (keyCoordinate >= coordinate &&
                (upper == nullptr || key.sequenceIndex < upper->sequenceIndex)) upper = &key;
        }
        if (stepKey == nullptr) continue;
        SpritePropertySample sampled;
        sampled.id = stepKey->id;
        sampled.name = name;
        sampled.value = stepKey->value;
        if (lower != nullptr && upper != nullptr && lower != upper &&
            lower->interpolation == SpriteTrackInterpolation::Linear &&
            lower->value.type == upper->value.type) {
            sampled.id = lower->id;
            const float span = static_cast<float>(upper->sequenceIndex - lower->sequenceIndex);
            const float alpha = span > 0.0F
                ? std::clamp((coordinate - static_cast<float>(lower->sequenceIndex)) / span,
                             0.0F, 1.0F)
                : 0.0F;
            sampled.value = interpolate_property(lower->value, upper->value, alpha);
        }
        result.properties.push_back(std::move(sampled));
    }

    const auto root = std::find_if(
        clip->rootMotionKeys.begin(), clip->rootMotionKeys.end(),
        [authored](const SpriteRootMotionKey& key) { return key.sequenceIndex == authored; });
    if (root != clip->rootMotionKeys.end()) {
        result.rootMotionDeltaPixels = root->deltaPixels;
        result.rootMotionRotationDegrees = root->rotationDegrees;
        result.rootMotionKeyId = root->id;
    }
    return result;
}



bool query_sprite_clip_interval_events(
    const SpriteAsset& asset, std::string_view clipName, float fromTimeSeconds,
    float toTimeSeconds, const SpriteIntervalQueryOptions& options,
    SpriteIntervalQueryResult& out, std::string* error) {
    out = {};
    if (!std::isfinite(fromTimeSeconds) || !std::isfinite(toTimeSeconds) ||
        fromTimeSeconds < 0.0F || toTimeSeconds < 0.0F) {
        return fail(error, "sprite interval times must be finite and nonnegative");
    }
    if (options.maximumEvents == 0U || options.maximumEvents > 1048576U)
        return fail(error, "sprite interval maximum event count is out of range");
    const SpriteClip* clip = find_sprite_clip(asset, clipName);
    if (clip == nullptr) return fail(error, "sprite interval clip does not exist");
    const std::size_t count = sequence_count(*clip);
    const double duration = static_cast<double>(sequence_duration(asset, *clip));
    if (count == 0U || !(duration > 0.0) || !std::isfinite(duration) ||
        !std::isfinite(clip->playbackRate) || !(clip->playbackRate > 0.0F)) {
        return fail(error, "sprite interval clip duration is invalid");
    }
    const double from = static_cast<double>(fromTimeSeconds) * clip->playbackRate;
    const double to = static_cast<double>(toTimeSeconds) * clip->playbackRate;
    if (!std::isfinite(from) || !std::isfinite(to))
        return fail(error, "sprite interval effective time is out of range");
    out.reverse = to < from;
    if (to == from) return true;

    std::vector<double> starts(count, 0.0);
    for (std::size_t index = 1U; index < count; ++index) {
        starts[index] = starts[index - 1U] +
            static_cast<double>(asset.frames[sequence_frame(*clip, index - 1U)].durationSeconds);
    }

    struct Crossing {
        double time{};
        std::int64_t cycle{};
        std::size_t boundary{};
    };
    struct ForwardCompare {
        bool operator()(const Crossing& left, const Crossing& right) const noexcept {
            if (left.time != right.time) return left.time > right.time;
            return left.boundary > right.boundary;
        }
    };
    struct ReverseCompare {
        bool operator()(const Crossing& left, const Crossing& right) const noexcept {
            if (left.time != right.time) return left.time < right.time;
            return left.boundary < right.boundary;
        }
    };

    auto bounded_i64 = [](double value, std::int64_t& result) noexcept {
        if (!std::isfinite(value) ||
            value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) return false;
        result = static_cast<std::int64_t>(value);
        return true;
    };
    auto saturating_add_boundaries = [&out](std::uint64_t amount) noexcept {
        if (amount > std::numeric_limits<std::uint64_t>::max() - out.crossedFrameBoundaries)
            out.crossedFrameBoundaries = std::numeric_limits<std::uint64_t>::max();
        else
            out.crossedFrameBoundaries += amount;
    };

    std::priority_queue<Crossing, std::vector<Crossing>, ForwardCompare> forward;
    std::priority_queue<Crossing, std::vector<Crossing>, ReverseCompare> reverse;
    const bool looping = clip->loopMode != SpriteLoopMode::Once;
    for (std::size_t boundary = looping ? 0U : 1U; boundary < count; ++boundary) {
        const double offset = starts[boundary];
        if (!looping) {
            const bool crossed = !out.reverse
                ? (offset > from && offset <= to)
                : (offset < from && offset >= to);
            if (crossed) {
                saturating_add_boundaries(1U);
                if (out.reverse) reverse.push({offset, 0, boundary});
                else forward.push({offset, 0, boundary});
            }
            continue;
        }

        const std::int64_t minimumCycle = boundary == 0U ? 1 : 0;
        std::int64_t firstCycle{};
        std::int64_t lastCycle{};
        if (!out.reverse) {
            if (!bounded_i64(std::floor((from - offset) / duration) + 1.0, firstCycle) ||
                !bounded_i64(std::floor((to - offset) / duration), lastCycle)) {
                return fail(error, "sprite interval spans too many playback cycles");
            }
        } else {
            if (!bounded_i64(std::ceil((to - offset) / duration), firstCycle) ||
                !bounded_i64(std::ceil((from - offset) / duration) - 1.0, lastCycle)) {
                return fail(error, "sprite interval spans too many playback cycles");
            }
        }
        firstCycle = std::max(firstCycle, minimumCycle);
        lastCycle = std::max(lastCycle, minimumCycle - 1);
        if (lastCycle < firstCycle) continue;
        const std::uint64_t occurrences = static_cast<std::uint64_t>(lastCycle - firstCycle) + 1U;
        saturating_add_boundaries(occurrences);
        const std::int64_t firstToVisit = out.reverse ? lastCycle : firstCycle;
        const double crossingTime = offset + static_cast<double>(firstToVisit) * duration;
        if (out.reverse) reverse.push({crossingTime, firstToVisit, boundary});
        else forward.push({crossingTime, firstToVisit, boundary});
    }

    auto append_event = [&](SpriteIntervalEvent event) {
        if (out.events.size() >= options.maximumEvents) {
            out.truncated = true;
            return false;
        }
        out.events.push_back(std::move(event));
        return true;
    };
    auto emit_crossing = [&](const Crossing& crossing) {
        const std::size_t enteredPlayback = out.reverse
            ? (crossing.boundary == 0U ? count - 1U : crossing.boundary - 1U)
            : crossing.boundary;
        const std::size_t previousPlayback = out.reverse
            ? crossing.boundary
            : (crossing.boundary == 0U ? count - 1U : crossing.boundary - 1U);
        const std::size_t enteredAuthored = authored_sequence_index(*clip, enteredPlayback);
        const std::size_t previousAuthored = authored_sequence_index(*clip, previousPlayback);
        const SpriteFrameIndex enteredFrame = sequence_frame(*clip, enteredPlayback);
        const float eventTime = static_cast<float>(crossing.time / clip->playbackRate);
        const auto base = [&](SpriteIntervalEventKind kind, SpriteTrackId id,
                              std::string name) {
            SpriteIntervalEvent event;
            event.kind = kind;
            event.timeSeconds = eventTime;
            event.reverse = out.reverse;
            event.cycle = crossing.cycle;
            event.playbackSequenceIndex = enteredPlayback;
            event.authoredSequenceIndex = enteredAuthored;
            event.frame = enteredFrame;
            event.trackId = id;
            event.name = std::move(name);
            return event;
        };

        if (options.combatBoundaries) {
            for (const SpriteCombatWindow& window : clip->combatWindows) {
                const bool wasActive = previousAuthored >= window.firstSequenceIndex &&
                    previousAuthored <= window.lastSequenceIndex;
                const bool active = enteredAuthored >= window.firstSequenceIndex &&
                    enteredAuthored <= window.lastSequenceIndex;
                if (wasActive && !active &&
                    !append_event(base(SpriteIntervalEventKind::CombatDeactivated,
                                       window.id, window.volume.name))) return false;
            }
            for (const SpriteCombatWindow& window : clip->combatWindows) {
                const bool wasActive = previousAuthored >= window.firstSequenceIndex &&
                    previousAuthored <= window.lastSequenceIndex;
                const bool active = enteredAuthored >= window.firstSequenceIndex &&
                    enteredAuthored <= window.lastSequenceIndex;
                if (!wasActive && active &&
                    !append_event(base(SpriteIntervalEventKind::CombatActivated,
                                       window.id, window.volume.name))) return false;
            }
        }
        const SpriteFrame& frame = asset.frames[enteredFrame];
        if (options.frameEvents && !frame.event.empty() &&
            !append_event(base(SpriteIntervalEventKind::FrameEvent,
                               kInvalidSpriteTrackId, frame.event))) return false;
        if (options.socketKeys) {
            for (const SpriteSocketKey& key : clip->socketKeys) {
                if (key.sequenceIndex == enteredAuthored &&
                    !append_event(base(SpriteIntervalEventKind::SocketKey, key.id, key.name)))
                    return false;
            }
        }
        if (options.propertyKeys) {
            for (const SpritePropertyKey& key : clip->propertyKeys) {
                if (key.sequenceIndex == enteredAuthored &&
                    !append_event(base(SpriteIntervalEventKind::PropertyKey, key.id, key.name)))
                    return false;
            }
        }
        if (options.rootMotionKeys) {
            for (const SpriteRootMotionKey& key : clip->rootMotionKeys) {
                if (key.sequenceIndex == enteredAuthored &&
                    !append_event(base(SpriteIntervalEventKind::RootMotionKey, key.id,
                                       "root_motion"))) return false;
            }
        }
        if (crossing.boundary == 0U) out.crossedLoopBoundary = true;
        return true;
    };

    while ((!out.reverse && !forward.empty()) || (out.reverse && !reverse.empty())) {
        Crossing crossing;
        if (out.reverse) {
            crossing = reverse.top();
            reverse.pop();
        } else {
            crossing = forward.top();
            forward.pop();
        }
        if (!emit_crossing(crossing)) break;
        if (!looping) continue;
        crossing.cycle += out.reverse ? -1 : 1;
        const std::int64_t minimumCycle = crossing.boundary == 0U ? 1 : 0;
        if (crossing.cycle < minimumCycle) continue;
        crossing.time += out.reverse ? -duration : duration;
        const bool remains = out.reverse ? crossing.time >= to : crossing.time <= to;
        if (remains) {
            if (out.reverse) reverse.push(crossing);
            else forward.push(crossing);
        }
    }
    if (out.events.size() >= options.maximumEvents &&
        ((!out.reverse && !forward.empty()) || (out.reverse && !reverse.empty()))) {
        out.truncated = true;
    }
    return true;
}

std::optional<SpriteRootMotionDelta> accumulate_sprite_root_motion(
    const SpriteAsset& asset, std::string_view clipName, float fromTimeSeconds,
    float toTimeSeconds, GameplayPlane2D plane, bool flipX, bool flipY) noexcept {
    const SpriteClip* clip = find_sprite_clip(asset, clipName);
    if (clip == nullptr || !std::isfinite(fromTimeSeconds) || !std::isfinite(toTimeSeconds) ||
        fromTimeSeconds < 0.0F || toTimeSeconds < fromTimeSeconds ||
        static_cast<unsigned>(plane) > static_cast<unsigned>(GameplayPlane2D::XZ)) {
        return std::nullopt;
    }
    SpriteRootMotionDelta result;
    if (toTimeSeconds == fromTimeSeconds) return result;
    const std::size_t count = sequence_count(*clip);
    const double duration = static_cast<double>(sequence_duration(asset, *clip));
    if (count == 0U || !(duration > 0.0) || !std::isfinite(duration)) return std::nullopt;
    const double from = static_cast<double>(fromTimeSeconds) * clip->playbackRate;
    const double to = static_cast<double>(toTimeSeconds) * clip->playbackRate;
    std::vector<double> starts(count, 0.0);
    for (std::size_t index = 1U; index < count; ++index) {
        starts[index] = starts[index - 1U] +
            static_cast<double>(asset.frames[sequence_frame(*clip, index - 1U)].durationSeconds);
    }
    auto add_key = [&](std::size_t playbackIndex, std::uint64_t occurrences) {
        if (occurrences == 0U) return;
        const std::size_t authored = authored_sequence_index(*clip, playbackIndex);
        const SpriteRootMotionKey* key = root_key_at(*clip, authored);
        if (key != nullptr) {
            result.deltaPixels.x += key->deltaPixels.x * static_cast<float>(occurrences);
            result.deltaPixels.y += key->deltaPixels.y * static_cast<float>(occurrences);
            result.rotationDegrees += key->rotationDegrees * static_cast<float>(occurrences);
        }
        const std::uint64_t total = static_cast<std::uint64_t>(result.enteredFrames) + occurrences;
        result.enteredFrames = total > std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(total);
    };
    if (clip->loopMode == SpriteLoopMode::Once) {
        const double boundedFrom = std::clamp(from, 0.0, duration);
        const double boundedTo = std::clamp(to, 0.0, duration);
        for (std::size_t index = 1U; index < count; ++index) {
            if (starts[index] > boundedFrom && starts[index] <= boundedTo) add_key(index, 1U);
        }
    } else {
        for (std::size_t index = 0U; index < count; ++index) {
            const double offset = starts[index];
            const double upperRaw = std::floor((to - offset) / duration);
            const double lowerRaw = std::floor((from - offset) / duration);
            std::int64_t upper = upperRaw > static_cast<double>(std::numeric_limits<std::int64_t>::max())
                ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(upperRaw);
            std::int64_t lower = lowerRaw > static_cast<double>(std::numeric_limits<std::int64_t>::max())
                ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(lowerRaw);
            const std::int64_t minimumCycle = index == 0U ? 1 : 0;
            upper = std::max(upper, minimumCycle - 1);
            lower = std::max(lower, minimumCycle - 1);
            if (upper <= lower) continue;
            const std::uint64_t occurrences = static_cast<std::uint64_t>(upper - lower);
            add_key(index, occurrences);
            if (index == 0U) result.crossedLoopBoundary = true;
        }
    }
    result.deltaPixels = mirrored_point(result.deltaPixels, flipX, flipY);
    result.rotationDegrees = mirrored_rotation(result.rotationDegrees, flipX, flipY);
    result.worldTranslation = root_pixels_to_world(result.deltaPixels, asset.pixelsPerWorldUnit, plane);
    return result;
}

bool visual_2d_less(
    const Visual2DSortEntry& left,
    const Visual2DSortEntry& right) noexcept {
    if (left.sortingLayer != right.sortingLayer) return left.sortingLayer < right.sortingLayer;
    if (left.orderInLayer != right.orderInLayer) return left.orderInLayer < right.orderInLayer;
    if (left.sortDepth != right.sortDepth) return left.sortDepth < right.sortDepth;
    if (left.owner != right.owner) return left.owner < right.owner;
    if (left.kind != right.kind)
        return static_cast<unsigned>(left.kind) < static_cast<unsigned>(right.kind);
    return left.sourceIndex < right.sourceIndex;
}

std::vector<Visual2DSortEntry> sort_visual_2d_entries(
    std::span<const Visual2DSortEntry> entries) {
    std::vector<Visual2DSortEntry> result(entries.begin(), entries.end());
    std::stable_sort(result.begin(), result.end(), visual_2d_less);
    return result;
}

bool SpriteRuntime::register_asset(
    SpriteAssetId id, SpriteAsset assetValue, std::string* error) {
    if (id == kInvalidSpriteAssetId) return fail(error, "sprite asset ID is invalid");
    if (assets_.contains(id)) return fail(error, "sprite asset ID is already registered");
    assign_sprite_track_ids(assetValue);
    if (!assetValue.validate(error)) return false;
    if (!assetValue.paletteAsset.empty()) {
        const SpritePaletteAsset* paletteValue = palette(assetValue.paletteAsset);
        if (paletteValue == nullptr)
            return fail(error, "sprite palette asset is not registered");
        if (assetValue.paletteBank >= paletteValue->banks.size())
            return fail(error, "sprite default palette bank is out of range");
    }
    const std::uint64_t hash = sprite_asset_content_hash(assetValue);
    if (assetValue.contentHash != 0U && assetValue.contentHash != hash)
        return fail(error, "sprite asset content hash is stale");
    assetValue.contentHash = hash;
    assets_.emplace(id, std::move(assetValue));
    return true;
}

bool SpriteRuntime::unregister_asset(SpriteAssetId id) noexcept {
    if (std::any_of(instances_.begin(), instances_.end(), [id](const auto& pair) {
            return pair.second.desc.asset == id;
        })) return false;
    return assets_.erase(id) != 0U;
}

const SpriteAsset* SpriteRuntime::asset(SpriteAssetId id) const noexcept {
    const auto found = assets_.find(id);
    return found == assets_.end() ? nullptr : &found->second;
}

bool SpriteRuntime::register_palette(
    std::string reference, SpritePaletteAsset paletteValue, std::string* error) {
    if (reference.empty() || reference.size() > 4096U)
        return fail(error, "sprite palette reference is invalid");
    if (palettes_.contains(reference))
        return fail(error, "sprite palette reference is already registered");
    if (!paletteValue.validate(error)) return false;
    const std::uint64_t hash = sprite_palette_content_hash(paletteValue);
    if (paletteValue.contentHash != 0U && paletteValue.contentHash != hash)
        return fail(error, "sprite palette content hash is stale");
    paletteValue.contentHash = hash;
    palettes_.emplace(std::move(reference), std::move(paletteValue));
    return true;
}

bool SpriteRuntime::unregister_palette(std::string_view reference) noexcept {
    if (std::any_of(assets_.begin(), assets_.end(), [reference](const auto& pair) {
            return pair.second.paletteAsset == reference;
        })) return false;
    const auto found = palettes_.find(reference);
    if (found == palettes_.end()) return false;
    palettes_.erase(found);
    return true;
}

const SpritePaletteAsset* SpriteRuntime::palette(std::string_view reference) const noexcept {
    const auto found = palettes_.find(reference);
    return found == palettes_.end() ? nullptr : &found->second;
}

bool SpriteRuntime::bind(
    SpriteOwnerId owner, SpriteInstanceDesc desc, std::string* error) {
    if (owner == kInvalidSpriteOwnerId || instances_.contains(owner))
        return fail(error, "sprite owner is invalid or already bound");
    const SpriteAsset* source = asset(desc.asset);
    if (source == nullptr) return fail(error, "sprite asset is not registered");
    if (desc.clip.empty()) desc.clip = source->clips.front().name;
    if (find_sprite_clip(*source, desc.clip) == nullptr)
        return fail(error, "sprite clip does not exist");
    if (!finite_transform(desc.transform) ||
        static_cast<unsigned>(desc.plane) > static_cast<unsigned>(GameplayPlane2D::XZ) ||
        static_cast<unsigned>(desc.blendMode) > static_cast<unsigned>(SpriteBlendMode::Multiply) ||
        !std::all_of(desc.color.begin(), desc.color.end(), [](float value) { return std::isfinite(value); }) ||
        !std::isfinite(desc.sortDepth) || !std::isfinite(desc.playbackSpeed) ||
        desc.playbackSpeed < 0.0F || desc.playbackSpeed > 1024.0F)
        return fail(error, "sprite instance is invalid");
    if (source->paletteAsset.empty()) {
        if (desc.paletteBankOverride || !desc.paletteSwaps.empty())
            return fail(error, "RGBA sprite instances cannot select or swap palette entries");
    } else {
        const SpritePaletteAsset* paletteValue = palette(source->paletteAsset);
        if (paletteValue == nullptr) return fail(error, "sprite palette asset is not registered");
        const std::uint32_t bank = desc.paletteBankOverride.value_or(source->paletteBank);
        if (bank >= paletteValue->banks.size())
            return fail(error, "sprite instance palette bank is out of range");
        if (!validate_sprite_palette_swaps(*paletteValue, desc.paletteSwaps, error))
            return false;
    }
    Instance instance;
    instance.desc = std::move(desc);
    const auto initial = sample_sprite_clip(*source, instance.desc.clip, 0.0F);
    instance.previousFrame = initial ? initial->frame : kInvalidSpriteFrameIndex;
    instances_.emplace(owner, std::move(instance));
    return true;
}

bool SpriteRuntime::unbind(SpriteOwnerId owner) noexcept {
    if (instances_.erase(owner) == 0U) return false;
    for (auto it = attachments_.begin(); it != attachments_.end();) {
        if (it->second.parent == owner) it = attachments_.erase(it); else ++it;
    }
    return true;
}
bool SpriteRuntime::contains(SpriteOwnerId owner) const noexcept { return instances_.contains(owner); }

bool SpriteRuntime::set_clip(
    SpriteOwnerId owner, std::string_view clip, bool restart, std::string* error) {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return fail(error, "sprite owner is not bound");
    const SpriteAsset& source = assets_.at(found->second.desc.asset);
    if (find_sprite_clip(source, clip) == nullptr) return fail(error, "sprite clip does not exist");
    found->second.desc.clip = std::string(clip);
    if (restart) found->second.timeSeconds = 0.0F;
    found->second.pendingRootMotion = {};
    const auto current = sample_sprite_clip(source, found->second.desc.clip, found->second.timeSeconds);
    found->second.previousFrame = current ? current->frame : kInvalidSpriteFrameIndex;
    return true;
}

bool SpriteRuntime::set_transform(SpriteOwnerId owner, const RigidTransform& transform) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end() || !finite_transform(transform)) return false;
    found->second.desc.transform = make_rigid_transform(transform.position, transform.rotation);
    return true;
}

bool SpriteRuntime::set_visible(SpriteOwnerId owner, bool visible) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return false;
    found->second.desc.visible = visible;
    return true;
}

bool SpriteRuntime::set_flip(SpriteOwnerId owner, bool flipX, bool flipY) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return false;
    found->second.desc.flipX = flipX;
    found->second.desc.flipY = flipY;
    return true;
}

bool SpriteRuntime::set_playing(SpriteOwnerId owner, bool playing) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return false;
    found->second.desc.playing = playing;
    return true;
}

bool SpriteRuntime::set_playback_speed(SpriteOwnerId owner, float speed) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end() || !std::isfinite(speed) || speed < 0.0F || speed > 1024.0F)
        return false;
    found->second.desc.playbackSpeed = speed;
    return true;
}

float SpriteRuntime::time_seconds(SpriteOwnerId owner) const noexcept {
    const auto found = instances_.find(owner);
    return found == instances_.end() ? 0.0F : found->second.timeSeconds;
}

const SpriteInstanceDesc* SpriteRuntime::instance_desc(SpriteOwnerId owner) const noexcept {
    const auto found = instances_.find(owner);
    return found == instances_.end() ? nullptr : &found->second.desc;
}

bool SpriteRuntime::set_sorting(
    SpriteOwnerId owner, std::int32_t layer, std::int32_t order, float depth) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end() || !std::isfinite(depth)) return false;
    found->second.desc.sortingLayer = layer;
    found->second.desc.orderInLayer = order;
    found->second.desc.sortDepth = depth;
    return true;
}

bool SpriteRuntime::set_palette_bank(
    SpriteOwnerId owner, std::optional<std::uint32_t> bank, std::string* error) {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return fail(error, "sprite owner is not bound");
    const SpriteAsset& source = assets_.at(found->second.desc.asset);
    if (source.paletteAsset.empty())
        return fail(error, "RGBA sprite instance has no palette bank");
    const SpritePaletteAsset* paletteValue = palette(source.paletteAsset);
    if (paletteValue == nullptr) return fail(error, "sprite palette asset is not registered");
    if (bank && *bank >= paletteValue->banks.size())
        return fail(error, "sprite instance palette bank is out of range");
    found->second.desc.paletteBankOverride = bank;
    return true;
}

bool SpriteRuntime::set_palette_swaps(
    SpriteOwnerId owner, std::vector<SpritePaletteSwap> swaps, std::string* error) {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return fail(error, "sprite owner is not bound");
    const SpriteAsset& source = assets_.at(found->second.desc.asset);
    if (source.paletteAsset.empty())
        return fail(error, "RGBA sprite instance has no palette entries to swap");
    const SpritePaletteAsset* paletteValue = palette(source.paletteAsset);
    if (paletteValue == nullptr) return fail(error, "sprite palette asset is not registered");
    if (!validate_sprite_palette_swaps(*paletteValue, swaps, error)) return false;
    found->second.desc.paletteSwaps = std::move(swaps);
    return true;
}

bool SpriteRuntime::seek(SpriteOwnerId owner, float timeSeconds) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end() || !std::isfinite(timeSeconds) || timeSeconds < 0.0F)
        return false;
    found->second.timeSeconds = timeSeconds;
    found->second.pendingRootMotion = {};
    const SpriteAsset& source = assets_.at(found->second.desc.asset);
    const auto current = sample_sprite_clip(source, found->second.desc.clip, timeSeconds);
    found->second.previousFrame = current ? current->frame : kInvalidSpriteFrameIndex;
    return true;
}


bool SpriteRuntime::bind_socket_attachment(
    SpriteSocketAttachmentDesc desc, std::string* error) {
    if (desc.id == kInvalidSpriteAttachmentId || attachments_.contains(desc.id))
        return fail(error, "sprite attachment ID is invalid or already bound");
    const auto parent = instances_.find(desc.parent);
    if (parent == instances_.end()) return fail(error, "sprite attachment parent is not bound");
    if (desc.socket.empty() || desc.socket.size() > 255U || !finite_transform(desc.localOffset))
        return fail(error, "sprite attachment socket or local transform is invalid");
    const SpriteAsset& assetValue = assets_.at(parent->second.desc.asset);
    const SpriteClip* clip = find_sprite_clip(assetValue, parent->second.desc.clip);
    if (clip == nullptr || !std::any_of(clip->socketKeys.begin(), clip->socketKeys.end(),
            [&desc](const SpriteSocketKey& key) { return key.name == desc.socket; })) {
        return fail(error, "sprite attachment socket does not exist in the active clip");
    }
    attachments_.emplace(desc.id, std::move(desc));
    return true;
}

bool SpriteRuntime::unbind_socket_attachment(SpriteAttachmentId id) noexcept {
    return attachments_.erase(id) != 0U;
}

std::vector<SpriteSocketAttachmentSample> SpriteRuntime::sample_socket_attachments() const {
    std::vector<SpriteSocketAttachmentSample> result;
    result.reserve(attachments_.size());
    for (const auto& [id, desc] : attachments_) {
        const auto parent = instances_.find(desc.parent);
        if (parent == instances_.end()) continue;
        const std::optional<SpriteTrackSample> tracks = sample_tracks(desc.parent);
        if (!tracks) continue;
        const auto socket = std::find_if(tracks->sockets.begin(), tracks->sockets.end(),
            [&desc](const SpriteSocketSample& candidate) { return candidate.name == desc.socket; });
        if (socket == tracks->sockets.end()) continue;
        SpriteSocketAttachmentSample sample;
        sample.id = id;
        sample.parent = desc.parent;
        sample.socket = desc.socket;
        sample.worldTransform = compose_rigid_transforms(socket->worldTransform, desc.localOffset);
        sample.visible = !desc.inheritVisibility || parent->second.desc.visible;
        result.push_back(std::move(sample));
    }
    return result;
}

std::optional<SpriteRootMotionDelta> SpriteRuntime::pending_root_motion(
    SpriteOwnerId owner) const noexcept {
    const auto found = instances_.find(owner);
    return found == instances_.end() ? std::nullopt
                                    : std::optional<SpriteRootMotionDelta>{found->second.pendingRootMotion};
}

std::optional<SpriteRootMotionDelta> SpriteRuntime::consume_root_motion(SpriteOwnerId owner) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return std::nullopt;
    SpriteRootMotionDelta result = found->second.pendingRootMotion;
    found->second.pendingRootMotion = {};
    return result;
}

bool SpriteRuntime::set_pending_root_motion(
    SpriteOwnerId owner, const SpriteRootMotionDelta& delta) noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return false;
    found->second.pendingRootMotion = delta;
    return true;
}

std::vector<SpriteGameplayDebugPacket> SpriteRuntime::build_gameplay_debug_packets() const {
    std::vector<SpriteGameplayDebugPacket> result;
    result.reserve(instances_.size());
    for (const auto& [owner, instance] : instances_) {
        const std::optional<SpriteTrackSample> tracks = sample_tracks(owner);
        if (!tracks) continue;
        SpriteGameplayDebugPacket packet;
        packet.owner = owner;
        packet.asset = instance.desc.asset;
        packet.clip = instance.desc.clip;
        packet.frame = tracks->animation.frame;
        packet.authoredSequenceIndex = tracks->animation.authoredSequenceIndex;
        packet.combatVolumes = tracks->combatVolumes;
        packet.sockets = tracks->sockets;
        packet.properties = tracks->properties;
        packet.pendingRootMotion = instance.pendingRootMotion;
        result.push_back(std::move(packet));
    }
    return result;
}

void SpriteRuntime::advance_palette_ticks(std::uint64_t ticks) noexcept {
    if (ticks > std::numeric_limits<std::uint64_t>::max() - paletteClockTicks_)
        paletteClockTicks_ = std::numeric_limits<std::uint64_t>::max();
    else
        paletteClockTicks_ += ticks;
}

void SpriteRuntime::tick(float deltaSeconds) {
    events_.clear();
    intervalEvents_.clear();
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F) return;
    const double paletteTicks = paletteTickRemainder_ +
        static_cast<double>(deltaSeconds) * kSpritePaletteClockTicksPerSecond;
    const double wholeTicks = std::floor(paletteTicks);
    paletteTickRemainder_ = paletteTicks - wholeTicks;
    if (wholeTicks > 0.0) {
        const double maximumTicks =
            static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        if (wholeTicks >= maximumTicks)
            paletteClockTicks_ = std::numeric_limits<std::uint64_t>::max();
        else
            advance_palette_ticks(static_cast<std::uint64_t>(wholeTicks));
    }
    for (auto& [owner, instance] : instances_) {
        const SpriteAsset& source = assets_.at(instance.desc.asset);
        if (instance.desc.playing && instance.desc.playbackSpeed > 0.0F) {
            const float previousTime = instance.timeSeconds;
            instance.timeSeconds += deltaSeconds * instance.desc.playbackSpeed;
            const std::optional<SpriteRootMotionDelta> delta = accumulate_sprite_root_motion(
                source, instance.desc.clip, previousTime, instance.timeSeconds,
                instance.desc.plane, instance.desc.flipX, instance.desc.flipY);
            if (delta) {
                instance.pendingRootMotion.deltaPixels.x += delta->deltaPixels.x;
                instance.pendingRootMotion.deltaPixels.y += delta->deltaPixels.y;
                instance.pendingRootMotion.rotationDegrees += delta->rotationDegrees;
                const Float3 rotated = transform_vector(instance.desc.transform, delta->worldTranslation);
                instance.pendingRootMotion.worldTranslation = add(
                    instance.pendingRootMotion.worldTranslation, rotated);
                instance.pendingRootMotion.crossedLoopBoundary =
                    instance.pendingRootMotion.crossedLoopBoundary || delta->crossedLoopBoundary;
                const std::uint64_t entered =
                    static_cast<std::uint64_t>(instance.pendingRootMotion.enteredFrames) +
                    delta->enteredFrames;
                instance.pendingRootMotion.enteredFrames =
                    entered > std::numeric_limits<std::uint32_t>::max()
                    ? std::numeric_limits<std::uint32_t>::max()
                    : static_cast<std::uint32_t>(entered);
            }

            SpriteIntervalQueryResult interval;
            SpriteIntervalQueryOptions options;
            options.maximumEvents = 16384U;
            std::string ignored;
            if (query_sprite_clip_interval_events(source, instance.desc.clip, previousTime,
                                                  instance.timeSeconds, options, interval,
                                                  &ignored)) {
                for (SpriteIntervalEvent& event : interval.events) {
                    event.owner = owner;
                    if (event.kind == SpriteIntervalEventKind::FrameEvent)
                        events_.push_back({owner, event.frame, event.name});
                    intervalEvents_.push_back(std::move(event));
                }
            }
        }

        const auto current = sample_sprite_clip(source, instance.desc.clip, instance.timeSeconds);
        if (!current) continue;
        instance.previousFrame = current->frame;
        if (current->finished) instance.desc.playing = false;
    }
}

SpriteRenderList SpriteRuntime::build_render_list(Float3 cameraOrigin) const {
    return build_render_list(cameraOrigin, {});
}

SpriteRenderList SpriteRuntime::build_render_list(
    Float3 cameraOrigin, std::span<const SpriteBlendRenderOverride> overrides) const {
    SpriteRenderList result;
    result.items.reserve(instances_.size() + overrides.size());

    const auto append_item = [&](SpriteOwnerId owner, const Instance& instance,
                                 const SpriteAsset& source, const SpriteSample& current,
                                 float blendWeight, bool crossFade) {
        if (current.frame >= source.frames.size() || blendWeight <= 0.0F) return;
        const SpriteFrame& frame = source.frames[current.frame];
        RigidTransform transform = instance.desc.transform;
        if (instance.desc.pixelSnap) {
            transform.position = snap_world_to_pixel(
                transform.position, cameraOrigin, source.pixelsPerWorldUnit, instance.desc.plane);
        }
        const float left = (static_cast<float>(frame.sourceOffsetX) - frame.pivotPixels.x) /
            source.pixelsPerWorldUnit;
        const float bottom = (static_cast<float>(frame.sourceOffsetY) - frame.pivotPixels.y) /
            source.pixelsPerWorldUnit;
        const float right = left + static_cast<float>(frame.atlasRect.width) /
            source.pixelsPerWorldUnit;
        const float top = bottom + static_cast<float>(frame.atlasRect.height) /
            source.pixelsPerWorldUnit;
        const std::array<SpriteVec2, 4> local = {{{left, bottom}, {right, bottom},
                                                   {right, top}, {left, top}}};
        float u0 = static_cast<float>(frame.atlasRect.x) / static_cast<float>(source.textureWidth);
        float u1 = static_cast<float>(frame.atlasRect.x + frame.atlasRect.width) /
            static_cast<float>(source.textureWidth);
        float v0 = static_cast<float>(frame.atlasRect.y) / static_cast<float>(source.textureHeight);
        float v1 = static_cast<float>(frame.atlasRect.y + frame.atlasRect.height) /
            static_cast<float>(source.textureHeight);
        if (instance.desc.flipX) std::swap(u0, u1);
        if (instance.desc.flipY) std::swap(v0, v1);
        const std::array<SpriteVec2, 4> uv = {{{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}}};

        SpriteDrawItem item;
        item.owner = owner;
        item.asset = instance.desc.asset;
        item.frame = current.frame;
        item.textureAsset = source.textureAsset;
        item.materialId = source.materialId;
        item.paletteBank = instance.desc.paletteBankOverride.value_or(source.paletteBank);
        item.paletteAsset = source.paletteAsset;
        if (!source.paletteAsset.empty()) {
            const SpritePaletteAsset& paletteValue = palettes_.at(source.paletteAsset);
            SpritePalettePacket packet;
            packet.paletteAsset = source.paletteAsset;
            const std::uint64_t effectiveTicks =
                paletteClockTicks_ > std::numeric_limits<std::uint64_t>::max() -
                    instance.desc.palettePhaseTicks
                ? std::numeric_limits<std::uint64_t>::max()
                : paletteClockTicks_ + instance.desc.palettePhaseTicks;
            const SpritePaletteResolveDesc resolve{
                item.paletteBank, effectiveTicks, instance.desc.paletteSwaps};
            std::string ignored;
            if (resolve_sprite_palette(paletteValue, resolve, packet, &ignored)) {
                packet.paletteAsset = source.paletteAsset;
                const auto existing = std::find_if(
                    result.palettes.begin(), result.palettes.end(),
                    [&packet](const SpritePalettePacket& candidate) {
                        return same_palette_packet(candidate, packet);
                    });
                if (existing == result.palettes.end()) {
                    item.palettePacket = result.palettes.size();
                    result.palettes.push_back(std::move(packet));
                } else {
                    item.palettePacket = static_cast<std::size_t>(
                        std::distance(result.palettes.begin(), existing));
                }
                item.paletteStateHash = result.palettes[item.palettePacket].stateHash;
            }
        }
        item.sampling = source.sampling;
        item.blendMode = crossFade ? SpriteBlendMode::Alpha : instance.desc.blendMode;
        item.plane = instance.desc.plane;
        item.sortingLayer = instance.desc.sortingLayer;
        item.orderInLayer = instance.desc.orderInLayer;
        item.sortDepth = instance.desc.sortDepth;
        for (std::size_t vertex = 0U; vertex < item.vertices.size(); ++vertex) {
            const Float3 localPosition = instance.desc.plane == GameplayPlane2D::XY
                ? Float3{local[vertex].x, local[vertex].y, 0.0F}
                : Float3{local[vertex].x, 0.0F, local[vertex].y};
            item.vertices[vertex].position = transform_point(transform, localPosition);
            item.vertices[vertex].uv = uv[vertex];
            item.vertices[vertex].color = instance.desc.color;
            item.vertices[vertex].color[3] *= std::clamp(blendWeight, 0.0F, 1.0F);
        }
        result.items.push_back(std::move(item));
    };

    for (const auto& [owner, instance] : instances_) {
        if (!instance.desc.visible) continue;
        const SpriteAsset& source = assets_.at(instance.desc.asset);
        const auto override = std::find_if(overrides.begin(), overrides.end(),
            [owner](const SpriteBlendRenderOverride& candidate) {
                return candidate.owner == owner;
            });
        if (override != overrides.end()) {
            const bool crossFade = override->source.has_value() && override->destination.has_value() &&
                override->sourceWeight > 0.0F && override->destinationWeight > 0.0F;
            if (override->source)
                append_item(owner, instance, source, *override->source,
                            override->sourceWeight, crossFade);
            if (override->destination)
                append_item(owner, instance, source, *override->destination,
                            override->destinationWeight, crossFade);
            continue;
        }
        const auto current = sample_sprite_clip(source, instance.desc.clip, instance.timeSeconds);
        if (current) append_item(owner, instance, source, *current, 1.0F, false);
    }
    std::stable_sort(result.items.begin(), result.items.end(), [](const SpriteDrawItem& left,
                                                                  const SpriteDrawItem& right) {
        return visual_2d_less(
            {left.owner, Visual2DKind::Sprite, left.sortingLayer, left.orderInLayer, left.sortDepth, 0U},
            {right.owner, Visual2DKind::Sprite, right.sortingLayer, right.orderInLayer, right.sortDepth, 0U});
    });
    for (std::size_t index = 0U; index < result.items.size(); ++index) {
        const SpriteDrawItem& item = result.items[index];
        if (result.batches.empty() || !same_batch(item, result.batches.back())) {
            result.batches.push_back({item.textureAsset, item.materialId, item.paletteBank,
                item.sampling, item.blendMode, index, 1U, item.paletteAsset,
                item.paletteStateHash, item.palettePacket});
        } else {
            ++result.batches.back().itemCount;
        }
    }
    return result;
}

std::optional<SpriteSample> SpriteRuntime::sample(SpriteOwnerId owner) const noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return std::nullopt;
    const SpriteAsset& source = assets_.at(found->second.desc.asset);
    return sample_sprite_clip(source, found->second.desc.clip, found->second.timeSeconds);
}

std::optional<SpriteTrackSample> SpriteRuntime::sample_tracks(SpriteOwnerId owner) const noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return std::nullopt;
    return sample_tracks(owner, found->second.desc.clip, found->second.timeSeconds);
}

std::optional<SpriteTrackSample> SpriteRuntime::sample_tracks(
    SpriteOwnerId owner, std::string_view clip, float timeSeconds) const noexcept {
    const auto found = instances_.find(owner);
    if (found == instances_.end()) return std::nullopt;
    const Instance& instance = found->second;
    const SpriteAsset& source = assets_.at(instance.desc.asset);
    std::optional<SpriteTrackSample> result = sample_sprite_clip_tracks(source, clip, timeSeconds);
    if (!result) return std::nullopt;

    for (SpriteSocketSample& socket : result->sockets) {
        socket.positionPixels = mirrored_point(
            socket.positionPixels, instance.desc.flipX, instance.desc.flipY);
        socket.rotationDegrees = mirrored_rotation(
            socket.rotationDegrees, instance.desc.flipX, instance.desc.flipY);
        if (instance.desc.flipX) socket.scale.x = -socket.scale.x;
        if (instance.desc.flipY) socket.scale.y = -socket.scale.y;
        const RigidTransform local = local_2d_transform(
            socket.positionPixels, socket.rotationDegrees,
            source.pixelsPerWorldUnit, instance.desc.plane);
        socket.worldTransform = compose_rigid_transforms(instance.desc.transform, local);
    }
    for (SpriteCombatVolumeSample& volume : result->combatVolumes) {
        volume.volume.centerPixels = mirrored_point(
            volume.volume.centerPixels, instance.desc.flipX, instance.desc.flipY);
        volume.volume.rotationDegrees = mirrored_rotation(
            volume.volume.rotationDegrees, instance.desc.flipX, instance.desc.flipY);
        volume.volume.knockbackPixelsPerSecond = mirrored_point(
            volume.volume.knockbackPixelsPerSecond, instance.desc.flipX, instance.desc.flipY);
        const RigidTransform local = local_2d_transform(
            volume.volume.centerPixels, volume.volume.rotationDegrees,
            source.pixelsPerWorldUnit, instance.desc.plane);
        volume.worldTransform = compose_rigid_transforms(instance.desc.transform, local);
    }
    result->rootMotionDeltaPixels = mirrored_point(
        result->rootMotionDeltaPixels, instance.desc.flipX, instance.desc.flipY);
    result->rootMotionRotationDegrees = mirrored_rotation(
        result->rootMotionRotationDegrees, instance.desc.flipX, instance.desc.flipY);
    return result;
}

std::vector<SpriteOwnerId> SpriteRuntime::owners() const {
    std::vector<SpriteOwnerId> result;
    result.reserve(instances_.size());
    for (const auto& [owner, instance] : instances_) {
        (void)instance;
        result.push_back(owner);
    }
    return result;
}

} // namespace dve
