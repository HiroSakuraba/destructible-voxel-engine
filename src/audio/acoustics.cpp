#include "dve/audio/acoustics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

namespace dve::audio {

CoarseAcousticGrid::CoarseAcousticGrid(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                       float cellSizeMeters, AudioVec3 origin)
    : width_(std::max(1U, width)), height_(std::max(1U, height)), depth_(std::max(1U, depth)),
      cellSize_(std::clamp(cellSizeMeters, 0.05F, 10.0F)), origin_(origin),
      cells_(static_cast<std::size_t>(width_) * height_ * depth_) {}

std::size_t CoarseAcousticGrid::index(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept {
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(width_) *
           (static_cast<std::size_t>(y) + static_cast<std::size_t>(height_) * z);
}

bool CoarseAcousticGrid::set_cell(std::uint32_t x, std::uint32_t y, std::uint32_t z, AcousticCell cellValue) noexcept {
    if (x >= width_ || y >= height_ || z >= depth_) return false;
    cells_[index(x, y, z)] = cellValue;
    ++generation_;
    return true;
}

std::optional<AcousticCell> CoarseAcousticGrid::cell(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept {
    if (x >= width_ || y >= height_ || z >= depth_) return std::nullopt;
    return cells_[index(x, y, z)];
}

AcousticTraceResult CoarseAcousticGrid::trace(AudioVec3 from, AudioVec3 to, std::uint32_t maxSamples) const noexcept {
    const float dx = to.x - from.x; const float dy = to.y - from.y; const float dz = to.z - from.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const std::uint32_t desired = static_cast<std::uint32_t>(std::ceil(distance / std::max(0.01F, cellSize_ * 0.35F))) + 1U;
    const std::uint32_t samples = std::clamp(desired, 2U, std::max(2U, maxSamples));
    float occupiedFraction{};
    float transmission = 1.0F;
    std::uint32_t occupied{};
    for (std::uint32_t i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples - 1U);
        const AudioVec3 p{from.x + dx * t, from.y + dy * t, from.z + dz * t};
        const int gx = static_cast<int>(std::floor((p.x - origin_.x) / cellSize_));
        const int gy = static_cast<int>(std::floor((p.y - origin_.y) / cellSize_));
        const int gz = static_cast<int>(std::floor((p.z - origin_.z) / cellSize_));
        if (gx < 0 || gy < 0 || gz < 0 || gx >= static_cast<int>(width_) ||
            gy >= static_cast<int>(height_) || gz >= static_cast<int>(depth_)) continue;
        const AcousticCell value = cells_[index(static_cast<std::uint32_t>(gx), static_cast<std::uint32_t>(gy), static_cast<std::uint32_t>(gz))];
        const float occupancy = static_cast<float>(value.occupancy) / 255.0F;
        const float porosity = static_cast<float>(value.porosity) / 255.0F;
        occupiedFraction += occupancy;
        if (occupancy > 0.05F) {
            ++occupied;
            transmission *= std::clamp(1.0F - occupancy * (0.82F - porosity * 0.65F), 0.05F, 1.0F);
        }
    }
    AcousticTraceResult result;
    result.totalSamples = samples;
    result.occupiedSamples = occupied;
    result.occlusion = std::clamp(occupiedFraction / static_cast<float>(samples) * 3.0F, 0.0F, 1.0F);
    result.transmission = std::clamp(transmission, 0.0F, 1.0F);
    result.wallThicknessMeters = static_cast<float>(occupied) * (distance / static_cast<float>(samples));
    return result;
}

bool RoomPortalGraph::add_room(AcousticRoom room, std::string* error) {
    if (room.id == 0U || room.volumeCubicMeters < 0.0F ||
        std::any_of(rooms_.begin(), rooms_.end(), [&](const AcousticRoom& existing) { return existing.id == room.id; })) {
        if (error) *error = "invalid or duplicate acoustic room";
        return false;
    }
    room.absorption = std::clamp(room.absorption, 0.0F, 1.0F);
    rooms_.push_back(std::move(room));
    ++generation_;
    return true;
}

bool RoomPortalGraph::add_portal(AcousticPortal portal, std::string* error) {
    const auto roomExists = [&](std::uint32_t id) {
        return std::any_of(rooms_.begin(), rooms_.end(), [&](const AcousticRoom& room) { return room.id == id; });
    };
    if (portal.id == 0U || portal.roomA == portal.roomB || !roomExists(portal.roomA) || !roomExists(portal.roomB) ||
        std::any_of(portals_.begin(), portals_.end(), [&](const AcousticPortal& existing) { return existing.id == portal.id; })) {
        if (error) *error = "invalid acoustic portal";
        return false;
    }
    portal.openness = std::clamp(portal.openness, 0.0F, 1.0F);
    portal.transmission = std::clamp(portal.transmission, 0.0F, 1.0F);
    portal.openingSquareMeters = std::max(0.0F, portal.openingSquareMeters);
    portals_.push_back(portal);
    ++generation_;
    return true;
}

bool RoomPortalGraph::set_portal_openness(std::uint32_t portalId, float openness) noexcept {
    for (auto& portal : portals_) if (portal.id == portalId) {
        portal.openness = std::clamp(openness, 0.0F, 1.0F);
        ++generation_;
        return true;
    }
    return false;
}

float RoomPortalGraph::path_transmission(std::uint32_t fromRoom, std::uint32_t toRoom, std::size_t maxHops) const noexcept {
    if (fromRoom == toRoom) return 1.0F;
    struct State { std::uint32_t room{}; float transmission{}; std::size_t hops{}; };
    std::queue<State> pending;
    std::unordered_map<std::uint32_t, float> best;
    pending.push({fromRoom, 1.0F, 0U}); best[fromRoom] = 1.0F;
    while (!pending.empty()) {
        const State state = pending.front(); pending.pop();
        if (state.hops >= maxHops) continue;
        for (const auto& portal : portals_) {
            std::uint32_t next{};
            if (portal.roomA == state.room) next = portal.roomB;
            else if (portal.roomB == state.room) next = portal.roomA;
            else continue;
            const float edge = std::clamp(portal.openness * portal.transmission *
                                          std::min(1.0F, std::sqrt(std::max(0.0F, portal.openingSquareMeters))), 0.0F, 1.0F);
            const float candidate = state.transmission * edge;
            if (candidate <= best[next]) continue;
            best[next] = candidate;
            if (next == toRoom) continue;
            pending.push({next, candidate, state.hops + 1U});
        }
    }
    const auto found = best.find(toRoom);
    return found == best.end() ? 0.0F : found->second;
}

SpatializationResult GridAwareSpatializer::spatialize(const AudioListenerState& listener,
                                                       const AudioEmitterState& emitter) const noexcept {
    AudioEmitterState adjusted = emitter;
    if (grid_) {
        const AcousticTraceResult trace = grid_->trace(listener.position, emitter.position);
        adjusted.occlusion = std::max(adjusted.occlusion, trace.occlusion);
        adjusted.transmission *= trace.transmission;
        adjusted.reverbSend = std::clamp(adjusted.reverbSend + trace.occlusion * 0.25F, 0.0F, 1.0F);
    }
    return fallback_.spatialize(listener, adjusted);
}

} // namespace dve::audio
