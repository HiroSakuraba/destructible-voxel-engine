#include "dve/grid_fluid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <utility>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-6F;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool finite(Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] Float3 cross_product(Float3 a, Float3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

[[nodiscard]] Float3 clamp_position(Float3 position,
                                    const GridFluidSettings& settings) noexcept {
    const float maximumX = static_cast<float>(settings.dimensions.x) * settings.cellSize;
    const float maximumY = static_cast<float>(settings.dimensions.y) * settings.cellSize;
    const float maximumZ = static_cast<float>(settings.dimensions.z) * settings.cellSize;
    const float margin = settings.cellSize * 1.0e-3F;
    return {
        std::clamp(position.x, margin, std::max(margin, maximumX - margin)),
        std::clamp(position.y, margin, std::max(margin, maximumY - margin)),
        std::clamp(position.z, margin, std::max(margin, maximumZ - margin)),
    };
}

[[nodiscard]] std::size_t cell_index(const GridFluidDimensions& dimensions,
                                     std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t z) noexcept {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(dimensions.x) *
               (static_cast<std::size_t>(y) +
                static_cast<std::size_t>(dimensions.y) * static_cast<std::size_t>(z));
}

[[nodiscard]] std::size_t u_index(const GridFluidDimensions& dimensions,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t z) noexcept {
    const std::size_t width = static_cast<std::size_t>(dimensions.x) + 1U;
    return static_cast<std::size_t>(x) +
           width * (static_cast<std::size_t>(y) +
                    static_cast<std::size_t>(dimensions.y) * static_cast<std::size_t>(z));
}

[[nodiscard]] std::size_t v_index(const GridFluidDimensions& dimensions,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t z) noexcept {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(dimensions.x) *
               (static_cast<std::size_t>(y) +
                (static_cast<std::size_t>(dimensions.y) + 1U) * static_cast<std::size_t>(z));
}

[[nodiscard]] std::size_t w_index(const GridFluidDimensions& dimensions,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t z) noexcept {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(dimensions.x) *
               (static_cast<std::size_t>(y) +
                static_cast<std::size_t>(dimensions.y) * static_cast<std::size_t>(z));
}

[[nodiscard]] std::size_t u_count(const GridFluidDimensions& dimensions) noexcept {
    return (static_cast<std::size_t>(dimensions.x) + 1U) *
           static_cast<std::size_t>(dimensions.y) *
           static_cast<std::size_t>(dimensions.z);
}

[[nodiscard]] std::size_t v_count(const GridFluidDimensions& dimensions) noexcept {
    return static_cast<std::size_t>(dimensions.x) *
           (static_cast<std::size_t>(dimensions.y) + 1U) *
           static_cast<std::size_t>(dimensions.z);
}

[[nodiscard]] std::size_t w_count(const GridFluidDimensions& dimensions) noexcept {
    return static_cast<std::size_t>(dimensions.x) *
           static_cast<std::size_t>(dimensions.y) *
           (static_cast<std::size_t>(dimensions.z) + 1U);
}

[[nodiscard]] float sample_lattice(const std::vector<float>& values,
                                   std::uint32_t sizeX,
                                   std::uint32_t sizeY,
                                   std::uint32_t sizeZ,
                                   float x,
                                   float y,
                                   float z) noexcept {
    if (values.empty() || sizeX == 0U || sizeY == 0U || sizeZ == 0U) return 0.0F;
    const float maximumX = static_cast<float>(sizeX - 1U);
    const float maximumY = static_cast<float>(sizeY - 1U);
    const float maximumZ = static_cast<float>(sizeZ - 1U);
    x = std::clamp(x, 0.0F, maximumX);
    y = std::clamp(y, 0.0F, maximumY);
    z = std::clamp(z, 0.0F, maximumZ);
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(x));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(y));
    const std::uint32_t z0 = static_cast<std::uint32_t>(std::floor(z));
    const std::uint32_t x1 = std::min(x0 + 1U, sizeX - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, sizeY - 1U);
    const std::uint32_t z1 = std::min(z0 + 1U, sizeZ - 1U);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float tz = z - static_cast<float>(z0);
    const auto at = [&](std::uint32_t ix, std::uint32_t iy, std::uint32_t iz) noexcept {
        const std::size_t index = static_cast<std::size_t>(ix) +
                                  static_cast<std::size_t>(sizeX) *
                                      (static_cast<std::size_t>(iy) +
                                       static_cast<std::size_t>(sizeY) *
                                           static_cast<std::size_t>(iz));
        return values[index];
    };
    const float c000 = at(x0, y0, z0);
    const float c100 = at(x1, y0, z0);
    const float c010 = at(x0, y1, z0);
    const float c110 = at(x1, y1, z0);
    const float c001 = at(x0, y0, z1);
    const float c101 = at(x1, y0, z1);
    const float c011 = at(x0, y1, z1);
    const float c111 = at(x1, y1, z1);
    const float c00 = c000 + (c100 - c000) * tx;
    const float c10 = c010 + (c110 - c010) * tx;
    const float c01 = c001 + (c101 - c001) * tx;
    const float c11 = c011 + (c111 - c011) * tx;
    const float c0 = c00 + (c10 - c00) * ty;
    const float c1 = c01 + (c11 - c01) * ty;
    return c0 + (c1 - c0) * tz;
}

[[nodiscard]] float sample_centered(const GridFluidSettings& settings,
                                    const std::vector<float>& values,
                                    Float3 position) noexcept {
    const float invCell = 1.0F / settings.cellSize;
    return sample_lattice(values,
                          settings.dimensions.x,
                          settings.dimensions.y,
                          settings.dimensions.z,
                          position.x * invCell - 0.5F,
                          position.y * invCell - 0.5F,
                          position.z * invCell - 0.5F);
}

[[nodiscard]] float sample_u(const GridFluidSettings& settings,
                             const GridFluidMacGrid& velocity,
                             Float3 position) noexcept {
    const float invCell = 1.0F / settings.cellSize;
    return sample_lattice(velocity.u,
                          settings.dimensions.x + 1U,
                          settings.dimensions.y,
                          settings.dimensions.z,
                          position.x * invCell,
                          position.y * invCell - 0.5F,
                          position.z * invCell - 0.5F);
}

[[nodiscard]] float sample_v(const GridFluidSettings& settings,
                             const GridFluidMacGrid& velocity,
                             Float3 position) noexcept {
    const float invCell = 1.0F / settings.cellSize;
    return sample_lattice(velocity.v,
                          settings.dimensions.x,
                          settings.dimensions.y + 1U,
                          settings.dimensions.z,
                          position.x * invCell - 0.5F,
                          position.y * invCell,
                          position.z * invCell - 0.5F);
}

[[nodiscard]] float sample_w(const GridFluidSettings& settings,
                             const GridFluidMacGrid& velocity,
                             Float3 position) noexcept {
    const float invCell = 1.0F / settings.cellSize;
    return sample_lattice(velocity.w,
                          settings.dimensions.x,
                          settings.dimensions.y,
                          settings.dimensions.z + 1U,
                          position.x * invCell - 0.5F,
                          position.y * invCell - 0.5F,
                          position.z * invCell);
}

[[nodiscard]] Float3 sample_velocity(const GridFluidSettings& settings,
                                     const GridFluidMacGrid& velocity,
                                     Float3 position) noexcept {
    position = clamp_position(position, settings);
    return {
        sample_u(settings, velocity, position),
        sample_v(settings, velocity, position),
        sample_w(settings, velocity, position),
    };
}

[[nodiscard]] Float3 trace_back(const GridFluidSettings& settings,
                                const GridFluidMacGrid& velocity,
                                Float3 position,
                                float deltaSeconds,
                                std::uint32_t traceOrder) noexcept {
    const Float3 firstVelocity = sample_velocity(settings, velocity, position);
    if (traceOrder <= 1U) {
        return clamp_position(subtract(position, multiply(firstVelocity, deltaSeconds)), settings);
    }
    const Float3 midpoint = clamp_position(
        subtract(position, multiply(firstVelocity, 0.5F * deltaSeconds)), settings);
    const Float3 midpointVelocity = sample_velocity(settings, velocity, midpoint);
    return clamp_position(subtract(position, multiply(midpointVelocity, deltaSeconds)), settings);
}

[[nodiscard]] std::pair<float, float> centered_source_range(
    const GridFluidSettings& settings,
    const std::vector<float>& source,
    Float3 position) noexcept {
    const float invCell = 1.0F / settings.cellSize;
    const float gx = std::clamp(position.x * invCell - 0.5F, 0.0F,
                                static_cast<float>(settings.dimensions.x - 1U));
    const float gy = std::clamp(position.y * invCell - 0.5F, 0.0F,
                                static_cast<float>(settings.dimensions.y - 1U));
    const float gz = std::clamp(position.z * invCell - 0.5F, 0.0F,
                                static_cast<float>(settings.dimensions.z - 1U));
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(gx));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(gy));
    const std::uint32_t z0 = static_cast<std::uint32_t>(std::floor(gz));
    const std::uint32_t x1 = std::min(x0 + 1U, settings.dimensions.x - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, settings.dimensions.y - 1U);
    const std::uint32_t z1 = std::min(z0 + 1U, settings.dimensions.z - 1U);
    float minimum = std::numeric_limits<float>::max();
    float maximum = -std::numeric_limits<float>::max();
    for (const std::uint32_t z : {z0, z1}) {
        for (const std::uint32_t y : {y0, y1}) {
            for (const std::uint32_t x : {x0, x1}) {
                const float value = source[cell_index(settings.dimensions, x, y, z)];
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
    }
    return {minimum, maximum};
}

void advect_centered_first_order(const GridFluidState& state,
                                 const std::vector<float>& source,
                                 std::vector<float>& destination,
                                 float deltaSeconds) {
    const GridFluidSettings& settings = state.settings;
    for (std::uint32_t z = 0U; z < settings.dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < settings.dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < settings.dimensions.x; ++x) {
                const std::size_t index = cell_index(settings.dimensions, x, y, z);
                if (state.cellTypes[index] == GridFluidCellType::Solid) {
                    destination[index] = 0.0F;
                    continue;
                }
                const Float3 position{
                    (static_cast<float>(x) + 0.5F) * settings.cellSize,
                    (static_cast<float>(y) + 0.5F) * settings.cellSize,
                    (static_cast<float>(z) + 0.5F) * settings.cellSize,
                };
                const Float3 sourcePosition = trace_back(
                    settings, state.velocity, position, deltaSeconds, settings.traceOrder);
                destination[index] = sample_centered(settings, source, sourcePosition);
            }
        }
    }
}

void advect_centered(const GridFluidState& state,
                     const std::vector<float>& source,
                     std::vector<float>& destination,
                     float deltaSeconds,
                     GridFluidAdvection scheme,
                     bool nonnegative) {
    destination.assign(source.size(), 0.0F);
    advect_centered_first_order(state, source, destination, deltaSeconds);
    if (scheme == GridFluidAdvection::SemiLagrangian) {
        if (nonnegative) {
            for (float& value : destination) value = std::max(0.0F, value);
        }
        return;
    }
    std::vector<float> backward(source.size(), 0.0F);
    GridFluidState backwardState = state;
    advect_centered_first_order(backwardState, destination, backward, -deltaSeconds);
    for (std::uint32_t z = 0U; z < state.settings.dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < state.settings.dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < state.settings.dimensions.x; ++x) {
                const std::size_t index = cell_index(state.settings.dimensions, x, y, z);
                if (state.cellTypes[index] == GridFluidCellType::Solid) {
                    destination[index] = 0.0F;
                    continue;
                }
                const Float3 position{
                    (static_cast<float>(x) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(y) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(z) + 0.5F) * state.settings.cellSize,
                };
                const Float3 sourcePosition = trace_back(state.settings,
                                                         state.velocity,
                                                         position,
                                                         deltaSeconds,
                                                         state.settings.traceOrder);
                const auto [minimum, maximum] = centered_source_range(
                    state.settings, source, sourcePosition);
                const float corrected = destination[index] +
                                        0.5F * (source[index] - backward[index]);
                destination[index] = std::clamp(corrected, minimum, maximum);
                if (nonnegative) destination[index] = std::max(0.0F, destination[index]);
            }
        }
    }
}

enum class FaceComponent : std::uint8_t { U, V, W };

[[nodiscard]] Float3 face_position(const GridFluidSettings& settings,
                                   FaceComponent component,
                                   std::uint32_t x,
                                   std::uint32_t y,
                                   std::uint32_t z) noexcept {
    if (component == FaceComponent::U) {
        return {static_cast<float>(x) * settings.cellSize,
                (static_cast<float>(y) + 0.5F) * settings.cellSize,
                (static_cast<float>(z) + 0.5F) * settings.cellSize};
    }
    if (component == FaceComponent::V) {
        return {(static_cast<float>(x) + 0.5F) * settings.cellSize,
                static_cast<float>(y) * settings.cellSize,
                (static_cast<float>(z) + 0.5F) * settings.cellSize};
    }
    return {(static_cast<float>(x) + 0.5F) * settings.cellSize,
            (static_cast<float>(y) + 0.5F) * settings.cellSize,
            static_cast<float>(z) * settings.cellSize};
}

[[nodiscard]] float sample_component(const GridFluidSettings& settings,
                                     const GridFluidMacGrid& grid,
                                     FaceComponent component,
                                     Float3 position) noexcept {
    if (component == FaceComponent::U) return sample_u(settings, grid, position);
    if (component == FaceComponent::V) return sample_v(settings, grid, position);
    return sample_w(settings, grid, position);
}

void advect_face_component_first_order(const GridFluidState& state,
                                       const GridFluidMacGrid& source,
                                       std::vector<float>& destination,
                                       FaceComponent component,
                                       float deltaSeconds) {
    const GridFluidDimensions dimensions = state.settings.dimensions;
    std::uint32_t sizeX = dimensions.x;
    std::uint32_t sizeY = dimensions.y;
    std::uint32_t sizeZ = dimensions.z;
    if (component == FaceComponent::U) ++sizeX;
    if (component == FaceComponent::V) ++sizeY;
    if (component == FaceComponent::W) ++sizeZ;
    destination.assign(static_cast<std::size_t>(sizeX) * sizeY * sizeZ, 0.0F);
    for (std::uint32_t z = 0U; z < sizeZ; ++z) {
        for (std::uint32_t y = 0U; y < sizeY; ++y) {
            for (std::uint32_t x = 0U; x < sizeX; ++x) {
                const Float3 position = face_position(state.settings, component, x, y, z);
                const Float3 sourcePosition = trace_back(state.settings,
                                                         state.velocity,
                                                         position,
                                                         deltaSeconds,
                                                         state.settings.traceOrder);
                const std::size_t index = static_cast<std::size_t>(x) +
                                          static_cast<std::size_t>(sizeX) *
                                              (static_cast<std::size_t>(y) +
                                               static_cast<std::size_t>(sizeY) * z);
                destination[index] = sample_component(state.settings,
                                                      source,
                                                      component,
                                                      sourcePosition);
            }
        }
    }
}

[[nodiscard]] std::pair<float, float> face_source_range(
    const GridFluidSettings& settings,
    const GridFluidMacGrid& source,
    FaceComponent component,
    Float3 position) noexcept {
    const float invCell = 1.0F / settings.cellSize;
    float gx = position.x * invCell - 0.5F;
    float gy = position.y * invCell - 0.5F;
    float gz = position.z * invCell - 0.5F;
    std::uint32_t sizeX = settings.dimensions.x;
    std::uint32_t sizeY = settings.dimensions.y;
    std::uint32_t sizeZ = settings.dimensions.z;
    const std::vector<float>* values = nullptr;
    if (component == FaceComponent::U) {
        gx += 0.5F;
        ++sizeX;
        values = &source.u;
    } else if (component == FaceComponent::V) {
        gy += 0.5F;
        ++sizeY;
        values = &source.v;
    } else {
        gz += 0.5F;
        ++sizeZ;
        values = &source.w;
    }
    gx = std::clamp(gx, 0.0F, static_cast<float>(sizeX - 1U));
    gy = std::clamp(gy, 0.0F, static_cast<float>(sizeY - 1U));
    gz = std::clamp(gz, 0.0F, static_cast<float>(sizeZ - 1U));
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(gx));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(gy));
    const std::uint32_t z0 = static_cast<std::uint32_t>(std::floor(gz));
    const std::uint32_t x1 = std::min(x0 + 1U, sizeX - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, sizeY - 1U);
    const std::uint32_t z1 = std::min(z0 + 1U, sizeZ - 1U);
    float minimum = std::numeric_limits<float>::max();
    float maximum = -std::numeric_limits<float>::max();
    for (const std::uint32_t z : {z0, z1}) {
        for (const std::uint32_t y : {y0, y1}) {
            for (const std::uint32_t x : {x0, x1}) {
                const std::size_t index = static_cast<std::size_t>(x) +
                                          static_cast<std::size_t>(sizeX) *
                                              (static_cast<std::size_t>(y) +
                                               static_cast<std::size_t>(sizeY) * z);
                const float value = (*values)[index];
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
    }
    return {minimum, maximum};
}

void advect_velocity(GridFluidState& state,
                     float deltaSeconds,
                     GridFluidTelemetry& telemetry) {
    const GridFluidMacGrid source = state.velocity;
    GridFluidMacGrid forward;
    forward.dimensions = state.settings.dimensions;
    advect_face_component_first_order(state, source, forward.u,
                                      FaceComponent::U, deltaSeconds);
    advect_face_component_first_order(state, source, forward.v,
                                      FaceComponent::V, deltaSeconds);
    advect_face_component_first_order(state, source, forward.w,
                                      FaceComponent::W, deltaSeconds);
    telemetry.velocityFacesAdvected += forward.u.size() + forward.v.size() + forward.w.size();
    if (state.settings.velocityAdvection == GridFluidAdvection::SemiLagrangian) {
        state.velocity = std::move(forward);
        return;
    }
    GridFluidState backwardState = state;
    backwardState.velocity = forward;
    GridFluidMacGrid backward;
    backward.dimensions = state.settings.dimensions;
    advect_face_component_first_order(backwardState, forward, backward.u,
                                      FaceComponent::U, -deltaSeconds);
    advect_face_component_first_order(backwardState, forward, backward.v,
                                      FaceComponent::V, -deltaSeconds);
    advect_face_component_first_order(backwardState, forward, backward.w,
                                      FaceComponent::W, -deltaSeconds);

    const auto correct = [&](FaceComponent component,
                             const std::vector<float>& original,
                             const std::vector<float>& reverse,
                             std::vector<float>& values,
                             std::uint32_t sizeX,
                             std::uint32_t sizeY,
                             std::uint32_t sizeZ) {
        for (std::uint32_t z = 0U; z < sizeZ; ++z) {
            for (std::uint32_t y = 0U; y < sizeY; ++y) {
                for (std::uint32_t x = 0U; x < sizeX; ++x) {
                    const std::size_t index = static_cast<std::size_t>(x) +
                                              static_cast<std::size_t>(sizeX) *
                                                  (static_cast<std::size_t>(y) +
                                                   static_cast<std::size_t>(sizeY) * z);
                    const Float3 position = face_position(state.settings, component, x, y, z);
                    const Float3 sourcePosition = trace_back(state.settings,
                                                             source,
                                                             position,
                                                             deltaSeconds,
                                                             state.settings.traceOrder);
                    const auto [minimum, maximum] = face_source_range(
                        state.settings, source, component, sourcePosition);
                    values[index] = std::clamp(values[index] +
                                               0.5F * (original[index] - reverse[index]),
                                               minimum,
                                               maximum);
                }
            }
        }
    };
    correct(FaceComponent::U, source.u, backward.u, forward.u,
            state.settings.dimensions.x + 1U,
            state.settings.dimensions.y,
            state.settings.dimensions.z);
    correct(FaceComponent::V, source.v, backward.v, forward.v,
            state.settings.dimensions.x,
            state.settings.dimensions.y + 1U,
            state.settings.dimensions.z);
    correct(FaceComponent::W, source.w, backward.w, forward.w,
            state.settings.dimensions.x,
            state.settings.dimensions.y,
            state.settings.dimensions.z + 1U);
    state.velocity = std::move(forward);
}

[[nodiscard]] bool cell_is_solid(const GridFluidState& state,
                                 std::int32_t x,
                                 std::int32_t y,
                                 std::int32_t z) noexcept {
    if (x < 0 || y < 0 || z < 0 ||
        x >= static_cast<std::int32_t>(state.settings.dimensions.x) ||
        y >= static_cast<std::int32_t>(state.settings.dimensions.y) ||
        z >= static_cast<std::int32_t>(state.settings.dimensions.z)) {
        return true;
    }
    return state.cellTypes[cell_index(state.settings.dimensions,
                                      static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y),
                                      static_cast<std::uint32_t>(z))] == GridFluidCellType::Solid;
}

std::uint64_t enforce_boundaries(GridFluidState& state) noexcept {
    const GridFluidDimensions dimensions = state.settings.dimensions;
    std::uint64_t clamped = 0U;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x <= dimensions.x; ++x) {
                const bool blocked = x == 0U || x == dimensions.x ||
                    cell_is_solid(state, static_cast<std::int32_t>(x) - 1,
                                  static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)) ||
                    cell_is_solid(state, static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y), static_cast<std::int32_t>(z));
                if (blocked) {
                    float& value = state.velocity.u[u_index(dimensions, x, y, z)];
                    if (value != 0.0F) ++clamped;
                    value = 0.0F;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y <= dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const bool blocked = y == 0U || y == dimensions.y ||
                    cell_is_solid(state, static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y) - 1, static_cast<std::int32_t>(z)) ||
                    cell_is_solid(state, static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y), static_cast<std::int32_t>(z));
                if (blocked) {
                    float& value = state.velocity.v[v_index(dimensions, x, y, z)];
                    if (value != 0.0F) ++clamped;
                    value = 0.0F;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z <= dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const bool blocked = z == 0U || z == dimensions.z ||
                    cell_is_solid(state, static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y), static_cast<std::int32_t>(z) - 1) ||
                    cell_is_solid(state, static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y), static_cast<std::int32_t>(z));
                if (blocked) {
                    float& value = state.velocity.w[w_index(dimensions, x, y, z)];
                    if (value != 0.0F) ++clamped;
                    value = 0.0F;
                }
            }
        }
    }
    return clamped;
}

void apply_combustion(GridFluidState& state,
                      float deltaSeconds,
                      GridFluidTelemetry& telemetry) noexcept {
    if (!state.settings.combustionEnabled) return;
    for (std::size_t index = 0U; index < state.density.size(); ++index) {
        if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
        if (state.fuel[index] <= 0.0F ||
            state.temperature[index] < state.settings.ignitionTemperature) {
            continue;
        }
        const float available = state.fuel[index];
        const float burn = std::min(available, state.settings.burnRate * deltaSeconds * available);
        state.fuel[index] -= burn;
        state.temperature[index] += burn * state.settings.heatRelease;
        state.density[index] += burn * state.settings.smokeYield;
        state.flame[index] += burn * state.settings.flameYield;
        ++telemetry.combustionCells;
    }
}

void dissipate_scalars(GridFluidState& state, float deltaSeconds) noexcept {
    const float densityScale = std::exp(-state.settings.densityDissipation * deltaSeconds);
    const float temperatureScale = std::exp(-state.settings.temperatureDissipation * deltaSeconds);
    const float fuelScale = std::exp(-state.settings.fuelDissipation * deltaSeconds);
    const float flameScale = std::exp(-state.settings.flameDissipation * deltaSeconds);
    for (std::size_t index = 0U; index < state.density.size(); ++index) {
        if (state.cellTypes[index] == GridFluidCellType::Solid) {
            state.density[index] = 0.0F;
            state.temperature[index] = state.settings.ambientTemperature;
            state.fuel[index] = 0.0F;
            state.flame[index] = 0.0F;
            continue;
        }
        state.density[index] = std::max(0.0F, state.density[index] * densityScale);
        state.temperature[index] = state.settings.ambientTemperature +
            (state.temperature[index] - state.settings.ambientTemperature) * temperatureScale;
        state.fuel[index] = std::max(0.0F, state.fuel[index] * fuelScale);
        state.flame[index] = std::max(0.0F, state.flame[index] * flameScale);
    }
}

[[nodiscard]] std::vector<Float3> centered_velocity_field(const GridFluidState& state) {
    std::vector<Float3> centered(state.density.size());
    const GridFluidDimensions dimensions = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                centered[cell_index(dimensions, x, y, z)] = {
                    0.5F * (state.velocity.u[u_index(dimensions, x, y, z)] +
                            state.velocity.u[u_index(dimensions, x + 1U, y, z)]),
                    0.5F * (state.velocity.v[v_index(dimensions, x, y, z)] +
                            state.velocity.v[v_index(dimensions, x, y + 1U, z)]),
                    0.5F * (state.velocity.w[w_index(dimensions, x, y, z)] +
                            state.velocity.w[w_index(dimensions, x, y, z + 1U)]),
                };
            }
        }
    }
    return centered;
}

[[nodiscard]] Float3 centered_at(const std::vector<Float3>& values,
                                 const GridFluidDimensions& dimensions,
                                 std::int32_t x,
                                 std::int32_t y,
                                 std::int32_t z) noexcept {
    x = std::clamp(x, 0, static_cast<std::int32_t>(dimensions.x) - 1);
    y = std::clamp(y, 0, static_cast<std::int32_t>(dimensions.y) - 1);
    z = std::clamp(z, 0, static_cast<std::int32_t>(dimensions.z) - 1);
    return values[cell_index(dimensions,
                             static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y),
                             static_cast<std::uint32_t>(z))];
}

void apply_forces(GridFluidState& state, float deltaSeconds) noexcept {
    const GridFluidDimensions dimensions = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 1U; x < dimensions.x; ++x) {
                state.velocity.u[u_index(dimensions, x, y, z)] +=
                    state.settings.gravity.x * deltaSeconds;
            }
        }
    }
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 1U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const std::size_t below = cell_index(dimensions, x, y - 1U, z);
                const std::size_t above = cell_index(dimensions, x, y, z);
                const float temperature = 0.5F *
                    (state.temperature[below] + state.temperature[above]);
                const float density = 0.5F * (state.density[below] + state.density[above]);
                const float buoyancy = state.settings.temperatureBuoyancy *
                                           (temperature - state.settings.ambientTemperature) -
                                       state.settings.smokeWeight * density;
                state.velocity.v[v_index(dimensions, x, y, z)] +=
                    (state.settings.gravity.y + buoyancy) * deltaSeconds;
            }
        }
    }
    for (std::uint32_t z = 1U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                state.velocity.w[w_index(dimensions, x, y, z)] +=
                    state.settings.gravity.z * deltaSeconds;
            }
        }
    }
}

void apply_vorticity_confinement(GridFluidState& state, float deltaSeconds) {
    if (!(state.settings.vorticityConfinement > 0.0F)) return;
    const GridFluidDimensions dimensions = state.settings.dimensions;
    const float reciprocalSpacing = 0.5F / state.settings.cellSize;
    const std::vector<Float3> centered = centered_velocity_field(state);
    std::vector<Float3> curl(centered.size());
    std::vector<float> magnitude(centered.size(), 0.0F);
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const std::int32_t ix = static_cast<std::int32_t>(x);
                const std::int32_t iy = static_cast<std::int32_t>(y);
                const std::int32_t iz = static_cast<std::int32_t>(z);
                const Float3 xp = centered_at(centered, dimensions, ix + 1, iy, iz);
                const Float3 xm = centered_at(centered, dimensions, ix - 1, iy, iz);
                const Float3 yp = centered_at(centered, dimensions, ix, iy + 1, iz);
                const Float3 ym = centered_at(centered, dimensions, ix, iy - 1, iz);
                const Float3 zp = centered_at(centered, dimensions, ix, iy, iz + 1);
                const Float3 zm = centered_at(centered, dimensions, ix, iy, iz - 1);
                const Float3 value{
                    (yp.z - ym.z - (zp.y - zm.y)) * reciprocalSpacing,
                    (zp.x - zm.x - (xp.z - xm.z)) * reciprocalSpacing,
                    (xp.y - xm.y - (yp.x - ym.x)) * reciprocalSpacing,
                };
                const std::size_t index = cell_index(dimensions, x, y, z);
                curl[index] = value;
                magnitude[index] = length(value);
            }
        }
    }
    std::vector<Float3> forces(centered.size());
    const auto magnitude_at = [&](std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
        x = std::clamp(x, 0, static_cast<std::int32_t>(dimensions.x) - 1);
        y = std::clamp(y, 0, static_cast<std::int32_t>(dimensions.y) - 1);
        z = std::clamp(z, 0, static_cast<std::int32_t>(dimensions.z) - 1);
        return magnitude[cell_index(dimensions,
                                    static_cast<std::uint32_t>(x),
                                    static_cast<std::uint32_t>(y),
                                    static_cast<std::uint32_t>(z))];
    };
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const std::int32_t ix = static_cast<std::int32_t>(x);
                const std::int32_t iy = static_cast<std::int32_t>(y);
                const std::int32_t iz = static_cast<std::int32_t>(z);
                Float3 gradient{
                    (magnitude_at(ix + 1, iy, iz) - magnitude_at(ix - 1, iy, iz)) * reciprocalSpacing,
                    (magnitude_at(ix, iy + 1, iz) - magnitude_at(ix, iy - 1, iz)) * reciprocalSpacing,
                    (magnitude_at(ix, iy, iz + 1) - magnitude_at(ix, iy, iz - 1)) * reciprocalSpacing,
                };
                gradient = normalize(gradient);
                const std::size_t index = cell_index(dimensions, x, y, z);
                forces[index] = multiply(cross_product(gradient, curl[index]),
                    state.settings.vorticityConfinement * state.settings.cellSize);
            }
        }
    }
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 1U; x < dimensions.x; ++x) {
                const float force = 0.5F *
                    (forces[cell_index(dimensions, x - 1U, y, z)].x +
                     forces[cell_index(dimensions, x, y, z)].x);
                state.velocity.u[u_index(dimensions, x, y, z)] += force * deltaSeconds;
            }
        }
    }
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 1U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const float force = 0.5F *
                    (forces[cell_index(dimensions, x, y - 1U, z)].y +
                     forces[cell_index(dimensions, x, y, z)].y);
                state.velocity.v[v_index(dimensions, x, y, z)] += force * deltaSeconds;
            }
        }
    }
    for (std::uint32_t z = 1U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const float force = 0.5F *
                    (forces[cell_index(dimensions, x, y, z - 1U)].z +
                     forces[cell_index(dimensions, x, y, z)].z);
                state.velocity.w[w_index(dimensions, x, y, z)] += force * deltaSeconds;
            }
        }
    }
}

float compute_divergence(GridFluidState& state) noexcept {
    const GridFluidDimensions dimensions = state.settings.dimensions;
    const float reciprocalCell = 1.0F / state.settings.cellSize;
    float maximum = 0.0F;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const std::size_t index = cell_index(dimensions, x, y, z);
                if (state.cellTypes[index] == GridFluidCellType::Solid) {
                    state.divergence[index] = 0.0F;
                    continue;
                }
                const float value = (
                    state.velocity.u[u_index(dimensions, x + 1U, y, z)] -
                    state.velocity.u[u_index(dimensions, x, y, z)] +
                    state.velocity.v[v_index(dimensions, x, y + 1U, z)] -
                    state.velocity.v[v_index(dimensions, x, y, z)] +
                    state.velocity.w[w_index(dimensions, x, y, z + 1U)] -
                    state.velocity.w[w_index(dimensions, x, y, z)]) * reciprocalCell;
                state.divergence[index] = value;
                maximum = std::max(maximum, std::abs(value));
            }
        }
    }
    return maximum;
}

[[nodiscard]] float pressure_diagonal(const GridFluidState& state,
                                      std::uint32_t x,
                                      std::uint32_t y,
                                      std::uint32_t z) noexcept {
    float diagonal = 1.0e-6F;
    const std::array<std::array<std::int32_t, 3>, 6> offsets{{
        {{-1, 0, 0}}, {{1, 0, 0}}, {{0, -1, 0}},
        {{0, 1, 0}}, {{0, 0, -1}}, {{0, 0, 1}},
    }};
    for (const auto& offset : offsets) {
        if (!cell_is_solid(state,
                           static_cast<std::int32_t>(x) + offset[0],
                           static_cast<std::int32_t>(y) + offset[1],
                           static_cast<std::int32_t>(z) + offset[2])) {
            diagonal += 1.0F;
        }
    }
    return diagonal;
}

void apply_pressure_matrix(const GridFluidState& state,
                           const std::vector<float>& input,
                           std::vector<float>& output) {
    const GridFluidDimensions dimensions = state.settings.dimensions;
    output.assign(input.size(), 0.0F);
    const std::array<std::array<std::int32_t, 3>, 6> offsets{{
        {{-1, 0, 0}}, {{1, 0, 0}}, {{0, -1, 0}},
        {{0, 1, 0}}, {{0, 0, -1}}, {{0, 0, 1}},
    }};
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const std::size_t index = cell_index(dimensions, x, y, z);
                if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
                float result = pressure_diagonal(state, x, y, z) * input[index];
                for (const auto& offset : offsets) {
                    const std::int32_t nx = static_cast<std::int32_t>(x) + offset[0];
                    const std::int32_t ny = static_cast<std::int32_t>(y) + offset[1];
                    const std::int32_t nz = static_cast<std::int32_t>(z) + offset[2];
                    if (cell_is_solid(state, nx, ny, nz)) continue;
                    result -= input[cell_index(dimensions,
                                               static_cast<std::uint32_t>(nx),
                                               static_cast<std::uint32_t>(ny),
                                               static_cast<std::uint32_t>(nz))];
                }
                output[index] = result;
            }
        }
    }
}

[[nodiscard]] double fluid_dot(const GridFluidState& state,
                               const std::vector<float>& a,
                               const std::vector<float>& b) noexcept {
    double result = 0.0;
    for (std::size_t index = 0U; index < a.size(); ++index) {
        if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
        result += static_cast<double>(a[index]) * static_cast<double>(b[index]);
    }
    return result;
}

std::pair<std::uint32_t, float> solve_pressure(GridFluidState& state,
                                               float deltaSeconds) {
    const std::size_t count = state.pressure.size();
    std::vector<float> rightHandSide(count, 0.0F);
    std::vector<float> residual(count, 0.0F);
    std::vector<float> direction(count, 0.0F);
    std::vector<float> preconditioned(count, 0.0F);
    std::vector<float> matrixDirection(count, 0.0F);
    std::vector<float> matrixPressure(count, 0.0F);
    const float scale = state.settings.cellSize * state.settings.cellSize /
                        std::max(deltaSeconds, 1.0e-6F);
    for (std::size_t index = 0U; index < count; ++index) {
        if (state.cellTypes[index] == GridFluidCellType::Solid) {
            state.pressure[index] = 0.0F;
            continue;
        }
        rightHandSide[index] = -state.divergence[index] * scale;
        if (!state.settings.warmStartPressure) state.pressure[index] = 0.0F;
    }
    apply_pressure_matrix(state, state.pressure, matrixPressure);
    for (std::uint32_t z = 0U; z < state.settings.dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < state.settings.dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < state.settings.dimensions.x; ++x) {
                const std::size_t index = cell_index(state.settings.dimensions, x, y, z);
                if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
                residual[index] = rightHandSide[index] - matrixPressure[index];
                preconditioned[index] = residual[index] /
                    pressure_diagonal(state, x, y, z);
                direction[index] = preconditioned[index];
            }
        }
    }
    const double rhsNormSquared = std::max(fluid_dot(state, rightHandSide, rightHandSide),
                                           1.0e-30);
    double rz = fluid_dot(state, residual, preconditioned);
    float relativeResidual = static_cast<float>(
        std::sqrt(fluid_dot(state, residual, residual) / rhsNormSquared));
    std::uint32_t iterations = 0U;
    while (iterations < state.settings.pressureMaximumIterations &&
           relativeResidual > state.settings.pressureRelativeTolerance &&
           std::abs(rz) > 1.0e-30) {
        apply_pressure_matrix(state, direction, matrixDirection);
        const double denominator = fluid_dot(state, direction, matrixDirection);
        if (!(denominator > 1.0e-30)) break;
        const float alpha = static_cast<float>(rz / denominator);
        for (std::size_t index = 0U; index < count; ++index) {
            if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
            state.pressure[index] += alpha * direction[index];
            residual[index] -= alpha * matrixDirection[index];
        }
        relativeResidual = static_cast<float>(
            std::sqrt(fluid_dot(state, residual, residual) / rhsNormSquared));
        ++iterations;
        if (relativeResidual <= state.settings.pressureRelativeTolerance) break;
        for (std::uint32_t z = 0U; z < state.settings.dimensions.z; ++z) {
            for (std::uint32_t y = 0U; y < state.settings.dimensions.y; ++y) {
                for (std::uint32_t x = 0U; x < state.settings.dimensions.x; ++x) {
                    const std::size_t index = cell_index(state.settings.dimensions, x, y, z);
                    if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
                    preconditioned[index] = residual[index] /
                        pressure_diagonal(state, x, y, z);
                }
            }
        }
        const double nextRz = fluid_dot(state, residual, preconditioned);
        const float beta = static_cast<float>(nextRz / rz);
        for (std::size_t index = 0U; index < count; ++index) {
            if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
            direction[index] = preconditioned[index] + beta * direction[index];
        }
        rz = nextRz;
    }
    return {iterations, relativeResidual};
}

void project_velocity(GridFluidState& state, float deltaSeconds) noexcept {
    const GridFluidDimensions dimensions = state.settings.dimensions;
    const float scale = deltaSeconds / state.settings.cellSize;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 1U; x < dimensions.x; ++x) {
                const std::size_t left = cell_index(dimensions, x - 1U, y, z);
                const std::size_t right = cell_index(dimensions, x, y, z);
                if (state.cellTypes[left] == GridFluidCellType::Solid ||
                    state.cellTypes[right] == GridFluidCellType::Solid) {
                    state.velocity.u[u_index(dimensions, x, y, z)] = 0.0F;
                } else {
                    state.velocity.u[u_index(dimensions, x, y, z)] -=
                        scale * (state.pressure[right] - state.pressure[left]);
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 1U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const std::size_t below = cell_index(dimensions, x, y - 1U, z);
                const std::size_t above = cell_index(dimensions, x, y, z);
                if (state.cellTypes[below] == GridFluidCellType::Solid ||
                    state.cellTypes[above] == GridFluidCellType::Solid) {
                    state.velocity.v[v_index(dimensions, x, y, z)] = 0.0F;
                } else {
                    state.velocity.v[v_index(dimensions, x, y, z)] -=
                        scale * (state.pressure[above] - state.pressure[below]);
                }
            }
        }
    }
    for (std::uint32_t z = 1U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const std::size_t behind = cell_index(dimensions, x, y, z - 1U);
                const std::size_t ahead = cell_index(dimensions, x, y, z);
                if (state.cellTypes[behind] == GridFluidCellType::Solid ||
                    state.cellTypes[ahead] == GridFluidCellType::Solid) {
                    state.velocity.w[w_index(dimensions, x, y, z)] = 0.0F;
                } else {
                    state.velocity.w[w_index(dimensions, x, y, z)] -=
                        scale * (state.pressure[ahead] - state.pressure[behind]);
                }
            }
        }
    }
}

[[nodiscard]] float maximum_speed(const GridFluidState& state) noexcept {
    float maximum = 0.0F;
    const GridFluidDimensions dimensions = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const Float3 position{
                    (static_cast<float>(x) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(y) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(z) + 0.5F) * state.settings.cellSize,
                };
                maximum = std::max(maximum,
                    length(sample_velocity(state.settings, state.velocity, position)));
            }
        }
    }
    return maximum;
}

void repair_nonfinite(GridFluidState& state, GridFluidTelemetry& telemetry) noexcept {
    const auto repair = [&](std::vector<float>& values, float replacement) {
        for (float& value : values) {
            if (!finite(value)) {
                value = replacement;
                ++telemetry.nonFiniteCorrections;
            }
        }
    };
    repair(state.velocity.u, 0.0F);
    repair(state.velocity.v, 0.0F);
    repair(state.velocity.w, 0.0F);
    repair(state.density, 0.0F);
    repair(state.temperature, state.settings.ambientTemperature);
    repair(state.fuel, 0.0F);
    repair(state.flame, 0.0F);
    repair(state.pressure, 0.0F);
    repair(state.divergence, 0.0F);
}

[[nodiscard]] bool point_inside_solid(const GridFluidState& state,
                                      Float3 position) noexcept {
    const float invCell = 1.0F / state.settings.cellSize;
    const std::int32_t x = static_cast<std::int32_t>(std::floor(position.x * invCell));
    const std::int32_t y = static_cast<std::int32_t>(std::floor(position.y * invCell));
    const std::int32_t z = static_cast<std::int32_t>(std::floor(position.z * invCell));
    return cell_is_solid(state, x, y, z);
}

void deposit_component(std::vector<float>& values,
                       std::vector<float>& weights,
                       std::uint32_t sizeX,
                       std::uint32_t sizeY,
                       std::uint32_t sizeZ,
                       float gx,
                       float gy,
                       float gz,
                       float value,
                       GridFluidParticleTransferTelemetry& telemetry) {
    gx = std::clamp(gx, 0.0F, static_cast<float>(sizeX - 1U));
    gy = std::clamp(gy, 0.0F, static_cast<float>(sizeY - 1U));
    gz = std::clamp(gz, 0.0F, static_cast<float>(sizeZ - 1U));
    const std::int32_t baseX = static_cast<std::int32_t>(std::floor(gx));
    const std::int32_t baseY = static_cast<std::int32_t>(std::floor(gy));
    const std::int32_t baseZ = static_cast<std::int32_t>(std::floor(gz));
    for (std::int32_t dz = 0; dz <= 1; ++dz) {
        for (std::int32_t dy = 0; dy <= 1; ++dy) {
            for (std::int32_t dx = 0; dx <= 1; ++dx) {
                const std::int32_t ix = std::clamp(baseX + dx, 0,
                                                   static_cast<std::int32_t>(sizeX) - 1);
                const std::int32_t iy = std::clamp(baseY + dy, 0,
                                                   static_cast<std::int32_t>(sizeY) - 1);
                const std::int32_t iz = std::clamp(baseZ + dz, 0,
                                                   static_cast<std::int32_t>(sizeZ) - 1);
                const float wx = dx == 0 ? 1.0F - (gx - static_cast<float>(baseX))
                                         : gx - static_cast<float>(baseX);
                const float wy = dy == 0 ? 1.0F - (gy - static_cast<float>(baseY))
                                         : gy - static_cast<float>(baseY);
                const float wz = dz == 0 ? 1.0F - (gz - static_cast<float>(baseZ))
                                         : gz - static_cast<float>(baseZ);
                const float weight = wx * wy * wz;
                const std::size_t index = static_cast<std::size_t>(ix) +
                                          static_cast<std::size_t>(sizeX) *
                                              (static_cast<std::size_t>(iy) +
                                               static_cast<std::size_t>(sizeY) *
                                                   static_cast<std::size_t>(iz));
                values[index] += value * weight;
                weights[index] += weight;
                ++telemetry.faceContributions;
            }
        }
    }
}

[[nodiscard]] std::uint32_t divide_round_up(std::uint32_t value,
                                             std::uint32_t divisor) noexcept {
    return (value + divisor - 1U) / divisor;
}

} // namespace

std::uint64_t GridFluidDimensions::cell_count() const noexcept {
    return static_cast<std::uint64_t>(x) * y * z;
}

bool GridFluidDimensions::validate(std::string* error) const {
    if (x < 2U || y < 2U || z < 2U || x > 512U || y > 512U || z > 512U) {
        set_error(error, "grid-fluid dimensions must be within [2, 512] on every axis");
        return false;
    }
    if (cell_count() > 134'217'728ULL) {
        set_error(error, "grid-fluid cell count exceeds the bounded module limit");
        return false;
    }
    return true;
}

bool GridFluidSettings::validate(std::string* error) const {
    if (!dimensions.validate(error)) return false;
    if (!(cellSize > 0.0F) || !finite(cellSize) || !(maximumCfl > 0.0F) ||
        !finite(maximumCfl) || !(maximumSubstepSeconds > 0.0F) ||
        !finite(maximumSubstepSeconds) || maximumSubsteps == 0U ||
        (traceOrder != 1U && traceOrder != 2U) || !finite(gravity) ||
        !finite(ambientTemperature) || !finite(temperatureBuoyancy) ||
        !finite(smokeWeight) || smokeWeight < 0.0F ||
        !finite(vorticityConfinement) || vorticityConfinement < 0.0F ||
        !finite(densityDissipation) || densityDissipation < 0.0F ||
        !finite(temperatureDissipation) || temperatureDissipation < 0.0F ||
        !finite(fuelDissipation) || fuelDissipation < 0.0F ||
        !finite(flameDissipation) || flameDissipation < 0.0F ||
        !finite(ignitionTemperature) || !finite(burnRate) || burnRate < 0.0F ||
        !finite(heatRelease) || heatRelease < 0.0F ||
        !finite(smokeYield) || smokeYield < 0.0F ||
        !finite(flameYield) || flameYield < 0.0F ||
        pressureMaximumIterations == 0U ||
        !(pressureRelativeTolerance > 0.0F) || !finite(pressureRelativeTolerance)) {
        set_error(error, "grid-fluid settings contain invalid or unbounded values");
        return false;
    }
    return true;
}

bool GridFluidMacGrid::validate(std::string* error) const {
    if (!dimensions.validate(error)) return false;
    if (u.size() != u_count(dimensions) || v.size() != v_count(dimensions) ||
        w.size() != w_count(dimensions)) {
        set_error(error, "MAC-grid component sizes do not match dimensions");
        return false;
    }
    return true;
}

void GridFluidMacGrid::clear() noexcept {
    std::fill(u.begin(), u.end(), 0.0F);
    std::fill(v.begin(), v.end(), 0.0F);
    std::fill(w.begin(), w.end(), 0.0F);
}

bool GridFluidState::validate(std::string* error) const {
    if (!settings.validate(error) || !velocity.validate(error) ||
        !previousVelocity.validate(error)) return false;
    const std::size_t count = static_cast<std::size_t>(settings.dimensions.cell_count());
    if (density.size() != count || temperature.size() != count || fuel.size() != count ||
        flame.size() != count || pressure.size() != count || divergence.size() != count ||
        cellTypes.size() != count) {
        set_error(error, "grid-fluid cell-centered field sizes do not match dimensions");
        return false;
    }
    return true;
}

GridFluidState make_grid_fluid_state(const GridFluidSettings& settings,
                                     std::string* error) {
    GridFluidState state;
    if (!settings.validate(error)) return state;
    state.settings = settings;
    state.velocity.dimensions = settings.dimensions;
    state.previousVelocity.dimensions = settings.dimensions;
    state.velocity.u.assign(u_count(settings.dimensions), 0.0F);
    state.velocity.v.assign(v_count(settings.dimensions), 0.0F);
    state.velocity.w.assign(w_count(settings.dimensions), 0.0F);
    state.previousVelocity = state.velocity;
    const std::size_t count = static_cast<std::size_t>(settings.dimensions.cell_count());
    state.density.assign(count, 0.0F);
    state.temperature.assign(count, settings.ambientTemperature);
    state.fuel.assign(count, 0.0F);
    state.flame.assign(count, 0.0F);
    state.pressure.assign(count, 0.0F);
    state.divergence.assign(count, 0.0F);
    state.cellTypes.assign(count, GridFluidCellType::Fluid);
    return state;
}

SimulationStepPlan plan_grid_fluid_step(float frameDeltaSeconds,
                                        const GridFluidState& state) {
    SimulationStepPolicy policy;
    policy.frameDeltaSeconds = frameDeltaSeconds;
    policy.maximumSpeed = maximum_speed(state);
    policy.characteristicLength = state.settings.cellSize;
    policy.maximumDisplacementFraction = state.settings.maximumCfl;
    policy.maximumSubstepSeconds = state.settings.maximumSubstepSeconds;
    policy.maximumSubsteps = state.settings.maximumSubsteps;
    return plan_simulation_step(policy);
}

GridFluidTelemetry step_grid_fluid(GridFluidState& state,
                                   float frameDeltaSeconds) {
    GridFluidTelemetry telemetry;
    if (!state.validate() || !(frameDeltaSeconds > 0.0F) || !finite(frameDeltaSeconds)) {
        return telemetry;
    }
    const SimulationStepPlan plan = plan_grid_fluid_step(frameDeltaSeconds, state);
    if (!plan.enabled) return telemetry;
    telemetry.substeps = plan.substeps;
    telemetry.substepBudgetClamped = plan.clamped;
    telemetry.maximumSpeedBefore = maximum_speed(state);
    for (std::uint32_t substep = 0U; substep < plan.substeps; ++substep) {
        state.previousVelocity = state.velocity;
        advect_velocity(state, plan.substepSeconds, telemetry);

        std::vector<float> nextDensity;
        std::vector<float> nextTemperature;
        std::vector<float> nextFuel;
        std::vector<float> nextFlame;
        advect_centered(state, state.density, nextDensity, plan.substepSeconds,
                        state.settings.scalarAdvection, true);
        advect_centered(state, state.temperature, nextTemperature, plan.substepSeconds,
                        state.settings.scalarAdvection, false);
        advect_centered(state, state.fuel, nextFuel, plan.substepSeconds,
                        state.settings.scalarAdvection, true);
        advect_centered(state, state.flame, nextFlame, plan.substepSeconds,
                        state.settings.scalarAdvection, true);
        telemetry.scalarCellsAdvected += state.density.size() * 4U;
        state.density = std::move(nextDensity);
        state.temperature = std::move(nextTemperature);
        state.fuel = std::move(nextFuel);
        state.flame = std::move(nextFlame);

        apply_combustion(state, plan.substepSeconds, telemetry);
        dissipate_scalars(state, plan.substepSeconds);
        apply_forces(state, plan.substepSeconds);
        apply_vorticity_confinement(state, plan.substepSeconds);
        telemetry.solidFacesClamped += enforce_boundaries(state);
        const float divergenceBefore = compute_divergence(state);
        telemetry.maximumDivergenceBeforeProjection = std::max(
            telemetry.maximumDivergenceBeforeProjection, divergenceBefore);
        const auto [iterations, residual] = solve_pressure(state, plan.substepSeconds);
        telemetry.pressureIterations += iterations;
        telemetry.pressureResidual = residual;
        project_velocity(state, plan.substepSeconds);
        telemetry.solidFacesClamped += enforce_boundaries(state);
        const float divergenceAfter = compute_divergence(state);
        telemetry.maximumDivergenceAfterProjection = std::max(
            telemetry.maximumDivergenceAfterProjection, divergenceAfter);
        repair_nonfinite(state, telemetry);
    }
    ++state.frameIndex;
    telemetry.maximumSpeedAfter = maximum_speed(state);
    telemetry.totalDensity = std::accumulate(state.density.begin(), state.density.end(), 0.0F);
    telemetry.totalFuel = std::accumulate(state.fuel.begin(), state.fuel.end(), 0.0F);
    return telemetry;
}

void clear_grid_fluid(GridFluidState& state) noexcept {
    state.velocity.clear();
    state.previousVelocity.clear();
    std::fill(state.density.begin(), state.density.end(), 0.0F);
    std::fill(state.temperature.begin(), state.temperature.end(),
              state.settings.ambientTemperature);
    std::fill(state.fuel.begin(), state.fuel.end(), 0.0F);
    std::fill(state.flame.begin(), state.flame.end(), 0.0F);
    std::fill(state.pressure.begin(), state.pressure.end(), 0.0F);
    std::fill(state.divergence.begin(), state.divergence.end(), 0.0F);
    state.frameIndex = 0U;
}

void clear_grid_fluid_obstacles(GridFluidState& state) noexcept {
    std::fill(state.cellTypes.begin(), state.cellTypes.end(), GridFluidCellType::Fluid);
    (void)enforce_boundaries(state);
}

bool set_grid_fluid_solid_box(GridFluidState& state,
                              Float3 minimum,
                              Float3 maximum,
                              std::string* error) {
    if (!state.validate(error) || !finite(minimum) || !finite(maximum) ||
        minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z) {
        set_error(error, "invalid grid-fluid solid box");
        return false;
    }
    const GridFluidDimensions dimensions = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const Float3 center{
                    (static_cast<float>(x) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(y) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(z) + 0.5F) * state.settings.cellSize,
                };
                if (center.x < minimum.x || center.x > maximum.x ||
                    center.y < minimum.y || center.y > maximum.y ||
                    center.z < minimum.z || center.z > maximum.z) continue;
                const std::size_t index = cell_index(dimensions, x, y, z);
                state.cellTypes[index] = GridFluidCellType::Solid;
                state.density[index] = 0.0F;
                state.temperature[index] = state.settings.ambientTemperature;
                state.fuel[index] = 0.0F;
                state.flame[index] = 0.0F;
                state.pressure[index] = 0.0F;
            }
        }
    }
    (void)enforce_boundaries(state);
    return true;
}

bool inject_grid_fluid_sphere(GridFluidState& state,
                              const GridFluidEmitterSample& emitter,
                              std::string* error) {
    if (!state.validate(error) || !finite(emitter.center) ||
        !(emitter.radius > 0.0F) || !finite(emitter.radius) ||
        !finite(emitter.density) || !finite(emitter.temperature) ||
        !finite(emitter.fuel) || !finite(emitter.flame) || !finite(emitter.velocity)) {
        set_error(error, "invalid grid-fluid emitter");
        return false;
    }
    const float radiusSquared = emitter.radius * emitter.radius;
    const GridFluidDimensions dimensions = state.settings.dimensions;
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const Float3 center{
                    (static_cast<float>(x) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(y) + 0.5F) * state.settings.cellSize,
                    (static_cast<float>(z) + 0.5F) * state.settings.cellSize,
                };
                if (length_squared(subtract(center, emitter.center)) > radiusSquared) continue;
                const std::size_t index = cell_index(dimensions, x, y, z);
                if (state.cellTypes[index] == GridFluidCellType::Solid) continue;
                state.density[index] += std::max(0.0F, emitter.density);
                state.temperature[index] += emitter.temperature;
                state.fuel[index] += std::max(0.0F, emitter.fuel);
                state.flame[index] += std::max(0.0F, emitter.flame);
            }
        }
    }
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x <= dimensions.x; ++x) {
                const Float3 position = face_position(state.settings, FaceComponent::U, x, y, z);
                if (length_squared(subtract(position, emitter.center)) <= radiusSquared) {
                    state.velocity.u[u_index(dimensions, x, y, z)] += emitter.velocity.x;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z < dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y <= dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const Float3 position = face_position(state.settings, FaceComponent::V, x, y, z);
                if (length_squared(subtract(position, emitter.center)) <= radiusSquared) {
                    state.velocity.v[v_index(dimensions, x, y, z)] += emitter.velocity.y;
                }
            }
        }
    }
    for (std::uint32_t z = 0U; z <= dimensions.z; ++z) {
        for (std::uint32_t y = 0U; y < dimensions.y; ++y) {
            for (std::uint32_t x = 0U; x < dimensions.x; ++x) {
                const Float3 position = face_position(state.settings, FaceComponent::W, x, y, z);
                if (length_squared(subtract(position, emitter.center)) <= radiusSquared) {
                    state.velocity.w[w_index(dimensions, x, y, z)] += emitter.velocity.z;
                }
            }
        }
    }
    (void)enforce_boundaries(state);
    return true;
}

float sample_grid_fluid_scalar(const GridFluidState& state,
                               const std::vector<float>& field,
                               Float3 worldPosition) noexcept {
    if (field.size() != state.density.size()) return 0.0F;
    return sample_centered(state.settings, field, clamp_position(worldPosition, state.settings));
}

Float3 sample_grid_fluid_velocity(const GridFluidState& state,
                                  Float3 worldPosition) noexcept {
    return sample_velocity(state.settings, state.velocity, worldPosition);
}

float maximum_grid_fluid_divergence(const GridFluidState& state) noexcept {
    float maximum = 0.0F;
    for (const float value : state.divergence) maximum = std::max(maximum, std::abs(value));
    return maximum;
}

std::uint64_t estimate_grid_fluid_bytes(const GridFluidSettings& settings) noexcept {
    if (!settings.dimensions.validate()) return 0U;
    const std::uint64_t cells = settings.dimensions.cell_count();
    const std::uint64_t faces = static_cast<std::uint64_t>(u_count(settings.dimensions) +
                                                           v_count(settings.dimensions) +
                                                           w_count(settings.dimensions));
    const std::uint64_t persistent = faces * sizeof(float) * 2U +
                                     cells * sizeof(float) * 6U +
                                     cells * sizeof(GridFluidCellType);
    const std::uint64_t temporaries = faces * sizeof(float) * 3U +
                                      cells * sizeof(float) * 12U;
    return persistent + temporaries;
}

bool GridFluidVolumeSnapshot::validate(std::string* error) const {
    if (!dimensions.validate(error) || !finite(cellSize) || cellSize <= 0.0F ||
        voxels.size() != static_cast<std::size_t>(dimensions.cell_count())) {
        set_error(error, "grid-fluid volume snapshot is incomplete");
        return false;
    }
    for (const GridFluidVolumeVoxel& voxel : voxels) {
        if (!finite(voxel.density) || !finite(voxel.temperature) ||
            !finite(voxel.emission) || !finite(voxel.fuel) ||
            voxel.density < 0.0F || voxel.emission < 0.0F || voxel.fuel < 0.0F) {
            set_error(error, "grid-fluid volume snapshot contains invalid values");
            return false;
        }
    }
    return true;
}

GridFluidVolumeSnapshot build_grid_fluid_volume_snapshot(
    const GridFluidState& state, float emissionScale) {
    GridFluidVolumeSnapshot snapshot;
    if (!state.validate() || !finite(emissionScale) || emissionScale < 0.0F) return snapshot;
    snapshot.dimensions = state.settings.dimensions;
    snapshot.cellSize = state.settings.cellSize;
    snapshot.sourceFrame = state.frameIndex;
    snapshot.voxels.resize(state.density.size());
    for (std::size_t index = 0U; index < snapshot.voxels.size(); ++index) {
        snapshot.voxels[index] = {
            std::max(0.0F, state.density[index]),
            state.temperature[index],
            std::max(0.0F, state.flame[index] * emissionScale),
            std::max(0.0F, state.fuel[index]),
        };
    }
    return snapshot;
}

bool GridFluidParticleTransferSettings::validate(std::string* error) const {
    if (!finite(flipRatio) || flipRatio < 0.0F || flipRatio > 1.0F) {
        set_error(error, "grid-fluid FLIP ratio must be within [0, 1]");
        return false;
    }
    return true;
}

GridFluidParticleTransferTelemetry deposit_grid_fluid_particles_to_mac(
    GridFluidState& state,
    const std::vector<GridFluidParticle>& particles) {
    GridFluidParticleTransferTelemetry telemetry;
    if (!state.validate()) return telemetry;
    state.previousVelocity = state.velocity;
    state.velocity.clear();
    std::vector<float> uWeights(state.velocity.u.size(), 0.0F);
    std::vector<float> vWeights(state.velocity.v.size(), 0.0F);
    std::vector<float> wWeights(state.velocity.w.size(), 0.0F);
    const float invCell = 1.0F / state.settings.cellSize;
    for (const GridFluidParticle& particle : particles) {
        if (!finite(particle.position) || !finite(particle.velocity)) continue;
        const Float3 position = clamp_position(particle.position, state.settings);
        deposit_component(state.velocity.u, uWeights,
                          state.settings.dimensions.x + 1U,
                          state.settings.dimensions.y,
                          state.settings.dimensions.z,
                          position.x * invCell,
                          position.y * invCell - 0.5F,
                          position.z * invCell - 0.5F,
                          particle.velocity.x, telemetry);
        deposit_component(state.velocity.v, vWeights,
                          state.settings.dimensions.x,
                          state.settings.dimensions.y + 1U,
                          state.settings.dimensions.z,
                          position.x * invCell - 0.5F,
                          position.y * invCell,
                          position.z * invCell - 0.5F,
                          particle.velocity.y, telemetry);
        deposit_component(state.velocity.w, wWeights,
                          state.settings.dimensions.x,
                          state.settings.dimensions.y,
                          state.settings.dimensions.z + 1U,
                          position.x * invCell - 0.5F,
                          position.y * invCell - 0.5F,
                          position.z * invCell,
                          particle.velocity.z, telemetry);
        ++telemetry.particlesDeposited;
    }
    const auto normalize_faces = [](std::vector<float>& values,
                                    const std::vector<float>& weights) noexcept {
        for (std::size_t index = 0U; index < values.size(); ++index) {
            if (weights[index] > kEpsilon) values[index] /= weights[index];
        }
    };
    normalize_faces(state.velocity.u, uWeights);
    normalize_faces(state.velocity.v, vWeights);
    normalize_faces(state.velocity.w, wWeights);
    (void)enforce_boundaries(state);
    return telemetry;
}

GridFluidParticleTransferTelemetry update_grid_fluid_particles_from_mac(
    const GridFluidState& state,
    std::vector<GridFluidParticle>& particles,
    const GridFluidParticleTransferSettings& settings,
    float deltaSeconds) {
    GridFluidParticleTransferTelemetry telemetry;
    if (!state.validate() || !settings.validate() || !(deltaSeconds >= 0.0F) ||
        !finite(deltaSeconds)) return telemetry;
    for (GridFluidParticle& particle : particles) {
        if (!finite(particle.position) || !finite(particle.velocity)) continue;
        const Float3 oldPosition = clamp_position(particle.position, state.settings);
        const Float3 currentGridVelocity = sample_velocity(
            state.settings, state.velocity, oldPosition);
        Float3 nextVelocity = currentGridVelocity;
        if (settings.mode == GridFluidTransferMode::FlipBlend) {
            const Float3 previousGridVelocity = sample_velocity(
                state.settings, state.previousVelocity, oldPosition);
            const Float3 flipVelocity = add(particle.velocity,
                                            subtract(currentGridVelocity,
                                                     previousGridVelocity));
            nextVelocity = add(multiply(currentGridVelocity, 1.0F - settings.flipRatio),
                               multiply(flipVelocity, settings.flipRatio));
        }
        particle.velocity = nextVelocity;
        if (settings.advectParticles && deltaSeconds > 0.0F) {
            const Float3 midpoint = clamp_position(add(oldPosition,
                multiply(currentGridVelocity, 0.5F * deltaSeconds)), state.settings);
            const Float3 midpointVelocity = sample_velocity(state.settings,
                                                             state.velocity,
                                                             midpoint);
            particle.position = clamp_position(add(oldPosition,
                multiply(midpointVelocity, deltaSeconds)), state.settings);
            if (point_inside_solid(state, particle.position)) {
                particle.position = oldPosition;
                particle.velocity = {};
                ++telemetry.particlesInsideSolids;
            }
            if (particle.position.x != oldPosition.x + midpointVelocity.x * deltaSeconds ||
                particle.position.y != oldPosition.y + midpointVelocity.y * deltaSeconds ||
                particle.position.z != oldPosition.z + midpointVelocity.z * deltaSeconds) {
                ++telemetry.particlesClamped;
            }
        }
        ++telemetry.particlesUpdated;
    }
    return telemetry;
}

bool GridFluidGpuFramePlan::validate(std::string* error) const {
    if (!enabled) return true;
    if (!dimensions.validate(error) || substeps == 0U || pressureIterations == 0U ||
        estimatedBytes == 0U || dispatches.empty()) {
        set_error(error, "enabled grid-fluid GPU plan is incomplete");
        return false;
    }
    if (dispatches.back().pass != GridFluidGpuPassKind::BuildVolumeOutput &&
        dispatches.back().pass != GridFluidGpuPassKind::GridToParticle &&
        dispatches.back().pass != GridFluidGpuPassKind::ProjectVelocity) {
        set_error(error, "grid-fluid GPU plan terminates at an invalid pass");
        return false;
    }
    return true;
}

GridFluidGpuFramePlan plan_grid_fluid_gpu_frame(const GridFluidSettings& settings,
                                                std::uint32_t substeps,
                                                bool hasEmitters,
                                                bool useParticles,
                                                bool buildVolumeOutput) {
    GridFluidGpuFramePlan plan;
    if (!settings.validate() || substeps == 0U) return plan;
    plan.enabled = true;
    plan.dimensions = settings.dimensions;
    plan.substeps = substeps;
    plan.pressureIterations = settings.pressureMaximumIterations;
    plan.usesMacCormackScalars = settings.scalarAdvection == GridFluidAdvection::MacCormack;
    plan.usesMacCormackVelocity = settings.velocityAdvection == GridFluidAdvection::MacCormack;
    plan.usesParticles = useParticles;
    plan.estimatedBytes = estimate_grid_fluid_bytes(settings);
    const std::uint32_t groupsX = divide_round_up(settings.dimensions.x, 4U);
    const std::uint32_t groupsY = divide_round_up(settings.dimensions.y, 4U);
    const std::uint32_t groupsZ = divide_round_up(settings.dimensions.z, 4U);
    const auto add_pass = [&](GridFluidGpuPassKind pass, std::uint32_t iteration = 0U) {
        plan.dispatches.push_back({pass, iteration, groupsX, groupsY, groupsZ});
    };
    for (std::uint32_t substep = 0U; substep < substeps; ++substep) {
        if (hasEmitters) add_pass(GridFluidGpuPassKind::ApplyEmitters, substep);
        if (useParticles) {
            add_pass(GridFluidGpuPassKind::ParticleToGrid, substep);
            add_pass(GridFluidGpuPassKind::NormalizeParticleGrid, substep);
        }
        add_pass(GridFluidGpuPassKind::AdvectVelocityForward, substep);
        if (plan.usesMacCormackVelocity) {
            add_pass(GridFluidGpuPassKind::AdvectVelocityBackward, substep);
            add_pass(GridFluidGpuPassKind::CorrectVelocity, substep);
        }
        add_pass(GridFluidGpuPassKind::AdvectScalarsForward, substep);
        if (plan.usesMacCormackScalars) {
            add_pass(GridFluidGpuPassKind::AdvectScalarsBackward, substep);
            add_pass(GridFluidGpuPassKind::CorrectScalars, substep);
        }
        if (settings.combustionEnabled) add_pass(GridFluidGpuPassKind::Combustion, substep);
        add_pass(GridFluidGpuPassKind::ApplyForces, substep);
        if (settings.vorticityConfinement > 0.0F) {
            add_pass(GridFluidGpuPassKind::VorticityConfinement, substep);
        }
        add_pass(GridFluidGpuPassKind::EnforceBoundaries, substep);
        add_pass(GridFluidGpuPassKind::ComputeDivergence, substep);
        for (std::uint32_t iteration = 0U;
             iteration < settings.pressureMaximumIterations; ++iteration) {
            add_pass(GridFluidGpuPassKind::PressureSolve, iteration);
        }
        add_pass(GridFluidGpuPassKind::ProjectVelocity, substep);
        if (useParticles) add_pass(GridFluidGpuPassKind::GridToParticle, substep);
    }
    if (buildVolumeOutput) add_pass(GridFluidGpuPassKind::BuildVolumeOutput, 0U);
    return plan;
}

} // namespace dve
