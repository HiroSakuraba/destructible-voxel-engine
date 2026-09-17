#include "dve/sprite_particle_renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace dve {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

[[nodiscard]] bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] SpriteVec2 plane_point(Float3 value, GameplayPlane2D plane) noexcept {
    return plane == GameplayPlane2D::XY ? SpriteVec2{value.x, value.y}
                                       : SpriteVec2{value.x, value.z};
}

[[nodiscard]] Float3 from_plane(SpriteVec2 point, float depth, GameplayPlane2D plane) noexcept {
    return plane == GameplayPlane2D::XY ? Float3{point.x, point.y, depth}
                                       : Float3{point.x, depth, point.y};
}

void append_quad(SpriteRenderList& list, SpriteOwnerId owner, std::string texture,
                 SpriteSampling sampling, SpriteBlendMode blend, std::int32_t layer,
                 std::int32_t order, float depth, GameplayPlane2D plane,
                 SpriteVec2 center, SpriteVec2 axis, SpriteVec2 perpendicular,
                 SpriteVec2 uvMin, SpriteVec2 uvMax, const std::array<float, 4>& color) {
    SpriteDrawItem item;
    item.owner = owner;
    item.asset = kInvalidSpriteAssetId;
    item.frame = 0U;
    item.textureAsset = std::move(texture);
    item.sampling = sampling;
    item.blendMode = blend;
    item.plane = plane;
    item.sortingLayer = layer;
    item.orderInLayer = order;
    item.sortDepth = depth;
    const std::array<SpriteVec2, 4> positions{
        SpriteVec2{center.x - axis.x - perpendicular.x, center.y - axis.y - perpendicular.y},
        SpriteVec2{center.x + axis.x - perpendicular.x, center.y + axis.y - perpendicular.y},
        SpriteVec2{center.x + axis.x + perpendicular.x, center.y + axis.y + perpendicular.y},
        SpriteVec2{center.x - axis.x + perpendicular.x, center.y - axis.y + perpendicular.y},
    };
    const std::array<SpriteVec2, 4> uvs{
        SpriteVec2{uvMin.x, uvMax.y}, SpriteVec2{uvMax.x, uvMax.y},
        SpriteVec2{uvMax.x, uvMin.y}, SpriteVec2{uvMin.x, uvMin.y},
    };
    for (std::size_t index = 0U; index < 4U; ++index) {
        item.vertices[index].position = from_plane(positions[index], depth, plane);
        item.vertices[index].uv = uvs[index];
        item.vertices[index].color = color;
    }
    list.items.push_back(std::move(item));
}

void rebuild_batches(SpriteRenderList& list) {
    list.batches.clear();
    for (std::size_t index = 0U; index < list.items.size(); ++index) {
        const SpriteDrawItem& item = list.items[index];
        if (!list.batches.empty()) {
            SpriteBatch& batch = list.batches.back();
            if (batch.textureAsset == item.textureAsset && batch.sampling == item.sampling &&
                batch.blendMode == item.blendMode && batch.materialId == item.materialId &&
                batch.paletteAsset == item.paletteAsset && batch.paletteBank == item.paletteBank) {
                ++batch.itemCount;
                continue;
            }
        }
        SpriteBatch batch;
        batch.textureAsset = item.textureAsset;
        batch.materialId = item.materialId;
        batch.paletteBank = item.paletteBank;
        batch.sampling = item.sampling;
        batch.blendMode = item.blendMode;
        batch.firstItem = index;
        batch.itemCount = 1U;
        list.batches.push_back(std::move(batch));
    }
}

[[nodiscard]] std::uint8_t byte_color(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

} // namespace

bool SpriteParticleStyle::validate(std::string* error) const {
    if (textureAsset.empty() || textureAsset.size() > 4096U || atlasColumns == 0U ||
        atlasRows == 0U || atlasColumns > 1024U || atlasRows > 1024U ||
        !std::isfinite(flipbookFramesPerSecond) || flipbookFramesPerSecond < 0.0F ||
        !std::isfinite(pixelsPerWorldUnit) || pixelsPerWorldUnit <= 0.0F ||
        !std::isfinite(baseSizePixels) || baseSizePixels <= 0.0F ||
        !std::isfinite(velocityStretch) || velocityStretch < 0.0F ||
        !std::isfinite(rotationRateDegrees))
        return fail(error, "sprite particle style is invalid");
    return true;
}

std::optional<SpriteParticleEmitterId> SpriteParticleSystem::spawn(
    VfxProgram program, SpriteParticleStyle style, Float3 origin, std::string* error) {
    if (emitters_.size() >= maximumEmitters_)
        return std::nullopt;
    if (!program.validate(error) || !style.validate(error) || !finite(origin)) return std::nullopt;
    const SpriteParticleEmitterId id = nextId_++;
    emitters_.emplace(std::piecewise_construct, std::forward_as_tuple(id),
        std::forward_as_tuple(id, std::move(program), std::move(style), origin));
    return id;
}

bool SpriteParticleSystem::remove(SpriteParticleEmitterId id) noexcept {
    return emitters_.erase(id) != 0U;
}

bool SpriteParticleSystem::set_origin(SpriteParticleEmitterId id, Float3 origin) noexcept {
    const auto found = emitters_.find(id);
    if (found == emitters_.end() || !finite(origin)) return false;
    found->second.origin = origin;
    return true;
}

bool SpriteParticleSystem::set_active(SpriteParticleEmitterId id, bool active) noexcept {
    const auto found = emitters_.find(id);
    if (found == emitters_.end()) return false;
    found->second.active = active;
    return true;
}

bool SpriteParticleSystem::contains(SpriteParticleEmitterId id) const noexcept {
    return emitters_.contains(id);
}

void SpriteParticleSystem::step(float deltaSeconds) {
    if (!(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds)) return;
    for (auto& [id, emitter] : emitters_) {
        (void)id;
        if (emitter.active) (void)emitter.runtime.step(deltaSeconds);
    }
}

SpriteParticleRenderPacket SpriteParticleSystem::build_render_packet(
    Float3 cameraOrigin, GameplayPlane2D plane) const {
    SpriteParticleRenderPacket packet;
    packet.stats.emitters = emitters_.size();
    for (const auto& [id, emitter] : emitters_) {
        const SpriteParticleStyle& style = emitter.style;
        const std::uint32_t frameCount = style.atlasColumns * style.atlasRows;
        for (const VfxParticleState& particle : emitter.runtime.particles()) {
            if (!particle.alive) continue;
            ++packet.stats.particles;
            const Float3 world = add(emitter.origin, particle.position);
            const SpriteVec2 center = plane_point(world, plane);
            const SpriteVec2 velocity = plane_point(particle.velocity, plane);
            const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
            const float angle = speed > 1.0e-5F ? std::atan2(velocity.y, velocity.x) : 0.0F;
            const float rotation = angle + particle.age * style.rotationRateDegrees *
                0.01745329251994329577F;
            const float halfWidth = style.baseSizePixels * std::max(0.01F, particle.size) /
                style.pixelsPerWorldUnit * 0.5F + speed * style.velocityStretch * 0.5F;
            const float halfHeight = style.baseSizePixels * std::max(0.01F, particle.size) /
                style.pixelsPerWorldUnit * 0.5F;
            const SpriteVec2 axis{std::cos(rotation) * halfWidth, std::sin(rotation) * halfWidth};
            const SpriteVec2 perpendicular{-std::sin(rotation) * halfHeight,
                                           std::cos(rotation) * halfHeight};
            const std::uint32_t frame = frameCount == 0U ? 0U :
                static_cast<std::uint32_t>(std::floor(particle.age * style.flipbookFramesPerSecond)) % frameCount;
            const std::uint32_t column = frame % style.atlasColumns;
            const std::uint32_t row = frame / style.atlasColumns;
            const SpriteVec2 uvMin{static_cast<float>(column) / static_cast<float>(style.atlasColumns),
                                   static_cast<float>(row) / static_cast<float>(style.atlasRows)};
            const SpriteVec2 uvMax{static_cast<float>(column + 1U) / static_cast<float>(style.atlasColumns),
                                   static_cast<float>(row + 1U) / static_cast<float>(style.atlasRows)};
            const float depth = plane == GameplayPlane2D::XY ? world.z - cameraOrigin.z
                                                              : world.y - cameraOrigin.y;
            append_quad(packet.renderList, id, style.textureAsset, style.sampling,
                style.blendMode, style.sortingLayer, style.orderInLayer, depth, plane,
                center, axis, perpendicular, uvMin, uvMax, particle.color);
        }
    }
    for (const SpriteParticleTrailDesc& trail : trails_) {
        for (std::size_t index = 1U; index < trail.points.size(); ++index) {
            const SpriteVec2 a = plane_point(trail.points[index - 1U].position, plane);
            const SpriteVec2 b = plane_point(trail.points[index].position, plane);
            const SpriteVec2 delta{b.x - a.x, b.y - a.y};
            const float lengthValue = std::sqrt(delta.x * delta.x + delta.y * delta.y);
            if (lengthValue <= 1.0e-5F) continue;
            const SpriteVec2 center{(a.x + b.x) * 0.5F, (a.y + b.y) * 0.5F};
            const SpriteVec2 axis{delta.x * 0.5F, delta.y * 0.5F};
            const float halfWidth = trail.widthPixels / trail.pixelsPerWorldUnit * 0.5F;
            const SpriteVec2 perpendicular{-delta.y / lengthValue * halfWidth,
                                            delta.x / lengthValue * halfWidth};
            append_quad(packet.renderList, trail.owner, trail.textureAsset, SpriteSampling::Linear,
                SpriteBlendMode::Alpha, trail.sortingLayer, trail.orderInLayer, 0.0F, plane,
                center, axis, perpendicular, {0.0F, 0.0F}, {1.0F, 1.0F}, trail.color);
            ++packet.stats.trailSegments;
        }
    }
    for (const SpriteParticleBeamDesc& beam : beams_) {
        const SpriteVec2 a = plane_point(beam.start, plane);
        const SpriteVec2 b = plane_point(beam.end, plane);
        const SpriteVec2 delta{b.x - a.x, b.y - a.y};
        const float lengthValue = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        if (lengthValue <= 1.0e-5F) continue;
        const SpriteVec2 center{(a.x + b.x) * 0.5F, (a.y + b.y) * 0.5F};
        const SpriteVec2 axis{delta.x * 0.5F, delta.y * 0.5F};
        const float halfWidth = beam.widthPixels / beam.pixelsPerWorldUnit * 0.5F;
        const SpriteVec2 perpendicular{-delta.y / lengthValue * halfWidth,
                                        delta.x / lengthValue * halfWidth};
        append_quad(packet.renderList, beam.owner, beam.textureAsset, SpriteSampling::Linear,
            SpriteBlendMode::Additive, beam.sortingLayer, beam.orderInLayer, 0.0F, plane,
            center, axis, perpendicular, {0.0F, 0.0F}, {1.0F, 1.0F}, beam.color);
        ++packet.stats.beams;
    }
    std::stable_sort(packet.renderList.items.begin(), packet.renderList.items.end(),
        [](const SpriteDrawItem& left, const SpriteDrawItem& right) {
            const Visual2DSortEntry a{left.owner, Visual2DKind::Particle, left.sortingLayer,
                                      left.orderInLayer, left.sortDepth, 0U};
            const Visual2DSortEntry b{right.owner, Visual2DKind::Particle, right.sortingLayer,
                                      right.orderInLayer, right.sortDepth, 0U};
            return visual_2d_less(a, b);
        });
    rebuild_batches(packet.renderList);
    packet.stats.batches = packet.renderList.batches.size();
    return packet;
}

std::vector<SpriteParticleEmitterId> SpriteParticleSystem::spawn_from_events(
    std::span<const SpriteIntervalEvent> events,
    const std::map<std::string, std::pair<VfxProgram, SpriteParticleStyle>, std::less<>>& registry,
    const std::map<SpriteOwnerId, Float3>& ownerPositions, std::string* error) {
    std::vector<SpriteParticleEmitterId> result;
    for (const SpriteIntervalEvent& event : events) {
        const auto definition = registry.find(event.name);
        const auto owner = ownerPositions.find(event.owner);
        if (definition == registry.end() || owner == ownerPositions.end()) continue;
        const auto id = spawn(definition->second.first, definition->second.second, owner->second, error);
        if (!id) return result;
        result.push_back(*id);
    }
    return result;
}

std::vector<SpriteParticleEmitterId> SpriteParticleSystem::spawn_from_sockets(
    std::span<const SpriteSocketAttachmentSample> sockets, std::string_view socketName,
    const VfxProgram& program, const SpriteParticleStyle& style, std::string* error) {
    std::vector<SpriteParticleEmitterId> result;
    for (const SpriteSocketAttachmentSample& socket : sockets) {
        if (socket.socket != socketName) continue;
        const auto id = spawn(program, style, socket.worldTransform.position, error);
        if (!id) return result;
        result.push_back(*id);
    }
    return result;
}

SpriteParticleReferenceImage rasterize_sprite_particles_reference(
    const SpriteParticleRenderPacket& packet, std::uint32_t width, std::uint32_t height,
    float pixelsPerWorldUnit, Float3 cameraOrigin) {
    SpriteParticleReferenceImage image;
    image.width = width;
    image.height = height;
    if (width == 0U || height == 0U || !std::isfinite(pixelsPerWorldUnit) || pixelsPerWorldUnit <= 0.0F)
        return image;
    image.rgba8.assign(static_cast<std::size_t>(width) * height * 4U, std::byte{0});
    for (const SpriteDrawItem& item : packet.renderList.items) {
        float minimumX = item.vertices[0].position.x;
        float maximumX = minimumX;
        float minimumY = item.vertices[0].position.y;
        float maximumY = minimumY;
        for (const SpriteVertex& vertex : item.vertices) {
            minimumX = std::min(minimumX, vertex.position.x);
            maximumX = std::max(maximumX, vertex.position.x);
            minimumY = std::min(minimumY, vertex.position.y);
            maximumY = std::max(maximumY, vertex.position.y);
        }
        const int x0 = static_cast<int>(std::floor((minimumX - cameraOrigin.x) * pixelsPerWorldUnit +
                                                   static_cast<float>(width) * 0.5F));
        const int x1 = static_cast<int>(std::ceil((maximumX - cameraOrigin.x) * pixelsPerWorldUnit +
                                                 static_cast<float>(width) * 0.5F));
        const int y0 = static_cast<int>(std::floor(static_cast<float>(height) * 0.5F -
                                                   (maximumY - cameraOrigin.y) * pixelsPerWorldUnit));
        const int y1 = static_cast<int>(std::ceil(static_cast<float>(height) * 0.5F -
                                                  (minimumY - cameraOrigin.y) * pixelsPerWorldUnit));
        const auto color = item.vertices[0].color;
        for (int y = std::max(0, y0); y < std::min(static_cast<int>(height), y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(static_cast<int>(width), x1); ++x) {
                const std::size_t pixel = (static_cast<std::size_t>(y) * width +
                                           static_cast<std::size_t>(x)) * 4U;
                image.rgba8[pixel] = std::byte{byte_color(color[0])};
                image.rgba8[pixel + 1U] = std::byte{byte_color(color[1])};
                image.rgba8[pixel + 2U] = std::byte{byte_color(color[2])};
                image.rgba8[pixel + 3U] = std::byte{byte_color(color[3])};
            }
        }
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte value : image.rgba8) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    image.contentHash = hash;
    return image;
}

} // namespace dve
