#include "dve/audio/acoustic_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace dve::audio {
namespace {

std::size_t update_index(const AcousticBrickUpdate& update, std::uint32_t x,
                         std::uint32_t y, std::uint32_t z) noexcept {
    return (static_cast<std::size_t>(z) * update.height + y) * update.width + x;
}

std::vector<AcousticSurfaceBox> derive_surfaces(const CoarseAcousticGrid& grid,
                                                std::uint64_t& occupiedCells) {
    std::vector<AcousticSurfaceBox> surfaces;
    occupiedCells = 0U;
    const float cell = grid.cell_size_meters();
    const auto origin = grid.origin();
    for (std::uint32_t z = 0; z < grid.depth(); ++z) {
        for (std::uint32_t y = 0; y < grid.height(); ++y) {
            std::uint32_t x = 0U;
            while (x < grid.width()) {
                const auto first = grid.cell(x, y, z);
                if (!first || first->occupancy == 0U) { ++x; continue; }
                const std::uint8_t material = first->material;
                const std::uint32_t begin = x;
                while (x < grid.width()) {
                    const auto current = grid.cell(x, y, z);
                    if (!current || current->occupancy == 0U || current->material != material) break;
                    ++occupiedCells;
                    ++x;
                }
                surfaces.push_back({
                    {origin.x + static_cast<float>(begin) * cell, origin.y + static_cast<float>(y) * cell, origin.z + static_cast<float>(z) * cell},
                    {origin.x + static_cast<float>(x) * cell, origin.y + static_cast<float>(y + 1U) * cell, origin.z + static_cast<float>(z + 1U) * cell},
                    material});
            }
        }
    }
    return surfaces;
}

float portal_openness(const CoarseAcousticGrid& grid, const AcousticPortalCandidate& portal) noexcept {
    const std::uint32_t maxX = std::min(portal.maxX, grid.width());
    const std::uint32_t maxY = std::min(portal.maxY, grid.height());
    const std::uint32_t maxZ = std::min(portal.maxZ, grid.depth());
    std::uint64_t total{};
    std::uint64_t open{};
    for (std::uint32_t z = std::min(portal.minZ, maxZ); z < maxZ; ++z)
        for (std::uint32_t y = std::min(portal.minY, maxY); y < maxY; ++y)
            for (std::uint32_t x = std::min(portal.minX, maxX); x < maxX; ++x) {
                ++total;
                const auto cell = grid.cell(x, y, z);
                if (!cell || cell->occupancy < 64U || cell->porosity > 192U) ++open;
            }
    return total == 0U ? 0.0F : static_cast<float>(open) / static_cast<float>(total);
}

} // namespace

struct AsyncAcousticPublisher::State {
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable idleCondition;
    std::optional<AcousticBuildRequest> pending;
    std::atomic<std::shared_ptr<const AcousticSnapshot>> published;
    std::jthread worker;
    std::uint64_t nextPublication{1U};
    std::uint64_t requested{};
    std::uint64_t publishedGeneration{};
    std::uint64_t discarded{};
    bool working{};
    std::string error;
};

AsyncAcousticPublisher::AsyncAcousticPublisher() : state_(std::make_unique<State>()) {
    state_->worker = std::jthread([this](std::stop_token stop) { worker_loop(stop); });
}
AsyncAcousticPublisher::~AsyncAcousticPublisher() {
    state_->worker.request_stop();
    state_->condition.notify_all();
}

std::uint64_t AsyncAcousticPublisher::submit(AcousticBuildRequest request) {
    std::lock_guard lock(state_->mutex);
    if (request.sourceGeneration == 0U) request.sourceGeneration = state_->requested + 1U;
    if (request.sourceGeneration < state_->requested) {
        ++state_->discarded;
        return state_->requested;
    }
    state_->requested = request.sourceGeneration;
    if (state_->pending) ++state_->discarded;
    state_->pending = std::move(request);
    state_->error.clear();
    state_->condition.notify_one();
    return state_->requested;
}
void AsyncAcousticPublisher::cancel_pending() {
    std::lock_guard lock(state_->mutex);
    if (state_->pending) { state_->pending.reset(); ++state_->discarded; }
    if (!state_->working) state_->idleCondition.notify_all();
}
void AsyncAcousticPublisher::wait_idle() {
    std::unique_lock lock(state_->mutex);
    state_->idleCondition.wait(lock, [&] { return !state_->working && !state_->pending; });
}
std::shared_ptr<const AcousticSnapshot> AsyncAcousticPublisher::snapshot() const noexcept {
    return state_->published.load(std::memory_order_acquire);
}
AcousticPublisherStatus AsyncAcousticPublisher::status() const {
    std::lock_guard lock(state_->mutex);
    return {state_->requested, state_->publishedGeneration, state_->discarded,
            state_->working, state_->pending.has_value(), state_->error};
}

void AsyncAcousticPublisher::worker_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        AcousticBuildRequest request;
        std::shared_ptr<const AcousticSnapshot> previous;
        {
            std::unique_lock lock(state_->mutex);
            state_->condition.wait(lock, [&] { return stop.stop_requested() || state_->pending.has_value(); });
            if (stop.stop_requested()) return;
            request = std::move(*state_->pending);
            state_->pending.reset();
            state_->working = true;
            previous = state_->published.load(std::memory_order_acquire);
        }
        const auto start = std::chrono::steady_clock::now();
        try {
            std::shared_ptr<CoarseAcousticGrid> grid;
            const bool compatiblePrevious = previous && previous->grid && !request.resetGrid &&
                previous->grid->width() == request.width && previous->grid->height() == request.height &&
                previous->grid->depth() == request.depth &&
                std::abs(previous->grid->cell_size_meters() - request.cellSizeMeters) < 1.0e-6F;
            if (compatiblePrevious) grid = std::make_shared<CoarseAcousticGrid>(*previous->grid);
            else grid = std::make_shared<CoarseAcousticGrid>(request.width, request.height, request.depth,
                                                              request.cellSizeMeters, request.origin);
            std::uint64_t changed{};
            for (const auto& update : request.dirtyBricks) {
                if (update.cells.size() != static_cast<std::size_t>(update.width) * update.height * update.depth)
                    continue;
                for (std::uint32_t z = 0; z < update.depth; ++z)
                    for (std::uint32_t y = 0; y < update.height; ++y)
                        for (std::uint32_t x = 0; x < update.width; ++x) {
                            const std::uint32_t gx = update.originX + x;
                            const std::uint32_t gy = update.originY + y;
                            const std::uint32_t gz = update.originZ + z;
                            if (gx >= grid->width() || gy >= grid->height() || gz >= grid->depth()) continue;
                            const auto next = update.cells[update_index(update, x, y, z)];
                            const auto old = grid->cell(gx, gy, gz);
                            if (!old || old->occupancy != next.occupancy || old->material != next.material ||
                                old->porosity != next.porosity || old->flags != next.flags) ++changed;
                            (void)grid->set_cell(gx, gy, gz, next);
                        }
            }
            std::uint64_t occupied{};
            auto surfaces = std::make_shared<std::vector<AcousticSurfaceBox>>(derive_surfaces(*grid, occupied));
            auto graph = std::make_shared<RoomPortalGraph>();
            for (const auto& room : request.rooms) (void)graph->add_room(room);
            std::uint32_t openPortals{};
            std::uint32_t closedPortals{};
            for (const auto& candidate : request.portalCandidates) {
                const float openness = portal_openness(*grid, candidate);
                if (openness >= 0.05F) ++openPortals; else ++closedPortals;
                (void)graph->add_portal({candidate.id, candidate.roomA, candidate.roomB,
                                        std::max(0.0F, candidate.areaSquareMeters), openness,
                                        std::clamp(candidate.transmission, 0.0F, 1.0F)});
            }
            auto snapshot = std::make_shared<AcousticSnapshot>();
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            {
                std::lock_guard lock(state_->mutex);
                snapshot->statistics = {request.sourceGeneration, state_->nextPublication++, occupied, changed,
                                        surfaces->size(), openPortals, closedPortals, elapsed};
                snapshot->grid = std::move(grid);
                snapshot->roomGraph = std::move(graph);
                snapshot->surfaces = std::move(surfaces);
                // Strict latest-request-wins publication: a completed result is discarded when a
                // newer source generation was requested while this build was in flight. This avoids
                // transiently publishing stale room/portal and occlusion state after destruction.
                if (request.sourceGeneration < state_->requested) {
                    ++state_->discarded;
                } else {
                    state_->published.store(snapshot, std::memory_order_release);
                    state_->publishedGeneration = snapshot->statistics.publicationGeneration;
                    state_->error.clear();
                }
                state_->working = false;
                if (!state_->pending) state_->idleCondition.notify_all();
            }
        } catch (const std::exception& exception) {
            std::lock_guard lock(state_->mutex);
            state_->error = exception.what();
            state_->working = false;
            if (!state_->pending) state_->idleCondition.notify_all();
        } catch (...) {
            std::lock_guard lock(state_->mutex);
            state_->error = "unknown acoustic build failure";
            state_->working = false;
            if (!state_->pending) state_->idleCondition.notify_all();
        }
    }
}

SpatializationResult PublishedAcousticSpatializer::spatialize(const AudioListenerState& listener,
                                                               const AudioEmitterState& emitter) const noexcept {
    AudioEmitterState adjusted = emitter;
    const auto snapshot = publisher_ ? publisher_->snapshot() : nullptr;
    if (snapshot && snapshot->grid) {
        const auto trace = snapshot->grid->trace(listener.position, emitter.position);
        adjusted.occlusion = std::max(adjusted.occlusion, trace.occlusion);
        adjusted.transmission = std::min(adjusted.transmission, trace.transmission);
        adjusted.reverbSend = std::clamp(adjusted.reverbSend + trace.occlusion * 0.18F, 0.0F, 1.0F);
    }
    return fallback_.spatialize(listener, adjusted);
}

} // namespace dve::audio
