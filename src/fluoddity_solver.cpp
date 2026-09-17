#include "dve/fluoddity_solver.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace dve {
namespace {

constexpr float kPi = 3.14159265358979323846F;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] std::size_t voxel_count(std::uint32_t resolution) noexcept {
    const std::size_t side = static_cast<std::size_t>(resolution);
    return side * side * side;
}

[[nodiscard]] std::size_t voxel_index(std::uint32_t resolution,
                                      std::uint32_t x,
                                      std::uint32_t y,
                                      std::uint32_t z) noexcept {
    const std::size_t side = static_cast<std::size_t>(resolution);
    return (static_cast<std::size_t>(z) * side + static_cast<std::size_t>(y)) * side +
           static_cast<std::size_t>(x);
}

[[nodiscard]] std::uint32_t pcg_hash(std::uint32_t seed) noexcept {
    const std::uint32_t state = seed * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

[[nodiscard]] float hash01(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<float>(pcg_hash(a ^ pcg_hash(b))) /
           static_cast<float>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] Float3 safe_normalize(Float3 value, Float3 fallback) noexcept {
    const float magnitudeSquared = length_squared(value);
    if (!(magnitudeSquared > 1.0e-12F) || !std::isfinite(magnitudeSquared)) return fallback;
    return multiply(value, 1.0F / std::sqrt(magnitudeSquared));
}

[[nodiscard]] float clamp_component(float value, float halfExtent) noexcept {
    return std::clamp(value, -halfExtent, halfExtent);
}

[[nodiscard]] float wrapped_component(float value, float halfExtent) noexcept {
    const float width = 2.0F * halfExtent;
    if (!(width > 0.0F)) return 0.0F;
    float normalized = std::fmod(value + halfExtent, width);
    if (normalized < 0.0F) normalized += width;
    return normalized - halfExtent;
}

[[nodiscard]] std::uint32_t coordinate_from_position(float value, float halfExtent,
                                                     std::uint32_t resolution) noexcept {
    const float normalized = std::clamp(value / (2.0F * halfExtent) + 0.5F, 0.0F,
                                        std::nextafter(1.0F, 0.0F));
    return std::min(resolution - 1U,
                    static_cast<std::uint32_t>(normalized * static_cast<float>(resolution)));
}

[[nodiscard]] FluoddityTrailVoxel lerp_voxel(const FluoddityTrailVoxel& a,
                                                const FluoddityTrailVoxel& b,
                                                float t) noexcept {
    return {add(multiply(a.velocity, 1.0F - t), multiply(b.velocity, t)),
            a.density * (1.0F - t) + b.density * t};
}

[[nodiscard]] float grid_coordinate(float value, float halfExtent,
                                    std::uint32_t resolution) noexcept {
    const float normalized = std::clamp(value / (2.0F * halfExtent) + 0.5F, 0.0F, 1.0F);
    return normalized * static_cast<float>(resolution - 1U);
}

[[nodiscard]] std::int32_t fixed_point(float value, float scale,
                                       std::uint64_t& saturations) noexcept {
    const double scaled = static_cast<double>(value) * static_cast<double>(scale);
    const double minimum = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    const double maximum = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    if (scaled <= minimum) {
        ++saturations;
        return std::numeric_limits<std::int32_t>::min();
    }
    if (scaled >= maximum) {
        ++saturations;
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(std::llround(scaled));
}

void saturating_add(std::int32_t& destination, std::int32_t value,
                    std::uint64_t& saturations) noexcept {
    const std::int64_t sum = static_cast<std::int64_t>(destination) +
                             static_cast<std::int64_t>(value);
    if (sum < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())) {
        destination = std::numeric_limits<std::int32_t>::min();
        ++saturations;
    } else if (sum > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        destination = std::numeric_limits<std::int32_t>::max();
        ++saturations;
    } else {
        destination = static_cast<std::int32_t>(sum);
    }
}

[[nodiscard]] float gravity_expand(float control) noexcept {
    constexpr float maximum = 0.5F;
    constexpr float decades = 4.0F;
    constexpr float knee = 0.05F;
    const float amount = std::abs(control);
    const float sign = control < 0.0F ? -1.0F : 1.0F;
    const float kneeValue = maximum * std::pow(10.0F, decades * (knee - 1.0F));
    if (amount <= knee) return sign * kneeValue * (amount / knee);
    return sign * maximum * std::pow(10.0F, decades * (amount - 1.0F));
}

[[nodiscard]] std::array<float, 6> mirrored_rule_output(
    const FluoddityRuleAsset& asset, Float3 left, Float3 right) noexcept {
    const std::array<float, 6> input{left.x, left.y, right.x, right.y, left.z, right.z};
    const auto base = evaluate_fluoddity_rule(asset.rule, input);
    if (asset.disableSymmetry) return base;
    const std::array<float, 6> mirroredInput{left.x, left.y, right.x, right.y, -left.z, -right.z};
    const auto mirror = evaluate_fluoddity_rule(asset.rule, mirroredInput);
    return {base[0] + mirror[0], base[1] + mirror[1], base[2] + mirror[2],
            base[3] + mirror[3], base[4] - mirror[4], base[5] - mirror[5]};
}

[[nodiscard]] Float3 initialized_position(std::uint32_t index, std::uint32_t count,
                                          const FluoddityRuleAsset& asset) noexcept {
    const std::uint32_t cohort = count == 0U ? 0U :
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(asset.cohortCount) * index) / count);
    const float a = hash01(index, cohort + 11U) * 2.0F * kPi;
    const float z = hash01(index + 31U, cohort + 97U) * 2.0F - 1.0F;
    const float radial = std::sqrt(std::max(0.0F, 1.0F - z * z));
    const float radius = asset.initialCondition == FluoddityInitialCondition::SphericalShell
                             ? 0.5F * asset.initialSpacing
                             : 0.25F * asset.initialSpacing * hash01(index + 7U, cohort + 3U);
    if (asset.initialCondition == FluoddityInitialCondition::FlatGrid) {
        const std::uint32_t side = std::max(1U, static_cast<std::uint32_t>(std::ceil(std::sqrt(
            static_cast<float>(std::max(1U, count))))));
        const float x = (static_cast<float>(index % side) + 0.5F) / static_cast<float>(side);
        const float y = (static_cast<float>(index / side) + 0.5F) / static_cast<float>(side);
        return {(x * 2.0F - 1.0F) * asset.initialSpacing,
                (y * 2.0F - 1.0F) * asset.initialSpacing, 0.0F};
    }
    return {radius * radial * std::cos(a), radius * radial * std::sin(a), radius * z};
}

void reset_particle(FluoddityParticleState& particle, std::uint32_t index,
                    std::uint32_t particleCount, const FluoddityRuleAsset& asset) noexcept {
    particle.position = initialized_position(index, particleCount, asset);
    const float azimuth = hash01(index + 113U, 29U) * 2.0F * kPi;
    const float z = hash01(index + 211U, 71U) * 2.0F - 1.0F;
    const float radial = std::sqrt(std::max(0.0F, 1.0F - z * z));
    particle.velocity = multiply({radial * std::cos(azimuth), radial * std::sin(azimuth), z},
                                 0.001F);
    particle.cohort = particleCount == 0U ? 0U :
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(asset.cohortCount) * index) /
                                   particleCount);
    particle.hue = asset.colorByCohort ? hash01(particle.cohort, 0U) : 0.0F;
    particle.size = 0.00015F;
    particle.age = 0U;
}

} // namespace

FluoddityTrailVoxel sample_fluoddity_trail(
    const std::vector<FluoddityTrailVoxel>& trail,
    const FluoddityReferenceSettings& settings,
    Float3 position) noexcept {
    const std::uint32_t r = settings.trailResolution;
    if (trail.empty() || r == 0U || trail.size() != voxel_count(r)) return {};
    if (settings.trailSampling == FluoddityTrailSampling::Nearest || r == 1U) {
        const std::uint32_t x = coordinate_from_position(position.x, settings.boundsHalfExtent.x, r);
        const std::uint32_t y = coordinate_from_position(position.y, settings.boundsHalfExtent.y, r);
        const std::uint32_t z = coordinate_from_position(position.z, settings.boundsHalfExtent.z, r);
        return trail[voxel_index(r, x, y, z)];
    }
    const float gx = grid_coordinate(position.x, settings.boundsHalfExtent.x, r);
    const float gy = grid_coordinate(position.y, settings.boundsHalfExtent.y, r);
    const float gz = grid_coordinate(position.z, settings.boundsHalfExtent.z, r);
    const auto x0 = static_cast<std::uint32_t>(std::floor(gx));
    const auto y0 = static_cast<std::uint32_t>(std::floor(gy));
    const auto z0 = static_cast<std::uint32_t>(std::floor(gz));
    const std::uint32_t x1 = std::min(x0 + 1U, r - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, r - 1U);
    const std::uint32_t z1 = std::min(z0 + 1U, r - 1U);
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);
    const float tz = gz - static_cast<float>(z0);
    const auto c00 = lerp_voxel(trail[voxel_index(r, x0, y0, z0)],
                                trail[voxel_index(r, x1, y0, z0)], tx);
    const auto c10 = lerp_voxel(trail[voxel_index(r, x0, y1, z0)],
                                trail[voxel_index(r, x1, y1, z0)], tx);
    const auto c01 = lerp_voxel(trail[voxel_index(r, x0, y0, z1)],
                                trail[voxel_index(r, x1, y0, z1)], tx);
    const auto c11 = lerp_voxel(trail[voxel_index(r, x0, y1, z1)],
                                trail[voxel_index(r, x1, y1, z1)], tx);
    return lerp_voxel(lerp_voxel(c00, c10, ty), lerp_voxel(c01, c11, ty), tz);
}

SimulationStepPlan plan_fluoddity_reference_step(
    const FluoddityReferenceSettings& settings,
    const FluoddityReferenceState& state) {
    float maximumSpeed{};
    for (const auto& particle : state.particles)
        maximumSpeed = std::max(maximumSpeed, length(particle.velocity));
    const float cellX = 2.0F * settings.boundsHalfExtent.x /
                        static_cast<float>(std::max(1U, settings.trailResolution));
    const float cellY = 2.0F * settings.boundsHalfExtent.y /
                        static_cast<float>(std::max(1U, settings.trailResolution));
    const float cellZ = 2.0F * settings.boundsHalfExtent.z /
                        static_cast<float>(std::max(1U, settings.trailResolution));
    return plan_simulation_step({settings.deltaSeconds, maximumSpeed,
        std::max(1.0e-5F, std::min({cellX, cellY, cellZ})),
        settings.maximumDisplacementFraction, settings.deltaSeconds,
        settings.maximumSubsteps});
}

bool FluoddityReferenceState::validate(const FluoddityReferenceSettings& settings,
                                       std::string* error) const {
    if (settings.trailResolution == 0U || settings.trailResolution > 512U) {
        set_error(error, "Fluoddity reference trail resolution must be in [1, 512]");
        return false;
    }
    const std::size_t expected = voxel_count(settings.trailResolution);
    if (trailA.size() != expected || trailB.size() != expected || accumulation.size() != expected) {
        set_error(error, "Fluoddity reference trail and accumulation sizes do not match resolution");
        return false;
    }
    if (trailReadIndex > 1U) {
        set_error(error, "Fluoddity reference trail read index is invalid");
        return false;
    }
    if (!(settings.deltaSeconds > 0.0F) || !std::isfinite(settings.deltaSeconds) ||
        !(settings.fixedPointScale > 0.0F) || !std::isfinite(settings.fixedPointScale) ||
        !(settings.maximumDisplacementFraction > 0.0F) ||
        settings.maximumDisplacementFraction > 1.0F ||
        settings.maximumSubsteps == 0U || settings.maximumSubsteps > 64U) {
        set_error(error, "Fluoddity reference timestep or fixed-point scale is invalid");
        return false;
    }
    return true;
}

FluoddityReferenceState make_fluoddity_reference_state(
    const FluoddityRuleAsset& asset, std::uint32_t particleCount,
    const FluoddityReferenceSettings& settings) {
    FluoddityReferenceState state;
    state.particles.resize(particleCount);
    for (std::uint32_t index = 0U; index < particleCount; ++index)
        reset_particle(state.particles[index], index, particleCount, asset);
    const std::size_t count = voxel_count(settings.trailResolution);
    state.trailA.resize(count);
    state.trailB.resize(count);
    state.accumulation.resize(count);
    return state;
}

FluoddityReferenceStepTelemetry step_fluoddity_reference(
    const FluoddityRuleAsset& asset, const FluoddityReferenceSettings& settings,
    FluoddityReferenceState& state) {
    FluoddityReferenceStepTelemetry telemetry;
    std::string validation;
    if (!state.validate(settings, &validation) || !asset.validate(nullptr)) {
        telemetry.nonFiniteCorrections = state.particles.size();
        return telemetry;
    }
    auto& readTrail = state.trailReadIndex == 0U ? state.trailA : state.trailB;
    auto& writeTrail = state.trailReadIndex == 0U ? state.trailB : state.trailA;
    std::fill(state.accumulation.begin(), state.accumulation.end(), FluoddityFixedPointVoxel{});

    const auto& sensorAngleSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::SensorAngle)];
    const auto& sensorDistanceSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::SensorDistance)];
    const auto& sensorGainSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::SensorGain)];
    const auto& globalSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::GlobalForceMultiplier)];
    const auto& axialSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::AxialForce)];
    const auto& lateralSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::LateralForce)];
    const auto& dragSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::Drag)];
    const auto& strafeSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::StrafePower)];
    const auto& hazardSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::HazardRate)];
    const auto& persistenceSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::TrailPersistence)];

    const std::uint32_t particleCount = static_cast<std::uint32_t>(state.particles.size());
    for (std::uint32_t index = 0U; index < particleCount; ++index) {
        auto& particle = state.particles[index];
        const float cohort = static_cast<float>(particle.cohort);
        const float hazard = std::clamp(evaluate_fluoddity_parameter(
            hazardSetting, particle.position.x, particle.position.y, cohort, asset.cohortCount,
            settings.frameNumber), 0.0F, 1.0F);
        if (hash01(index, settings.frameNumber) < hazard) {
            reset_particle(particle, index, particleCount, asset);
            ++telemetry.resetParticles;
            continue;
        }

        const Float3 velocityDirection = safe_normalize(particle.velocity, {1.0F, 0.0F, 0.0F});
        const Float3 environment = safe_normalize(sample_fluoddity_trail(readTrail, settings, particle.position).velocity,
                                                  {0.0F, 1.0F, 0.0F});
        Float3 tangent = subtract(environment, multiply(velocityDirection,
                                                        dot(environment, velocityDirection)));
        tangent = safe_normalize(tangent, safe_normalize(cross(velocityDirection,
                                                               {0.0F, 0.0F, 1.0F}),
                                                        {0.0F, 1.0F, 0.0F}));
        const Float3 normal = safe_normalize(cross(velocityDirection, tangent),
                                             {0.0F, 0.0F, 1.0F});
        const float angle = evaluate_fluoddity_parameter(sensorAngleSetting, particle.position.x,
                                                         particle.position.y, cohort,
                                                         asset.cohortCount, settings.frameNumber) * kPi;
        const float distance = 0.005F * evaluate_fluoddity_parameter(
            sensorDistanceSetting, particle.position.x, particle.position.y, cohort,
            asset.cohortCount, settings.frameNumber);
        const Float3 leftOffset = add(multiply(velocityDirection, std::cos(angle)),
                                      multiply(tangent, std::sin(angle)));
        const Float3 rightOffset = subtract(multiply(velocityDirection, std::cos(angle)),
                                            multiply(tangent, std::sin(angle)));
        const float sensorGain = 38.855F * evaluate_fluoddity_parameter(
            sensorGainSetting, particle.position.x, particle.position.y, cohort,
            asset.cohortCount, settings.frameNumber);
        const Float3 leftWorld = multiply(sample_fluoddity_trail(readTrail, settings,
                                                       add(particle.position,
                                                           multiply(leftOffset, distance))).velocity,
                                          sensorGain);
        const Float3 rightWorld = multiply(sample_fluoddity_trail(readTrail, settings,
                                                        add(particle.position,
                                                            multiply(rightOffset, distance))).velocity,
                                           sensorGain);
        const Float3 leftLocal{dot(leftWorld, velocityDirection), dot(leftWorld, tangent),
                               dot(leftWorld, normal)};
        const Float3 rightLocal{dot(rightWorld, velocityDirection), dot(rightWorld, tangent),
                                dot(rightWorld, normal)};
        const auto output = mirrored_rule_output(asset, leftLocal, rightLocal);
        const float axial = evaluate_fluoddity_parameter(axialSetting, particle.position.x,
                                                         particle.position.y, cohort,
                                                         asset.cohortCount, settings.frameNumber);
        const float lateral = evaluate_fluoddity_parameter(lateralSetting, particle.position.x,
                                                           particle.position.y, cohort,
                                                           asset.cohortCount, settings.frameNumber);
        const float global = evaluate_fluoddity_parameter(globalSetting, particle.position.x,
                                                          particle.position.y, cohort,
                                                          asset.cohortCount, settings.frameNumber);
        const Float3 forceLocal{output[0] * axial, output[1] * lateral, output[4] * axial};
        const Float3 strafeLocal{output[2] * axial, output[3] * lateral, output[5] * axial};
        const Float3 forceWorld = multiply(add(add(multiply(velocityDirection, forceLocal.x),
                                                   multiply(tangent, forceLocal.y)),
                                               multiply(normal, forceLocal.z)),
                                           global / 400.0F);
        const Float3 strafeWorld = multiply(add(add(multiply(velocityDirection, strafeLocal.x),
                                                    multiply(tangent, strafeLocal.y)),
                                                multiply(normal, strafeLocal.z)),
                                            global / 20.0F);
        const float drag = evaluate_fluoddity_parameter(dragSetting, particle.position.x,
                                                        particle.position.y, cohort,
                                                        asset.cohortCount, settings.frameNumber);
        particle.velocity = add(multiply(particle.velocity, drag), forceWorld);
        particle.velocity.y -= 0.01F * gravity_expand(asset.gravityForce);
        const float speed = length(particle.velocity);
        if (!std::isfinite(speed)) {
            particle.velocity = {};
            ++telemetry.nonFiniteCorrections;
        } else if (speed > settings.maximumSpeed && speed > 0.0F) {
            particle.velocity = multiply(particle.velocity, settings.maximumSpeed / speed);
        }
        telemetry.maximumObservedSpeed = std::max(telemetry.maximumObservedSpeed,
                                                   length(particle.velocity));
        const float strafePower = evaluate_fluoddity_parameter(
            strafeSetting, particle.position.x, particle.position.y, cohort,
            asset.cohortCount, settings.frameNumber);
        particle.position = add(particle.position,
                                multiply(add(particle.velocity,
                                             multiply(strafeWorld, strafePower)),
                                         settings.deltaSeconds * 60.0F));
        particle.position.y -= 0.01F * gravity_expand(asset.gravityStrafe);

        bool resetForBoundary = false;
        auto handleBoundary = [&](float& position, float& velocity, float halfExtent) {
            if (position >= -halfExtent && position <= halfExtent) return;
            if (asset.boundaryMode == FluoddityBoundaryMode::Wrap) {
                position = wrapped_component(position, halfExtent);
            } else if (asset.boundaryMode == FluoddityBoundaryMode::Bounce) {
                position = clamp_component(position, halfExtent);
                velocity = -velocity;
            } else {
                resetForBoundary = true;
            }
        };
        handleBoundary(particle.position.x, particle.velocity.x, settings.boundsHalfExtent.x);
        handleBoundary(particle.position.y, particle.velocity.y, settings.boundsHalfExtent.y);
        handleBoundary(particle.position.z, particle.velocity.z, settings.boundsHalfExtent.z);
        if (resetForBoundary) {
            reset_particle(particle, index, particleCount, asset);
            ++telemetry.resetParticles;
            continue;
        }
        ++particle.age;
        particle.hue = asset.colorByCohort ? hash01(particle.cohort, 0U) :
            std::abs(asset.hueSensitivity * output[0]);
        ++telemetry.movedParticles;

        const float persistence = std::clamp(evaluate_fluoddity_parameter(
            persistenceSetting, particle.position.x, particle.position.y, cohort,
            asset.cohortCount, settings.frameNumber), 0.001F, 0.999F);
        const float scale = ((1.0F - persistence) / persistence) * settings.depositStrength;
        const std::uint32_t x = coordinate_from_position(particle.position.x,
                                                         settings.boundsHalfExtent.x,
                                                         settings.trailResolution);
        const std::uint32_t y = coordinate_from_position(particle.position.y,
                                                         settings.boundsHalfExtent.y,
                                                         settings.trailResolution);
        const std::uint32_t z = coordinate_from_position(particle.position.z,
                                                         settings.boundsHalfExtent.z,
                                                         settings.trailResolution);
        auto& voxel = state.accumulation[voxel_index(settings.trailResolution, x, y, z)];
        saturating_add(voxel.channels[0], fixed_point(particle.velocity.x * scale,
                                                      settings.fixedPointScale,
                                                      telemetry.saturatedAtomicAdds),
                       telemetry.saturatedAtomicAdds);
        saturating_add(voxel.channels[1], fixed_point(particle.velocity.y * scale,
                                                      settings.fixedPointScale,
                                                      telemetry.saturatedAtomicAdds),
                       telemetry.saturatedAtomicAdds);
        saturating_add(voxel.channels[2], fixed_point(particle.velocity.z * scale,
                                                      settings.fixedPointScale,
                                                      telemetry.saturatedAtomicAdds),
                       telemetry.saturatedAtomicAdds);
        if (asset.trailMode == FluoddityTrailMode::VelocityRgbDensityA) {
            saturating_add(voxel.channels[3], fixed_point(scale, settings.fixedPointScale,
                                                          telemetry.saturatedAtomicAdds),
                           telemetry.saturatedAtomicAdds);
        }
        ++telemetry.depositedParticles;
    }

    const auto& diffusionSetting = asset.parameters[static_cast<std::size_t>(FluoddityParameter::TrailDiffusion)];
    const std::uint32_t r = settings.trailResolution;
    const float inverseScale = 1.0F / settings.fixedPointScale;
    auto fetch = [&](std::int32_t x, std::int32_t y, std::int32_t z) -> const FluoddityTrailVoxel& {
        auto coordinate = [&](std::int32_t value) -> std::uint32_t {
            if (asset.boundaryMode == FluoddityBoundaryMode::Wrap) {
                const std::int32_t side = static_cast<std::int32_t>(r);
                value %= side;
                if (value < 0) value += side;
                return static_cast<std::uint32_t>(value);
            }
            return static_cast<std::uint32_t>(std::clamp(value, 0, static_cast<std::int32_t>(r) - 1));
        };
        return readTrail[voxel_index(r, coordinate(x), coordinate(y), coordinate(z))];
    };
    for (std::uint32_t z = 0U; z < r; ++z) {
        for (std::uint32_t y = 0U; y < r; ++y) {
            for (std::uint32_t x = 0U; x < r; ++x) {
                const std::size_t index = voxel_index(r, x, y, z);
                const float positionX = (static_cast<float>(x) + 0.5F) / static_cast<float>(r) * 2.0F - 1.0F;
                const float positionY = (static_cast<float>(y) + 0.5F) / static_cast<float>(r) * 2.0F - 1.0F;
                float diffusion = std::clamp(evaluate_fluoddity_parameter(
                    diffusionSetting, positionX, positionY, 0.0F, 1U, settings.frameNumber),
                    0.001F, 1.0F);
                diffusion *= diffusion;
                const float centerWeight = 4.0F / (std::pow(5.0F, diffusion) - 1.0F);
                const auto& center = readTrail[index];
                Float3 neighborSum{};
                neighborSum = add(neighborSum, fetch(static_cast<std::int32_t>(x) + 1,
                                                      static_cast<std::int32_t>(y),
                                                      static_cast<std::int32_t>(z)).velocity);
                neighborSum = add(neighborSum, fetch(static_cast<std::int32_t>(x) - 1,
                                                      static_cast<std::int32_t>(y),
                                                      static_cast<std::int32_t>(z)).velocity);
                neighborSum = add(neighborSum, fetch(static_cast<std::int32_t>(x),
                                                      static_cast<std::int32_t>(y) + 1,
                                                      static_cast<std::int32_t>(z)).velocity);
                neighborSum = add(neighborSum, fetch(static_cast<std::int32_t>(x),
                                                      static_cast<std::int32_t>(y) - 1,
                                                      static_cast<std::int32_t>(z)).velocity);
                neighborSum = add(neighborSum, fetch(static_cast<std::int32_t>(x),
                                                      static_cast<std::int32_t>(y),
                                                      static_cast<std::int32_t>(z) + 1).velocity);
                neighborSum = add(neighborSum, fetch(static_cast<std::int32_t>(x),
                                                      static_cast<std::int32_t>(y),
                                                      static_cast<std::int32_t>(z) - 1).velocity);
                const Float3 accumulated{
                    static_cast<float>(state.accumulation[index].channels[0]) * inverseScale,
                    static_cast<float>(state.accumulation[index].channels[1]) * inverseScale,
                    static_cast<float>(state.accumulation[index].channels[2]) * inverseScale};
                const float density = static_cast<float>(state.accumulation[index].channels[3]) * inverseScale;
                Float3 blurred = multiply(add(multiply(center.velocity, centerWeight), neighborSum),
                                          1.0F / (centerWeight + 6.0F));
                const float persistence = std::clamp(evaluate_fluoddity_parameter(
                    persistenceSetting, positionX, positionY, 0.0F, 1U, settings.frameNumber),
                    0.0F, 0.999F);
                writeTrail[index].velocity = multiply(add(blurred, accumulated), persistence);
                writeTrail[index].density = asset.trailMode == FluoddityTrailMode::VelocityRgbDensityA
                                                ? (center.density + density) * persistence
                                                : 0.0F;
            }
        }
    }
    state.trailReadIndex ^= 1U;
    return telemetry;
}

const std::vector<FluoddityTrailVoxel>& fluoddity_read_trail(
    const FluoddityReferenceState& state) noexcept {
    return state.trailReadIndex == 0U ? state.trailA : state.trailB;
}

} // namespace dve
