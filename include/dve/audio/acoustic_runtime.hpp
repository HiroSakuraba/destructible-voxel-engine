#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "dve/audio/acoustics.hpp"

namespace dve::audio {

struct AcousticBrickUpdate {
    std::uint32_t originX{};
    std::uint32_t originY{};
    std::uint32_t originZ{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::vector<AcousticCell> cells;
};

struct AcousticPortalCandidate {
    std::uint32_t id{};
    std::uint32_t roomA{};
    std::uint32_t roomB{};
    std::uint32_t minX{};
    std::uint32_t minY{};
    std::uint32_t minZ{};
    std::uint32_t maxX{};
    std::uint32_t maxY{};
    std::uint32_t maxZ{};
    float areaSquareMeters{};
    float transmission{1.0F};
};

struct AcousticBuildRequest {
    std::uint64_t sourceGeneration{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    float cellSizeMeters{0.5F};
    AudioVec3 origin{};
    bool resetGrid{};
    std::vector<AcousticBrickUpdate> dirtyBricks;
    std::vector<AcousticRoom> rooms;
    std::vector<AcousticPortalCandidate> portalCandidates;
};

// A run-length box proxy is deliberately coarser than a render mesh. Adjacent occupied cells of
// the same material are merged along X; later backends can merge further or triangulate boxes.
struct AcousticSurfaceBox {
    AudioVec3 minimum{};
    AudioVec3 maximum{};
    std::uint8_t material{};
};

struct AcousticSnapshotStatistics {
    std::uint64_t sourceGeneration{};
    std::uint64_t publicationGeneration{};
    std::uint64_t occupiedCells{};
    std::uint64_t changedCells{};
    std::uint64_t surfaceBoxes{};
    std::uint32_t openPortals{};
    std::uint32_t closedPortals{};
    double buildMilliseconds{};
};

struct AcousticSnapshot {
    AcousticSnapshotStatistics statistics;
    std::shared_ptr<const CoarseAcousticGrid> grid;
    std::shared_ptr<const RoomPortalGraph> roomGraph;
    std::shared_ptr<const std::vector<AcousticSurfaceBox>> surfaces;
};

struct AcousticPublisherStatus {
    std::uint64_t requestedGeneration{};
    std::uint64_t publishedGeneration{};
    std::uint64_t discardedGenerations{};
    bool working{};
    bool hasPending{};
    std::string error;
};

// Latest-request-wins asynchronous publication. Voxel/game threads submit owned dirty-brick
// snapshots; the worker derives an immutable grid, simplified surfaces, and room/portal graph.
// The audio thread only atomically loads the latest shared snapshot.
class AsyncAcousticPublisher {
public:
    AsyncAcousticPublisher();
    ~AsyncAcousticPublisher();
    AsyncAcousticPublisher(const AsyncAcousticPublisher&) = delete;
    AsyncAcousticPublisher& operator=(const AsyncAcousticPublisher&) = delete;

    std::uint64_t submit(AcousticBuildRequest request);
    void cancel_pending();
    void wait_idle();
    [[nodiscard]] std::shared_ptr<const AcousticSnapshot> snapshot() const noexcept;
    [[nodiscard]] AcousticPublisherStatus status() const;
private:
    void worker_loop(std::stop_token stop);
    struct State;
    std::unique_ptr<State> state_;
};

// Spatializer that consumes immutable snapshots. No worker mutex is acquired on spatialize();
// loading the shared snapshot pins its lifetime through the query.
class PublishedAcousticSpatializer final : public IAudioSpatializer {
public:
    explicit PublishedAcousticSpatializer(std::shared_ptr<const AsyncAcousticPublisher> publisher) noexcept
        : publisher_(std::move(publisher)) {}
    [[nodiscard]] SpatializationResult spatialize(const AudioListenerState& listener,
                                                   const AudioEmitterState& emitter) const noexcept override;
private:
    std::shared_ptr<const AsyncAcousticPublisher> publisher_;
    AnalyticSpatializer fallback_;
};

} // namespace dve::audio
