#include "dve/flip_liquid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-6F;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] std::size_t cell_index(const GridFluidDimensions& d,
                                     std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t z) noexcept {
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(d.x) *
        (static_cast<std::size_t>(y) + static_cast<std::size_t>(d.y) * z);
}
[[nodiscard]] std::size_t u_index(const GridFluidDimensions& d,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t z) noexcept {
    const std::size_t sx = static_cast<std::size_t>(d.x) + 1U;
    return static_cast<std::size_t>(x) + sx *
        (static_cast<std::size_t>(y) + static_cast<std::size_t>(d.y) * z);
}
[[nodiscard]] std::size_t v_index(const GridFluidDimensions& d,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t z) noexcept {
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(d.x) *
        (static_cast<std::size_t>(y) + (static_cast<std::size_t>(d.y) + 1U) * z);
}
[[nodiscard]] std::size_t w_index(const GridFluidDimensions& d,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t z) noexcept {
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(d.x) *
        (static_cast<std::size_t>(y) + static_cast<std::size_t>(d.y) * z);
}

[[nodiscard]] std::size_t u_count(const GridFluidDimensions& d) noexcept {
    return (static_cast<std::size_t>(d.x) + 1U) * d.y * d.z;
}
[[nodiscard]] std::size_t v_count(const GridFluidDimensions& d) noexcept {
    return static_cast<std::size_t>(d.x) * (static_cast<std::size_t>(d.y) + 1U) * d.z;
}
[[nodiscard]] std::size_t w_count(const GridFluidDimensions& d) noexcept {
    return static_cast<std::size_t>(d.x) * d.y * (static_cast<std::size_t>(d.z) + 1U);
}

[[nodiscard]] Float3 domain_maximum(const FlipLiquidSettings& settings) noexcept {
    return {static_cast<float>(settings.dimensions.x) * settings.cellSize,
            static_cast<float>(settings.dimensions.y) * settings.cellSize,
            static_cast<float>(settings.dimensions.z) * settings.cellSize};
}

[[nodiscard]] Float3 clamp_position(Float3 p, const FlipLiquidSettings& settings) noexcept {
    const Float3 maximum = domain_maximum(settings);
    const float margin = std::max(settings.particleRadius, settings.cellSize * 1.0e-3F);
    return {std::clamp(p.x, margin, maximum.x - margin),
            std::clamp(p.y, margin, maximum.y - margin),
            std::clamp(p.z, margin, maximum.z - margin)};
}

[[nodiscard]] std::array<std::uint32_t, 3> cell_coordinate(
    Float3 p, const FlipLiquidSettings& settings) noexcept {
    const auto clamp_axis = [](float value, float h, std::uint32_t n) {
        const auto raw = static_cast<std::int64_t>(std::floor(value / h));
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(raw, 0, n - 1));
    };
    return {clamp_axis(p.x, settings.cellSize, settings.dimensions.x),
            clamp_axis(p.y, settings.cellSize, settings.dimensions.y),
            clamp_axis(p.z, settings.cellSize, settings.dimensions.z)};
}

[[nodiscard]] Float3 cell_center(const FlipLiquidSettings& settings,
                                 std::uint32_t x,
                                 std::uint32_t y,
                                 std::uint32_t z) noexcept {
    const float h = settings.cellSize;
    return {(static_cast<float>(x) + 0.5F) * h,
            (static_cast<float>(y) + 0.5F) * h,
            (static_cast<float>(z) + 0.5F) * h};
}
[[nodiscard]] Float3 u_face_position(const FlipLiquidSettings& settings,
                                     std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t z) noexcept {
    const float h = settings.cellSize;
    return {static_cast<float>(x) * h,
            (static_cast<float>(y) + 0.5F) * h,
            (static_cast<float>(z) + 0.5F) * h};
}
[[nodiscard]] Float3 v_face_position(const FlipLiquidSettings& settings,
                                     std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t z) noexcept {
    const float h = settings.cellSize;
    return {(static_cast<float>(x) + 0.5F) * h,
            static_cast<float>(y) * h,
            (static_cast<float>(z) + 0.5F) * h};
}
[[nodiscard]] Float3 w_face_position(const FlipLiquidSettings& settings,
                                     std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t z) noexcept {
    const float h = settings.cellSize;
    return {(static_cast<float>(x) + 0.5F) * h,
            (static_cast<float>(y) + 0.5F) * h,
            static_cast<float>(z) * h};
}

[[nodiscard]] float component(Float3 value, int axis) noexcept {
    if (axis == 0) return value.x;
    if (axis == 1) return value.y;
    return value.z;
}

[[nodiscard]] float affine_component(const FlipLiquidParticle& particle,
                                     int velocityAxis,
                                     Float3 offset) noexcept {
    const Float3 row = velocityAxis == 0 ? particle.affineX :
                       velocityAxis == 1 ? particle.affineY : particle.affineZ;
    return dot(row, offset);
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}
[[nodiscard]] float unit_random(std::uint64_t key) noexcept {
    const std::uint32_t bits = static_cast<std::uint32_t>(splitmix64(key) >> 40U);
    return static_cast<float>(bits) / static_cast<float>(0xFFFFFFU);
}

struct AxisGridView {
    std::vector<float>* values{};
    std::vector<float>* weights{};
    int axis{};
};

void deposit_component(const FlipLiquidState& state,
                       const FlipLiquidParticle& particle,
                       AxisGridView view,
                       FlipLiquidTelemetry& telemetry) {
    const auto& settings = state.settings;
    const auto& d = settings.dimensions;
    const float h = settings.cellSize;

    const Float3 offsetOrigin = view.axis == 0 ? Float3{0.0F, 0.5F, 0.5F} :
                                view.axis == 1 ? Float3{0.5F, 0.0F, 0.5F} :
                                                 Float3{0.5F, 0.5F, 0.0F};
    const Float3 gridPosition{particle.position.x / h - offsetOrigin.x,
                              particle.position.y / h - offsetOrigin.y,
                              particle.position.z / h - offsetOrigin.z};
    const std::int32_t baseX = static_cast<std::int32_t>(std::floor(gridPosition.x));
    const std::int32_t baseY = static_cast<std::int32_t>(std::floor(gridPosition.y));
    const std::int32_t baseZ = static_cast<std::int32_t>(std::floor(gridPosition.z));
    const float fx = gridPosition.x - static_cast<float>(baseX);
    const float fy = gridPosition.y - static_cast<float>(baseY);
    const float fz = gridPosition.z - static_cast<float>(baseZ);

    const std::int32_t nx = static_cast<std::int32_t>(d.x) + (view.axis == 0 ? 1 : 0);
    const std::int32_t ny = static_cast<std::int32_t>(d.y) + (view.axis == 1 ? 1 : 0);
    const std::int32_t nz = static_cast<std::int32_t>(d.z) + (view.axis == 2 ? 1 : 0);

    for (std::int32_t oz = 0; oz <= 1; ++oz) {
        for (std::int32_t oy = 0; oy <= 1; ++oy) {
            for (std::int32_t ox = 0; ox <= 1; ++ox) {
                const std::int32_t x = baseX + ox;
                const std::int32_t y = baseY + oy;
                const std::int32_t z = baseZ + oz;
                if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) continue;
                const float weight = (ox == 0 ? 1.0F - fx : fx) *
                                     (oy == 0 ? 1.0F - fy : fy) *
                                     (oz == 0 ? 1.0F - fz : fz);
                Float3 facePosition{};
                std::size_t index{};
                if (view.axis == 0) {
                    facePosition = u_face_position(settings, x, y, z);
                    index = u_index(d, x, y, z);
                } else if (view.axis == 1) {
                    facePosition = v_face_position(settings, x, y, z);
                    index = v_index(d, x, y, z);
                } else {
                    facePosition = w_face_position(settings, x, y, z);
                    index = w_index(d, x, y, z);
                }
                float velocity = component(particle.velocity, view.axis);
                if (settings.transferMode == FlipLiquidTransferMode::FlipApic) {
                    velocity += settings.apicRatio * affine_component(
                        particle, view.axis, subtract(facePosition, particle.position));
                }
                (*view.values)[index] += weight * velocity;
                (*view.weights)[index] += weight;
                ++telemetry.particleToGridContributions;
            }
        }
    }
}

void particle_to_grid(FlipLiquidState& state, FlipLiquidTelemetry& telemetry) {
    state.velocity.clear();
    std::vector<float> uWeights(state.velocity.u.size(), 0.0F);
    std::vector<float> vWeights(state.velocity.v.size(), 0.0F);
    std::vector<float> wWeights(state.velocity.w.size(), 0.0F);
    for (const auto& particle : state.particles) {
        deposit_component(state, particle, {&state.velocity.u, &uWeights, 0}, telemetry);
        deposit_component(state, particle, {&state.velocity.v, &vWeights, 1}, telemetry);
        deposit_component(state, particle, {&state.velocity.w, &wWeights, 2}, telemetry);
    }
    auto normalize = [](std::vector<float>& values, const std::vector<float>& weights) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            values[index] = weights[index] > kEpsilon ? values[index] / weights[index] : 0.0F;
        }
    };
    normalize(state.velocity.u, uWeights);
    normalize(state.velocity.v, vWeights);
    normalize(state.velocity.w, wWeights);
    state.previousVelocity = state.velocity;
}

void classify_and_build_level_set(FlipLiquidState& state) {
    const auto& settings = state.settings;
    const auto& d = settings.dimensions;
    const std::size_t count = static_cast<std::size_t>(d.cell_count());
    const float farValue = (static_cast<float>(settings.levelSetBandCells) + 1.0F) * settings.cellSize;
    for (std::size_t index = 0; index < count; ++index) {
        if (state.cellTypes[index] != FlipLiquidCellType::Solid) {
            state.cellTypes[index] = FlipLiquidCellType::Air;
        }
        state.levelSet[index] = farValue;
    }

    const std::int32_t band = static_cast<std::int32_t>(settings.levelSetBandCells) + 1;
    for (const auto& particle : state.particles) {
        const auto c = cell_coordinate(particle.position, settings);
        for (std::int32_t dz = -band; dz <= band; ++dz) {
            for (std::int32_t dy = -band; dy <= band; ++dy) {
                for (std::int32_t dx = -band; dx <= band; ++dx) {
                    const std::int32_t x = static_cast<std::int32_t>(c[0]) + dx;
                    const std::int32_t y = static_cast<std::int32_t>(c[1]) + dy;
                    const std::int32_t z = static_cast<std::int32_t>(c[2]) + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= static_cast<std::int32_t>(d.x) ||
                        y >= static_cast<std::int32_t>(d.y) || z >= static_cast<std::int32_t>(d.z)) {
                        continue;
                    }
                    const std::size_t index = cell_index(d, x, y, z);
                    if (state.cellTypes[index] == FlipLiquidCellType::Solid) continue;
                    const float phi = length(subtract(cell_center(settings, x, y, z),
                                                      particle.position)) - settings.particleRadius;
                    state.levelSet[index] = std::min(state.levelSet[index], phi);
                }
            }
        }
        const std::size_t own = cell_index(d, c[0], c[1], c[2]);
        if (state.cellTypes[own] != FlipLiquidCellType::Solid) {
            state.cellTypes[own] = FlipLiquidCellType::Liquid;
            state.levelSet[own] = std::min(state.levelSet[own], -0.25F * settings.particleRadius);
        }
    }

    // Fill small one-cell holes when level-set evidence is negative.
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                const std::size_t index = cell_index(d, x, y, z);
                if (state.cellTypes[index] == FlipLiquidCellType::Air && state.levelSet[index] < 0.0F) {
                    state.cellTypes[index] = FlipLiquidCellType::Liquid;
                }
            }
        }
    }
}

[[nodiscard]] bool is_solid(const FlipLiquidState& state,
                            std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    const auto& d = state.settings.dimensions;
    if (x < 0 || y < 0 || z < 0 || x >= static_cast<std::int32_t>(d.x) ||
        y >= static_cast<std::int32_t>(d.y) || z >= static_cast<std::int32_t>(d.z)) return true;
    return state.cellTypes[cell_index(d, x, y, z)] == FlipLiquidCellType::Solid;
}
[[nodiscard]] bool is_liquid(const FlipLiquidState& state,
                             std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    const auto& d = state.settings.dimensions;
    if (x < 0 || y < 0 || z < 0 || x >= static_cast<std::int32_t>(d.x) ||
        y >= static_cast<std::int32_t>(d.y) || z >= static_cast<std::int32_t>(d.z)) return false;
    return state.cellTypes[cell_index(d, x, y, z)] == FlipLiquidCellType::Liquid;
}

void apply_gravity(FlipLiquidState& state, float dt) {
    const auto& d = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x <= d.x; ++x) {
                if (is_liquid(state, x - 1, y, z) || is_liquid(state, x, y, z)) {
                    state.velocity.u[u_index(d, x, y, z)] += state.settings.gravity.x * dt;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y <= d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                if (is_liquid(state, x, y - 1, z) || is_liquid(state, x, y, z)) {
                    state.velocity.v[v_index(d, x, y, z)] += state.settings.gravity.y * dt;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z <= d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                if (is_liquid(state, x, y, z - 1) || is_liquid(state, x, y, z)) {
                    state.velocity.w[w_index(d, x, y, z)] += state.settings.gravity.z * dt;
                }
            }
        }
    }
}

void enforce_boundaries(FlipLiquidState& state) {
    const auto& d = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x <= d.x; ++x) {
                if (x == 0U || x == d.x || is_solid(state, x - 1, y, z) || is_solid(state, x, y, z)) {
                    state.velocity.u[u_index(d, x, y, z)] = 0.0F;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y <= d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                if (y == 0U || y == d.y || is_solid(state, x, y - 1, z) || is_solid(state, x, y, z)) {
                    state.velocity.v[v_index(d, x, y, z)] = 0.0F;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z <= d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                if (z == 0U || z == d.z || is_solid(state, x, y, z - 1) || is_solid(state, x, y, z)) {
                    state.velocity.w[w_index(d, x, y, z)] = 0.0F;
                }
            }
        }
    }
}

[[nodiscard]] float compute_divergence(FlipLiquidState& state) {
    const auto& d = state.settings.dimensions;
    const float invH = 1.0F / state.settings.cellSize;
    float maximum = 0.0F;
    std::fill(state.divergence.begin(), state.divergence.end(), 0.0F);
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                const std::size_t index = cell_index(d, x, y, z);
                if (state.cellTypes[index] != FlipLiquidCellType::Liquid) continue;
                const float value = invH * (
                    state.velocity.u[u_index(d, x + 1U, y, z)] - state.velocity.u[u_index(d, x, y, z)] +
                    state.velocity.v[v_index(d, x, y + 1U, z)] - state.velocity.v[v_index(d, x, y, z)] +
                    state.velocity.w[w_index(d, x, y, z + 1U)] - state.velocity.w[w_index(d, x, y, z)]);
                state.divergence[index] = value;
                maximum = std::max(maximum, std::abs(value));
            }
        }
    }
    return maximum;
}

[[nodiscard]] float pressure_diagonal(const FlipLiquidState& state,
                                      std::uint32_t x,
                                      std::uint32_t y,
                                      std::uint32_t z) noexcept {
    float diagonal = 0.0F;
    const std::array<std::array<std::int32_t, 3>, 6> neighbors{{
        {{-1, 0, 0}}, {{1, 0, 0}}, {{0, -1, 0}}, {{0, 1, 0}}, {{0, 0, -1}}, {{0, 0, 1}}
    }};
    for (const auto& n : neighbors) {
        if (!is_solid(state, static_cast<std::int32_t>(x) + n[0],
                      static_cast<std::int32_t>(y) + n[1],
                      static_cast<std::int32_t>(z) + n[2])) {
            diagonal += 1.0F;
        }
    }
    return std::max(diagonal, 1.0F);
}

void apply_pressure_matrix(const FlipLiquidState& state,
                           const std::vector<float>& input,
                           std::vector<float>& output) {
    const auto& d = state.settings.dimensions;
    std::fill(output.begin(), output.end(), 0.0F);
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                const std::size_t index = cell_index(d, x, y, z);
                if (state.cellTypes[index] != FlipLiquidCellType::Liquid) continue;
                float value = pressure_diagonal(state, x, y, z) * input[index];
                const auto subtract_neighbor = [&](std::int32_t nx, std::int32_t ny, std::int32_t nz) {
                    if (is_liquid(state, nx, ny, nz)) {
                        value -= input[cell_index(d, nx, ny, nz)];
                    }
                };
                subtract_neighbor(x - 1, y, z); subtract_neighbor(x + 1, y, z);
                subtract_neighbor(x, y - 1, z); subtract_neighbor(x, y + 1, z);
                subtract_neighbor(x, y, z - 1); subtract_neighbor(x, y, z + 1);
                output[index] = value;
            }
        }
    }
}

[[nodiscard]] std::pair<std::uint32_t, float> solve_pressure(FlipLiquidState& state,
                                                              float dt) {
    const std::size_t count = state.pressure.size();
    std::vector<float> residual(count, 0.0F);
    std::vector<float> preconditioned(count, 0.0F);
    std::vector<float> direction(count, 0.0F);
    std::vector<float> matrixDirection(count, 0.0F);
    std::fill(state.pressure.begin(), state.pressure.end(), 0.0F);

    const float rhsScale = state.settings.particleDensity * state.settings.cellSize *
                           state.settings.cellSize / std::max(dt, kEpsilon);
    float rhsNormSquared = 0.0F;
    const auto& d = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                const std::size_t index = cell_index(d, x, y, z);
                if (state.cellTypes[index] != FlipLiquidCellType::Liquid) continue;
                residual[index] = -rhsScale * state.divergence[index];
                preconditioned[index] = residual[index] / pressure_diagonal(state, x, y, z);
                direction[index] = preconditioned[index];
                rhsNormSquared += residual[index] * residual[index];
            }
        }
    }
    if (rhsNormSquared <= kEpsilon) return {0U, 0.0F};

    float rz = 0.0F;
    for (std::size_t i = 0; i < count; ++i) rz += residual[i] * preconditioned[i];
    float relative = 1.0F;
    std::uint32_t iterations = 0U;
    while (iterations < state.settings.pressureMaximumIterations &&
           relative > state.settings.pressureRelativeTolerance && rz > kEpsilon) {
        apply_pressure_matrix(state, direction, matrixDirection);
        float denominator = 0.0F;
        for (std::size_t i = 0; i < count; ++i) denominator += direction[i] * matrixDirection[i];
        if (std::abs(denominator) <= kEpsilon) break;
        const float alpha = rz / denominator;
        float residualNormSquared = 0.0F;
        for (std::size_t i = 0; i < count; ++i) {
            state.pressure[i] += alpha * direction[i];
            residual[i] -= alpha * matrixDirection[i];
            residualNormSquared += residual[i] * residual[i];
        }
        ++iterations;
        relative = std::sqrt(residualNormSquared / rhsNormSquared);
        if (relative <= state.settings.pressureRelativeTolerance) break;
        for (std::uint32_t z = 0U; z < d.z; ++z) {
            for (std::uint32_t y = 0U; y < d.y; ++y) {
                for (std::uint32_t x = 0U; x < d.x; ++x) {
                    const std::size_t index = cell_index(d, x, y, z);
                    preconditioned[index] = state.cellTypes[index] == FlipLiquidCellType::Liquid ?
                        residual[index] / pressure_diagonal(state, x, y, z) : 0.0F;
                }
            }
        }
        float nextRz = 0.0F;
        for (std::size_t i = 0; i < count; ++i) nextRz += residual[i] * preconditioned[i];
        const float beta = nextRz / std::max(rz, kEpsilon);
        for (std::size_t i = 0; i < count; ++i) direction[i] = preconditioned[i] + beta * direction[i];
        rz = nextRz;
    }
    return {iterations, relative};
}

[[nodiscard]] float pressure_at(const FlipLiquidState& state,
                                std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    if (!is_liquid(state, x, y, z)) return 0.0F;
    return state.pressure[cell_index(state.settings.dimensions, x, y, z)];
}

void project_velocity(FlipLiquidState& state, float dt) {
    const auto& d = state.settings.dimensions;
    const float scale = dt / (state.settings.particleDensity * state.settings.cellSize);
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 1U; x < d.x; ++x) {
                if (is_solid(state, x - 1, y, z) || is_solid(state, x, y, z)) continue;
                if (is_liquid(state, x - 1, y, z) || is_liquid(state, x, y, z)) {
                    state.velocity.u[u_index(d, x, y, z)] -= scale *
                        (pressure_at(state, x, y, z) - pressure_at(state, x - 1, y, z));
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 1U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                if (is_solid(state, x, y - 1, z) || is_solid(state, x, y, z)) continue;
                if (is_liquid(state, x, y - 1, z) || is_liquid(state, x, y, z)) {
                    state.velocity.v[v_index(d, x, y, z)] -= scale *
                        (pressure_at(state, x, y, z) - pressure_at(state, x, y - 1, z));
                }
            }
        }
    }
    for (std::uint32_t z = 1U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                if (is_solid(state, x, y, z - 1) || is_solid(state, x, y, z)) continue;
                if (is_liquid(state, x, y, z - 1) || is_liquid(state, x, y, z)) {
                    state.velocity.w[w_index(d, x, y, z)] -= scale *
                        (pressure_at(state, x, y, z) - pressure_at(state, x, y, z - 1));
                }
            }
        }
    }
    enforce_boundaries(state);
}

void extrapolate_component(std::vector<float>& values,
                           std::vector<std::uint8_t>& valid,
                           std::uint32_t nx,
                           std::uint32_t ny,
                           std::uint32_t nz,
                           std::uint32_t layers,
                           FlipLiquidTelemetry& telemetry) {
    const auto index = [nx, ny](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return static_cast<std::size_t>(x) + static_cast<std::size_t>(nx) *
            (static_cast<std::size_t>(y) + static_cast<std::size_t>(ny) * z);
    };
    for (std::uint32_t layer = 0U; layer < layers; ++layer) {
        std::vector<float> next = values;
        std::vector<std::uint8_t> nextValid = valid;
        for (std::uint32_t z = 0U; z < nz; ++z) {
            for (std::uint32_t y = 0U; y < ny; ++y) {
                for (std::uint32_t x = 0U; x < nx; ++x) {
                    const std::size_t i = index(x, y, z);
                    if (valid[i]) continue;
                    float sum = 0.0F;
                    std::uint32_t count = 0U;
                    const auto add_neighbor = [&](std::int32_t px, std::int32_t py, std::int32_t pz) {
                        if (px < 0 || py < 0 || pz < 0 || px >= static_cast<std::int32_t>(nx) ||
                            py >= static_cast<std::int32_t>(ny) || pz >= static_cast<std::int32_t>(nz)) return;
                        const std::size_t n = index(px, py, pz);
                        if (valid[n]) { sum += values[n]; ++count; }
                    };
                    add_neighbor(x - 1, y, z); add_neighbor(x + 1, y, z);
                    add_neighbor(x, y - 1, z); add_neighbor(x, y + 1, z);
                    add_neighbor(x, y, z - 1); add_neighbor(x, y, z + 1);
                    if (count > 0U) {
                        next[i] = sum / static_cast<float>(count);
                        nextValid[i] = 1U;
                        ++telemetry.extrapolatedFaces;
                    }
                }
            }
        }
        values.swap(next);
        valid.swap(nextValid);
    }
}

void extrapolate_velocity(FlipLiquidState& state, FlipLiquidTelemetry& telemetry) {
    const auto& d = state.settings.dimensions;
    std::vector<std::uint8_t> uValid(state.velocity.u.size(), 0U);
    std::vector<std::uint8_t> vValid(state.velocity.v.size(), 0U);
    std::vector<std::uint8_t> wValid(state.velocity.w.size(), 0U);
    for (std::uint32_t z = 0U; z < d.z; ++z) for (std::uint32_t y = 0U; y < d.y; ++y)
        for (std::uint32_t x = 0U; x <= d.x; ++x)
            uValid[u_index(d, x, y, z)] = (is_liquid(state, x - 1, y, z) || is_liquid(state, x, y, z)) ? 1U : 0U;
    for (std::uint32_t z = 0U; z < d.z; ++z) for (std::uint32_t y = 0U; y <= d.y; ++y)
        for (std::uint32_t x = 0U; x < d.x; ++x)
            vValid[v_index(d, x, y, z)] = (is_liquid(state, x, y - 1, z) || is_liquid(state, x, y, z)) ? 1U : 0U;
    for (std::uint32_t z = 0U; z <= d.z; ++z) for (std::uint32_t y = 0U; y < d.y; ++y)
        for (std::uint32_t x = 0U; x < d.x; ++x)
            wValid[w_index(d, x, y, z)] = (is_liquid(state, x, y, z - 1) || is_liquid(state, x, y, z)) ? 1U : 0U;

    extrapolate_component(state.velocity.u, uValid, d.x + 1U, d.y, d.z,
                          state.settings.velocityExtrapolationLayers, telemetry);
    extrapolate_component(state.velocity.v, vValid, d.x, d.y + 1U, d.z,
                          state.settings.velocityExtrapolationLayers, telemetry);
    extrapolate_component(state.velocity.w, wValid, d.x, d.y, d.z + 1U,
                          state.settings.velocityExtrapolationLayers, telemetry);
    enforce_boundaries(state);
}

[[nodiscard]] float trilinear_component(const FlipLiquidSettings& settings,
                                        const std::vector<float>& values,
                                        int axis,
                                        Float3 p) noexcept {
    const auto& d = settings.dimensions;
    const float h = settings.cellSize;
    const Float3 origin = axis == 0 ? Float3{0.0F, 0.5F, 0.5F} :
                          axis == 1 ? Float3{0.5F, 0.0F, 0.5F} : Float3{0.5F, 0.5F, 0.0F};
    const Float3 g{p.x / h - origin.x, p.y / h - origin.y, p.z / h - origin.z};
    const std::int32_t nx = static_cast<std::int32_t>(d.x) + (axis == 0 ? 1 : 0);
    const std::int32_t ny = static_cast<std::int32_t>(d.y) + (axis == 1 ? 1 : 0);
    const std::int32_t nz = static_cast<std::int32_t>(d.z) + (axis == 2 ? 1 : 0);
    const std::int32_t x0 = std::clamp(static_cast<std::int32_t>(std::floor(g.x)), 0, nx - 1);
    const std::int32_t y0 = std::clamp(static_cast<std::int32_t>(std::floor(g.y)), 0, ny - 1);
    const std::int32_t z0 = std::clamp(static_cast<std::int32_t>(std::floor(g.z)), 0, nz - 1);
    const std::int32_t x1 = std::min(x0 + 1, nx - 1);
    const std::int32_t y1 = std::min(y0 + 1, ny - 1);
    const std::int32_t z1 = std::min(z0 + 1, nz - 1);
    const float fx = std::clamp(g.x - std::floor(g.x), 0.0F, 1.0F);
    const float fy = std::clamp(g.y - std::floor(g.y), 0.0F, 1.0F);
    const float fz = std::clamp(g.z - std::floor(g.z), 0.0F, 1.0F);
    const auto get = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        if (axis == 0) return values[u_index(d, x, y, z)];
        if (axis == 1) return values[v_index(d, x, y, z)];
        return values[w_index(d, x, y, z)];
    };
    const float c00 = std::lerp(get(x0, y0, z0), get(x1, y0, z0), fx);
    const float c10 = std::lerp(get(x0, y1, z0), get(x1, y1, z0), fx);
    const float c01 = std::lerp(get(x0, y0, z1), get(x1, y0, z1), fx);
    const float c11 = std::lerp(get(x0, y1, z1), get(x1, y1, z1), fx);
    return std::lerp(std::lerp(c00, c10, fy), std::lerp(c01, c11, fy), fz);
}

[[nodiscard]] Float3 sample_mac(const FlipLiquidSettings& settings,
                                const GridFluidMacGrid& grid,
                                Float3 p) noexcept {
    p = clamp_position(p, settings);
    return {trilinear_component(settings, grid.u, 0, p),
            trilinear_component(settings, grid.v, 1, p),
            trilinear_component(settings, grid.w, 2, p)};
}

[[nodiscard]] Float3 velocity_gradient_row(const FlipLiquidSettings& settings,
                                           const GridFluidMacGrid& grid,
                                           Float3 p,
                                           int velocityAxis) noexcept {
    const float epsilon = settings.cellSize * 0.5F;
    const auto value = [&](Float3 q) {
        return component(sample_mac(settings, grid, q), velocityAxis);
    };
    return {(value(add(p, {epsilon, 0.0F, 0.0F})) - value(add(p, {-epsilon, 0.0F, 0.0F}))) / (2.0F * epsilon),
            (value(add(p, {0.0F, epsilon, 0.0F})) - value(add(p, {0.0F, -epsilon, 0.0F}))) / (2.0F * epsilon),
            (value(add(p, {0.0F, 0.0F, epsilon})) - value(add(p, {0.0F, 0.0F, -epsilon}))) / (2.0F * epsilon)};
}

[[nodiscard]] Float3 clamp_affine(Float3 value, float maximum) noexcept {
    const float magnitude = length(value);
    return magnitude > maximum && magnitude > kEpsilon ? multiply(value, maximum / magnitude) : value;
}

void resolve_particle_solid_contact(FlipLiquidState& state,
                                    FlipLiquidParticle& particle,
                                    Float3 oldPosition,
                                    FlipLiquidTelemetry& telemetry) {
    const auto c = cell_coordinate(particle.position, state.settings);
    const std::size_t index = cell_index(state.settings.dimensions, c[0], c[1], c[2]);
    if (state.cellTypes[index] == FlipLiquidCellType::Solid) {
        particle.position = oldPosition;
        particle.velocity = multiply(particle.velocity, 0.25F);
        particle.affineX = particle.affineY = particle.affineZ = {};
        ++telemetry.particleSolidContacts;
    }
}

void grid_to_particles(FlipLiquidState& state, float dt, FlipLiquidTelemetry& telemetry) {
    const auto& settings = state.settings;
    for (auto& particle : state.particles) {
        const Float3 picVelocity = sample_mac(settings, state.velocity, particle.position);
        const Float3 oldGridVelocity = sample_mac(settings, state.previousVelocity, particle.position);
        const Float3 flipVelocity = add(particle.velocity, subtract(picVelocity, oldGridVelocity));
        if (settings.transferMode == FlipLiquidTransferMode::Pic) {
            particle.velocity = picVelocity;
        } else if (settings.transferMode == FlipLiquidTransferMode::Flip) {
            particle.velocity = flipVelocity;
        } else {
            particle.velocity = add(multiply(picVelocity, 1.0F - settings.flipRatio),
                                    multiply(flipVelocity, settings.flipRatio));
            particle.affineX = clamp_affine(velocity_gradient_row(settings, state.velocity,
                                                                  particle.position, 0), settings.affineClamp);
            particle.affineY = clamp_affine(velocity_gradient_row(settings, state.velocity,
                                                                  particle.position, 1), settings.affineClamp);
            particle.affineZ = clamp_affine(velocity_gradient_row(settings, state.velocity,
                                                                  particle.position, 2), settings.affineClamp);
        }
        const Float3 oldPosition = particle.position;
        const Float3 midpoint = add(particle.position, multiply(picVelocity, 0.5F * dt));
        particle.position = add(particle.position,
                                multiply(sample_mac(settings, state.velocity, midpoint), dt));
        const Float3 clamped = clamp_position(particle.position, settings);
        if (length_squared(subtract(clamped, particle.position)) > 0.0F) {
            ++telemetry.particleBoundaryContacts;
            particle.position = clamped;
        }
        resolve_particle_solid_contact(state, particle, oldPosition, telemetry);
        ++telemetry.gridToParticleUpdates;
    }
}

void reseed_particles(FlipLiquidState& state, FlipLiquidTelemetry& telemetry) {
    if (!state.settings.reseedingEnabled) return;
    const auto& d = state.settings.dimensions;
    const std::size_t cellCount = static_cast<std::size_t>(d.cell_count());
    std::vector<std::vector<std::uint32_t>> occupants(cellCount);
    for (std::uint32_t i = 0U; i < state.particles.size(); ++i) {
        const auto c = cell_coordinate(state.particles[i].position, state.settings);
        occupants[cell_index(d, c[0], c[1], c[2])].push_back(i);
    }

    std::vector<std::uint8_t> remove(state.particles.size(), 0U);
    for (std::size_t cell = 0; cell < occupants.size(); ++cell) {
        auto& indices = occupants[cell];
        if (indices.size() > state.settings.maximumParticlesPerCell) {
            std::sort(indices.begin(), indices.end(), [&](std::uint32_t a, std::uint32_t b) {
                return state.particles[a].persistentId < state.particles[b].persistentId;
            });
            for (std::size_t i = state.settings.maximumParticlesPerCell; i < indices.size(); ++i) {
                remove[indices[i]] = 1U;
                ++telemetry.particlesRemoved;
            }
        }
    }
    if (telemetry.particlesRemoved > 0U) {
        std::vector<FlipLiquidParticle> kept;
        kept.reserve(state.particles.size() - telemetry.particlesRemoved);
        for (std::size_t i = 0; i < state.particles.size(); ++i) if (!remove[i]) kept.push_back(state.particles[i]);
        state.particles.swap(kept);
    }

    occupants.assign(cellCount, {});
    for (std::uint32_t i = 0U; i < state.particles.size(); ++i) {
        const auto c = cell_coordinate(state.particles[i].position, state.settings);
        occupants[cell_index(d, c[0], c[1], c[2])].push_back(i);
    }
    for (std::uint32_t z = 0U; z < d.z; ++z) {
        for (std::uint32_t y = 0U; y < d.y; ++y) {
            for (std::uint32_t x = 0U; x < d.x; ++x) {
                const std::size_t index = cell_index(d, x, y, z);
                if (state.cellTypes[index] != FlipLiquidCellType::Liquid ||
                    occupants[index].size() >= state.settings.minimumParticlesPerCell) continue;
                const std::uint32_t desired = state.settings.targetParticlesPerCell -
                                              static_cast<std::uint32_t>(occupants[index].size());
                for (std::uint32_t i = 0U; i < desired &&
                     state.particles.size() < state.settings.maximumParticles; ++i) {
                    const std::uint64_t key = state.settings.deterministicSeed ^
                        (static_cast<std::uint64_t>(state.frameIndex) << 32U) ^
                        (static_cast<std::uint64_t>(index) << 8U) ^ i;
                    const Float3 jitter{unit_random(key) - 0.5F,
                                        unit_random(key + 1U) - 0.5F,
                                        unit_random(key + 2U) - 0.5F};
                    FlipLiquidParticle particle;
                    particle.position = add(cell_center(state.settings, x, y, z),
                                            multiply(jitter, 0.7F * state.settings.cellSize));
                    particle.position = clamp_position(particle.position, state.settings);
                    particle.velocity = sample_mac(state.settings, state.velocity, particle.position);
                    particle.persistentId = state.nextParticleId++;
                    state.particles.push_back(particle);
                    ++telemetry.particlesSpawned;
                }
            }
        }
    }
}

[[nodiscard]] float maximum_speed(const std::vector<FlipLiquidParticle>& particles) noexcept {
    float result = 0.0F;
    for (const auto& particle : particles) result = std::max(result, length(particle.velocity));
    return result;
}

void repair_state(FlipLiquidState& state, FlipLiquidTelemetry& telemetry) {
    auto repair_vector = [&](std::vector<float>& values) {
        for (float& value : values) if (!finite(value)) { value = 0.0F; ++telemetry.nonFiniteCorrections; }
    };
    repair_vector(state.velocity.u); repair_vector(state.velocity.v); repair_vector(state.velocity.w);
    repair_vector(state.pressure); repair_vector(state.divergence); repair_vector(state.levelSet);
    for (auto& particle : state.particles) {
        if (!finite(particle.position)) { particle.position = clamp_position({}, state.settings); ++telemetry.nonFiniteCorrections; }
        if (!finite(particle.velocity)) { particle.velocity = {}; ++telemetry.nonFiniteCorrections; }
        if (!finite(particle.affineX)) { particle.affineX = {}; ++telemetry.nonFiniteCorrections; }
        if (!finite(particle.affineY)) { particle.affineY = {}; ++telemetry.nonFiniteCorrections; }
        if (!finite(particle.affineZ)) { particle.affineZ = {}; ++telemetry.nonFiniteCorrections; }
    }
}

[[nodiscard]] std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) noexcept {
    return (value + divisor - 1U) / divisor;
}

} // namespace

bool FlipLiquidSettings::validate(std::string* error) const {
    if (!dimensions.validate(error)) return false;
    if (!(cellSize > 0.0F) || !(particleRadius > 0.0F) || particleRadius > cellSize ||
        !(particleDensity > 0.0F) || flipRatio < 0.0F || flipRatio > 1.0F ||
        apicRatio < 0.0F || apicRatio > 2.0F || !(affineClamp > 0.0F) ||
        !(maximumCfl > 0.0F) || !(maximumSubstepSeconds > 0.0F) ||
        maximumSubsteps == 0U || maximumSubsteps > 64U ||
        pressureMaximumIterations == 0U || pressureMaximumIterations > 4096U ||
        !(pressureRelativeTolerance > 0.0F) || pressureRelativeTolerance > 1.0F ||
        velocityExtrapolationLayers > 64U || levelSetBandCells == 0U || levelSetBandCells > 32U ||
        minimumParticlesPerCell == 0U || targetParticlesPerCell < minimumParticlesPerCell ||
        maximumParticlesPerCell < targetParticlesPerCell || maximumParticlesPerCell > 1024U ||
        maximumParticles == 0U || maximumParticles > 16'777'216U) {
        set_error(error, "FLIP liquid settings are outside supported bounds");
        return false;
    }
    const std::array<float, 12> values{cellSize, particleRadius, particleDensity,
        gravity.x, gravity.y, gravity.z, flipRatio, apicRatio, affineClamp,
        maximumCfl, maximumSubstepSeconds, pressureRelativeTolerance};
    if (!std::all_of(values.begin(), values.end(), [](float value) { return finite(value); })) {
        set_error(error, "FLIP liquid settings contain non-finite values");
        return false;
    }
    return true;
}

bool FlipLiquidState::validate(std::string* error) const {
    if (!settings.validate(error) || !velocity.validate(error) || !previousVelocity.validate(error)) return false;
    const std::size_t count = static_cast<std::size_t>(settings.dimensions.cell_count());
    if (pressure.size() != count || divergence.size() != count || levelSet.size() != count ||
        cellTypes.size() != count || particles.size() > settings.maximumParticles) {
        set_error(error, "FLIP liquid state array sizes are inconsistent");
        return false;
    }
    for (const auto& particle : particles) {
        if (!finite(particle.position) || !finite(particle.velocity) || !finite(particle.affineX) ||
            !finite(particle.affineY) || !finite(particle.affineZ)) {
            set_error(error, "FLIP liquid particle contains non-finite state");
            return false;
        }
    }
    return true;
}

FlipLiquidState make_flip_liquid_state(const FlipLiquidSettings& settings, std::string* error) {
    FlipLiquidState state;
    if (!settings.validate(error)) return state;
    state.settings = settings;
    state.velocity.dimensions = settings.dimensions;
    state.previousVelocity.dimensions = settings.dimensions;
    state.velocity.u.assign(u_count(settings.dimensions), 0.0F);
    state.velocity.v.assign(v_count(settings.dimensions), 0.0F);
    state.velocity.w.assign(w_count(settings.dimensions), 0.0F);
    state.previousVelocity = state.velocity;
    const std::size_t count = static_cast<std::size_t>(settings.dimensions.cell_count());
    state.pressure.assign(count, 0.0F);
    state.divergence.assign(count, 0.0F);
    state.levelSet.assign(count, (static_cast<float>(settings.levelSetBandCells) + 1.0F) * settings.cellSize);
    state.cellTypes.assign(count, FlipLiquidCellType::Air);
    return state;
}

bool add_flip_liquid_particle(FlipLiquidState& state, Float3 position, Float3 velocity,
                              std::string* error) {
    if (!state.validate(error) || !finite(position) || !finite(velocity)) {
        set_error(error, "FLIP liquid particle input is invalid");
        return false;
    }
    if (state.particles.size() >= state.settings.maximumParticles) {
        set_error(error, "FLIP liquid particle budget is exhausted");
        return false;
    }
    const Float3 maximum = domain_maximum(state.settings);
    if (position.x <= 0.0F || position.y <= 0.0F || position.z <= 0.0F ||
        position.x >= maximum.x || position.y >= maximum.y || position.z >= maximum.z) {
        set_error(error, "FLIP liquid particle lies outside the domain");
        return false;
    }
    state.particles.push_back({position, velocity, {}, {}, {}, state.nextParticleId++});
    return true;
}

std::uint32_t add_flip_liquid_box(FlipLiquidState& state, Float3 minimum, Float3 maximum,
                                  float spacing, Float3 velocity, std::string* error) {
    if (!state.validate(error) || !finite(minimum) || !finite(maximum) || !finite(velocity) ||
        !(spacing > 0.0F) || minimum.x >= maximum.x || minimum.y >= maximum.y ||
        minimum.z >= maximum.z) {
        set_error(error, "FLIP liquid box parameters are invalid");
        return 0U;
    }
    std::uint32_t added = 0U;
    for (float z = minimum.z; z <= maximum.z + 0.5F * spacing; z += spacing) {
        for (float y = minimum.y; y <= maximum.y + 0.5F * spacing; y += spacing) {
            for (float x = minimum.x; x <= maximum.x + 0.5F * spacing; x += spacing) {
                if (state.particles.size() >= state.settings.maximumParticles) return added;
                if (add_flip_liquid_particle(state, {x, y, z}, velocity, nullptr)) ++added;
            }
        }
    }
    return added;
}

void clear_flip_liquid(FlipLiquidState& state) noexcept {
    state.velocity.clear(); state.previousVelocity.clear();
    std::fill(state.pressure.begin(), state.pressure.end(), 0.0F);
    std::fill(state.divergence.begin(), state.divergence.end(), 0.0F);
    std::fill(state.levelSet.begin(), state.levelSet.end(),
              (static_cast<float>(state.settings.levelSetBandCells) + 1.0F) * state.settings.cellSize);
    for (auto& cell : state.cellTypes) if (cell != FlipLiquidCellType::Solid) cell = FlipLiquidCellType::Air;
    state.particles.clear(); state.frameIndex = 0U; state.nextParticleId = 1U;
}

void clear_flip_liquid_solids(FlipLiquidState& state) noexcept {
    for (auto& cell : state.cellTypes) if (cell == FlipLiquidCellType::Solid) cell = FlipLiquidCellType::Air;
}

bool set_flip_liquid_solid_box(FlipLiquidState& state, Float3 minimum, Float3 maximum,
                               std::string* error) {
    if (!state.validate(error) || !finite(minimum) || !finite(maximum) ||
        minimum.x >= maximum.x || minimum.y >= maximum.y || minimum.z >= maximum.z) {
        set_error(error, "FLIP liquid solid box is invalid");
        return false;
    }
    const auto& d = state.settings.dimensions;
    bool touched = false;
    for (std::uint32_t z = 0U; z < d.z; ++z) for (std::uint32_t y = 0U; y < d.y; ++y)
        for (std::uint32_t x = 0U; x < d.x; ++x) {
            const Float3 center = cell_center(state.settings, x, y, z);
            if (center.x >= minimum.x && center.x <= maximum.x && center.y >= minimum.y &&
                center.y <= maximum.y && center.z >= minimum.z && center.z <= maximum.z) {
                const std::size_t index = cell_index(d, x, y, z);
                state.cellTypes[index] = FlipLiquidCellType::Solid;
                state.pressure[index] = state.divergence[index] = 0.0F;
                touched = true;
            }
        }
    enforce_boundaries(state);
    return touched;
}

SimulationStepPlan plan_flip_liquid_step(float frameDeltaSeconds, const FlipLiquidState& state) {
    SimulationStepPolicy policy;
    policy.frameDeltaSeconds = frameDeltaSeconds;
    policy.maximumSpeed = maximum_speed(state.particles);
    policy.characteristicLength = state.settings.cellSize;
    policy.maximumDisplacementFraction = state.settings.maximumCfl;
    policy.maximumSubstepSeconds = state.settings.maximumSubstepSeconds;
    policy.maximumSubsteps = state.settings.maximumSubsteps;
    return plan_simulation_step(policy);
}

FlipLiquidTelemetry step_flip_liquid(FlipLiquidState& state, float frameDeltaSeconds) {
    FlipLiquidTelemetry telemetry;
    if (!state.validate() || !(frameDeltaSeconds > 0.0F) || !finite(frameDeltaSeconds) ||
        state.particles.empty()) return telemetry;
    telemetry.maximumSpeedBefore = maximum_speed(state.particles);
    const SimulationStepPlan plan = plan_flip_liquid_step(frameDeltaSeconds, state);
    telemetry.substeps = plan.substeps;
    telemetry.substepBudgetClamped = plan.clamped;

    for (std::uint32_t substep = 0U; substep < plan.substeps; ++substep) {
        particle_to_grid(state, telemetry);
        classify_and_build_level_set(state);
        apply_gravity(state, plan.substepSeconds);
        enforce_boundaries(state);
        telemetry.maximumDivergenceBeforeProjection = std::max(
            telemetry.maximumDivergenceBeforeProjection, compute_divergence(state));
        const auto [iterations, residual] = solve_pressure(state, plan.substepSeconds);
        telemetry.pressureIterations += iterations;
        telemetry.pressureResidual = residual;
        project_velocity(state, plan.substepSeconds);
        telemetry.maximumDivergenceAfterProjection = std::max(
            telemetry.maximumDivergenceAfterProjection, compute_divergence(state));
        extrapolate_velocity(state, telemetry);
        grid_to_particles(state, plan.substepSeconds, telemetry);
        classify_and_build_level_set(state);
        reseed_particles(state, telemetry);
        repair_state(state, telemetry);
        ++state.frameIndex;
    }

    classify_and_build_level_set(state);
    const auto liquidCount = std::count(state.cellTypes.begin(), state.cellTypes.end(),
                                        FlipLiquidCellType::Liquid);
    telemetry.liquidCells = static_cast<std::uint32_t>(liquidCount);
    for (std::uint32_t z = 0U; z < state.settings.dimensions.z; ++z)
        for (std::uint32_t y = 0U; y < state.settings.dimensions.y; ++y)
            for (std::uint32_t x = 0U; x < state.settings.dimensions.x; ++x) {
                if (!is_liquid(state, x, y, z)) continue;
                const bool surface = !is_liquid(state, x - 1, y, z) || !is_liquid(state, x + 1, y, z) ||
                                     !is_liquid(state, x, y - 1, z) || !is_liquid(state, x, y + 1, z) ||
                                     !is_liquid(state, x, y, z - 1) || !is_liquid(state, x, y, z + 1);
                if (surface) ++telemetry.surfaceCells;
            }
    telemetry.maximumSpeedAfter = maximum_speed(state.particles);
    telemetry.estimatedLiquidVolume = static_cast<float>(telemetry.liquidCells) *
        state.settings.cellSize * state.settings.cellSize * state.settings.cellSize;
    return telemetry;
}

Float3 sample_flip_liquid_velocity(const FlipLiquidState& state, Float3 worldPosition) noexcept {
    return state.validate() ? sample_mac(state.settings, state.velocity, worldPosition) : Float3{};
}

float sample_flip_liquid_level_set(const FlipLiquidState& state, Float3 worldPosition) noexcept {
    if (!state.validate()) return 0.0F;
    const auto& d = state.settings.dimensions;
    const float h = state.settings.cellSize;
    const Float3 g{worldPosition.x / h - 0.5F, worldPosition.y / h - 0.5F,
                   worldPosition.z / h - 0.5F};
    const std::int32_t x0 = std::clamp(static_cast<std::int32_t>(std::floor(g.x)), 0,
                                       static_cast<std::int32_t>(d.x) - 1);
    const std::int32_t y0 = std::clamp(static_cast<std::int32_t>(std::floor(g.y)), 0,
                                       static_cast<std::int32_t>(d.y) - 1);
    const std::int32_t z0 = std::clamp(static_cast<std::int32_t>(std::floor(g.z)), 0,
                                       static_cast<std::int32_t>(d.z) - 1);
    const std::int32_t x1 = std::min(x0 + 1, static_cast<std::int32_t>(d.x) - 1);
    const std::int32_t y1 = std::min(y0 + 1, static_cast<std::int32_t>(d.y) - 1);
    const std::int32_t z1 = std::min(z0 + 1, static_cast<std::int32_t>(d.z) - 1);
    const float fx = std::clamp(g.x - std::floor(g.x), 0.0F, 1.0F);
    const float fy = std::clamp(g.y - std::floor(g.y), 0.0F, 1.0F);
    const float fz = std::clamp(g.z - std::floor(g.z), 0.0F, 1.0F);
    const auto get = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return state.levelSet[cell_index(d, x, y, z)];
    };
    const float c00 = std::lerp(get(x0, y0, z0), get(x1, y0, z0), fx);
    const float c10 = std::lerp(get(x0, y1, z0), get(x1, y1, z0), fx);
    const float c01 = std::lerp(get(x0, y0, z1), get(x1, y0, z1), fx);
    const float c11 = std::lerp(get(x0, y1, z1), get(x1, y1, z1), fx);
    return std::lerp(std::lerp(c00, c10, fy), std::lerp(c01, c11, fy), fz);
}

std::uint64_t estimate_flip_liquid_bytes(const FlipLiquidSettings& settings) noexcept {
    if (!settings.validate()) return 0U;
    const std::uint64_t faces = u_count(settings.dimensions) + v_count(settings.dimensions) +
                                w_count(settings.dimensions);
    const std::uint64_t cells = settings.dimensions.cell_count();
    return 2U * faces * sizeof(float) + cells * (3U * sizeof(float) + sizeof(FlipLiquidCellType)) +
           static_cast<std::uint64_t>(settings.maximumParticles) * sizeof(FlipLiquidParticle);
}

bool FlipLiquidSurfaceSnapshot::validate(std::string* error) const {
    if (!dimensions.validate(error) || !(cellSize > 0.0F) || !finite(cellSize) ||
        levelSet.size() != dimensions.cell_count() || cellTypes.size() != dimensions.cell_count()) {
        set_error(error, "FLIP liquid surface snapshot is invalid");
        return false;
    }
    return std::all_of(levelSet.begin(), levelSet.end(), [](float value) { return finite(value); });
}

FlipLiquidSurfaceSnapshot build_flip_liquid_surface_snapshot(const FlipLiquidState& state) {
    FlipLiquidSurfaceSnapshot snapshot;
    if (!state.validate()) return snapshot;
    snapshot.dimensions = state.settings.dimensions;
    snapshot.cellSize = state.settings.cellSize;
    snapshot.sourceFrame = state.frameIndex;
    snapshot.levelSet = state.levelSet;
    snapshot.cellTypes = state.cellTypes;
    return snapshot;
}

bool FlipLiquidGpuFramePlan::validate(std::string* error) const {
    if (!enabled || !dimensions.validate(error) || particleCount == 0U || substeps == 0U ||
        pressureIterations == 0U || dispatches.empty() || estimatedBytes == 0U) {
        set_error(error, "FLIP liquid GPU frame plan is incomplete");
        return false;
    }
    return std::all_of(dispatches.begin(), dispatches.end(), [](const auto& dispatch) {
        return dispatch.groupsX > 0U && dispatch.groupsY > 0U && dispatch.groupsZ > 0U;
    });
}

FlipLiquidGpuFramePlan plan_flip_liquid_gpu_frame(const FlipLiquidSettings& settings,
                                                   std::uint32_t particleCount,
                                                   std::uint32_t substeps,
                                                   bool buildSurfaceOutput) {
    FlipLiquidGpuFramePlan plan;
    if (!settings.validate() || particleCount == 0U || substeps == 0U ||
        particleCount > settings.maximumParticles) return plan;
    plan.enabled = true;
    plan.dimensions = settings.dimensions;
    plan.particleCount = particleCount;
    plan.substeps = substeps;
    plan.pressureIterations = settings.pressureMaximumIterations;
    plan.extrapolationLayers = settings.velocityExtrapolationLayers;
    plan.usesApic = settings.transferMode == FlipLiquidTransferMode::FlipApic;
    plan.reseeds = settings.reseedingEnabled;
    plan.estimatedBytes = estimate_flip_liquid_bytes(settings);
    const std::uint32_t pg = ceil_div(particleCount, 256U);
    const std::uint32_t gx = ceil_div(settings.dimensions.x, 8U);
    const std::uint32_t gy = ceil_div(settings.dimensions.y, 8U);
    const std::uint32_t gz = ceil_div(settings.dimensions.z, 4U);
    const auto grid = [&](FlipLiquidGpuPassKind pass, std::uint32_t iteration = 0U) {
        plan.dispatches.push_back({pass, iteration, gx, gy, gz});
    };
    const auto particles = [&](FlipLiquidGpuPassKind pass, std::uint32_t iteration = 0U) {
        plan.dispatches.push_back({pass, iteration, pg, 1U, 1U});
    };
    for (std::uint32_t substep = 0U; substep < substeps; ++substep) {
        grid(FlipLiquidGpuPassKind::ClearGrid, substep);
        particles(FlipLiquidGpuPassKind::ParticleToGrid, substep);
        grid(FlipLiquidGpuPassKind::NormalizeGrid, substep);
        grid(FlipLiquidGpuPassKind::SaveGridVelocity, substep);
        grid(FlipLiquidGpuPassKind::ClassifyCells, substep);
        particles(FlipLiquidGpuPassKind::BuildParticleLevelSet, substep);
        grid(FlipLiquidGpuPassKind::ApplyForces, substep);
        grid(FlipLiquidGpuPassKind::EnforceBoundaries, substep);
        grid(FlipLiquidGpuPassKind::ComputeDivergence, substep);
        for (std::uint32_t iteration = 0U; iteration < settings.pressureMaximumIterations; ++iteration) {
            grid(FlipLiquidGpuPassKind::PressureSolve, iteration);
        }
        grid(FlipLiquidGpuPassKind::ProjectVelocity, substep);
        for (std::uint32_t layer = 0U; layer < settings.velocityExtrapolationLayers; ++layer) {
            grid(FlipLiquidGpuPassKind::ExtrapolateVelocity, layer);
        }
        particles(FlipLiquidGpuPassKind::GridToParticle, substep);
        particles(FlipLiquidGpuPassKind::AdvectParticles, substep);
        if (settings.reseedingEnabled) particles(FlipLiquidGpuPassKind::ReseedParticles, substep);
    }
    if (buildSurfaceOutput) grid(FlipLiquidGpuPassKind::BuildSurfaceOutput);
    return plan;
}

} // namespace dve
