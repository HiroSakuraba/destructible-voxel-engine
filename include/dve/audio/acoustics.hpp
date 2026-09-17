#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/audio/spatializer.hpp"

namespace dve::audio {

struct AcousticCell {
    std::uint8_t occupancy{};
    std::uint8_t material{};
    std::uint8_t porosity{};
    std::uint8_t flags{};
};

struct AcousticTraceResult {
    float occlusion{};
    float transmission{1.0F};
    float wallThicknessMeters{};
    std::uint32_t occupiedSamples{};
    std::uint32_t totalSamples{};
};

class CoarseAcousticGrid {
public:
    CoarseAcousticGrid(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                       float cellSizeMeters, AudioVec3 origin = {});

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }
    [[nodiscard]] float cell_size_meters() const noexcept { return cellSize_; }
    [[nodiscard]] AudioVec3 origin() const noexcept { return origin_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    bool set_cell(std::uint32_t x, std::uint32_t y, std::uint32_t z, AcousticCell cell) noexcept;
    [[nodiscard]] std::optional<AcousticCell> cell(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept;
    [[nodiscard]] AcousticTraceResult trace(AudioVec3 from, AudioVec3 to,
                                            std::uint32_t maxSamples = 256) const noexcept;

private:
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept;
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint32_t depth_{};
    float cellSize_{};
    AudioVec3 origin_{};
    std::vector<AcousticCell> cells_;
    std::uint64_t generation_{1U};
};

struct AcousticRoom {
    std::uint32_t id{};
    std::string name;
    float volumeCubicMeters{};
    float absorption{};
    bool exterior{};
};

struct AcousticPortal {
    std::uint32_t id{};
    std::uint32_t roomA{};
    std::uint32_t roomB{};
    float openingSquareMeters{};
    float openness{};
    float transmission{};
};

class RoomPortalGraph {
public:
    bool add_room(AcousticRoom room, std::string* error = nullptr);
    bool add_portal(AcousticPortal portal, std::string* error = nullptr);
    bool set_portal_openness(std::uint32_t portalId, float openness) noexcept;
    [[nodiscard]] const std::vector<AcousticRoom>& rooms() const noexcept { return rooms_; }
    [[nodiscard]] const std::vector<AcousticPortal>& portals() const noexcept { return portals_; }
    [[nodiscard]] float path_transmission(std::uint32_t fromRoom, std::uint32_t toRoom,
                                          std::size_t maxHops = 8) const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
private:
    std::vector<AcousticRoom> rooms_;
    std::vector<AcousticPortal> portals_;
    std::uint64_t generation_{1U};
};

// Wraps the analytic panner with coarse-grid obstruction. A production Steam Audio adapter can
// implement the same IAudioSpatializer boundary without leaking middleware types into the mixer.
class GridAwareSpatializer final : public IAudioSpatializer {
public:
    explicit GridAwareSpatializer(const CoarseAcousticGrid* grid = nullptr) noexcept : grid_(grid) {}
    void set_grid(const CoarseAcousticGrid* grid) noexcept { grid_ = grid; }
    [[nodiscard]] SpatializationResult spatialize(const AudioListenerState& listener,
                                                   const AudioEmitterState& emitter) const noexcept override;
private:
    const CoarseAcousticGrid* grid_{};
    AnalyticSpatializer fallback_{};
};

} // namespace dve::audio
