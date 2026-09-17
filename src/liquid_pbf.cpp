#include "dve/liquid_pbf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace dve {
namespace {

constexpr float kPi = 3.14159265358979323846F;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] std::int64_t cell_key(std::int32_t x, std::int32_t y,
                                    std::int32_t z) noexcept {
    constexpr std::int64_t bias = 1LL << 20;
    const std::int64_t bx = static_cast<std::int64_t>(x) + bias;
    const std::int64_t by = static_cast<std::int64_t>(y) + bias;
    const std::int64_t bz = static_cast<std::int64_t>(z) + bias;
    return (bx & 0x1FFFFFLL) | ((by & 0x1FFFFFLL) << 21LL) |
           ((bz & 0x1FFFFFLL) << 42LL);
}

[[nodiscard]] std::array<std::int32_t, 3> cell(Float3 position, float cellSize) noexcept {
    return {static_cast<std::int32_t>(std::floor(position.x / cellSize)),
            static_cast<std::int32_t>(std::floor(position.y / cellSize)),
            static_cast<std::int32_t>(std::floor(position.z / cellSize))};
}

[[nodiscard]] float poly6(float distanceSquared, float h) noexcept {
    const float hSquared = h * h;
    if (distanceSquared >= hSquared) return 0.0F;
    const float difference = hSquared - distanceSquared;
    const float coefficient = 315.0F / (64.0F * kPi * std::pow(h, 9.0F));
    return coefficient * difference * difference * difference;
}

[[nodiscard]] Float3 spiky_gradient(Float3 difference, float h) noexcept {
    const float distance = length(difference);
    if (!(distance > 1.0e-7F) || distance >= h) return {};
    const float coefficient = -45.0F / (kPi * std::pow(h, 6.0F));
    const float scale = coefficient * (h - distance) * (h - distance) / distance;
    return multiply(difference, scale);
}

using NeighborList = std::vector<std::vector<std::uint32_t>>;

[[nodiscard]] NeighborList build_neighbors(const std::vector<PbfLiquidParticle>& particles,
                                            float h,
                                            std::uint32_t maximumNeighbors,
                                            PbfLiquidTelemetry& telemetry) {
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells;
    cells.reserve(particles.size() * 2U + 1U);
    for (std::uint32_t index = 0U; index < particles.size(); ++index) {
        const auto coordinate = cell(particles[index].predictedPosition, h);
        cells[cell_key(coordinate[0], coordinate[1], coordinate[2])].push_back(index);
    }
    NeighborList neighbors(particles.size());
    const float hSquared = h * h;
    for (std::uint32_t index = 0U; index < particles.size(); ++index) {
        const auto coordinate = cell(particles[index].predictedPosition, h);
        auto& list = neighbors[index];
        for (std::int32_t z = -1; z <= 1; ++z) {
            for (std::int32_t y = -1; y <= 1; ++y) {
                for (std::int32_t x = -1; x <= 1; ++x) {
                    const auto found = cells.find(cell_key(coordinate[0] + x,
                                                          coordinate[1] + y,
                                                          coordinate[2] + z));
                    if (found == cells.end()) continue;
                    for (const std::uint32_t other : found->second) {
                        if (other == index) continue;
                        if (length_squared(subtract(particles[index].predictedPosition,
                                                   particles[other].predictedPosition)) < hSquared) {
                            if (list.size() < maximumNeighbors) {
                                list.push_back(other);
                                ++telemetry.neighborPairs;
                            } else {
                                ++telemetry.neighborOverflow;
                            }
                        }
                    }
                }
            }
        }
        telemetry.maximumNeighbors = std::max(telemetry.maximumNeighbors,
                                               static_cast<std::uint32_t>(list.size()));
    }
    return neighbors;
}

void apply_bounds(PbfLiquidParticle& particle, const PbfLiquidSettings& settings,
                  PbfLiquidTelemetry& telemetry) noexcept {
    const float radius = settings.particleRadius;
    auto clampAxis = [&](float& value, float minimum, float maximum) {
        const float bounded = std::clamp(value, minimum + radius, maximum - radius);
        if (bounded != value) {
            value = bounded;
            ++telemetry.boundaryContacts;
        }
    };
    clampAxis(particle.predictedPosition.x, settings.boundsMinimum.x, settings.boundsMaximum.x);
    clampAxis(particle.predictedPosition.y, settings.boundsMinimum.y, settings.boundsMaximum.y);
    clampAxis(particle.predictedPosition.z, settings.boundsMinimum.z, settings.boundsMaximum.z);
}

[[nodiscard]] std::uint32_t next_power_of_two(std::uint32_t value) noexcept {
    if (value <= 1U) return 1U;
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
}

[[nodiscard]] std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) noexcept {
    return (value + divisor - 1U) / divisor;
}

} // namespace

bool PbfLiquidSettings::validate(std::string* error) const {
    if (!(particleRadius > 0.0F) || !(kernelRadius > particleRadius) ||
        !(restDensity > 0.0F) || !(particleMass > 0.0F) ||
        constraintCompliance < 0.0F || !(lambdaRelaxation > 0.0F) ||
        viscosity < 0.0F || vorticityConfinement < 0.0F ||
        solverIterations == 0U || solverIterations > 32U ||
        maximumSubsteps == 0U || maximumSubsteps > 64U ||
        maximumNeighbors == 0U || maximumNeighbors > 4096U ||
        !(maximumDisplacementFraction > 0.0F) || maximumDisplacementFraction > 1.0F ||
        !(correctionClampRatio > 0.0F) || correctionClampRatio > 1.0F ||
        maximumParticles == 0U || maximumParticles > 16'777'216U) {
        set_error(error, "PBF liquid scalar settings are invalid");
        return false;
    }
    const std::array<float, 18> values{
        particleRadius, kernelRadius, restDensity, particleMass, constraintCompliance,
        lambdaRelaxation, artificialPressure, artificialPressureRadiusRatio, viscosity,
        vorticityConfinement, gravity.x, gravity.y, gravity.z, boundsMinimum.x,
        boundsMinimum.y, boundsMinimum.z, boundsMaximum.x, boundsMaximum.y};
    for (const float value : values) {
        if (!std::isfinite(value)) {
            set_error(error, "PBF liquid contains a non-finite setting");
            return false;
        }
    }
    if (!std::isfinite(boundsMaximum.z) || boundsMinimum.x >= boundsMaximum.x ||
        boundsMinimum.y >= boundsMaximum.y || boundsMinimum.z >= boundsMaximum.z) {
        set_error(error, "PBF liquid bounds are invalid");
        return false;
    }
    return true;
}

PbfLiquidWorld::PbfLiquidWorld(PbfLiquidSettings settings) : settings_(settings) {
    if (!settings_.validate(nullptr)) settings_ = {};
}

bool PbfLiquidWorld::set_settings(PbfLiquidSettings settings, std::string* error) {
    if (!settings.validate(error) || particles_.size() > settings.maximumParticles) return false;
    settings_ = settings;
    return true;
}

bool PbfLiquidWorld::add_particle(Float3 position, Float3 velocity, std::string* error) {
    if (particles_.size() >= settings_.maximumParticles) {
        set_error(error, "PBF liquid maximum particle count reached");
        return false;
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(velocity.x) ||
        !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) {
        set_error(error, "PBF particle state must be finite");
        return false;
    }
    particles_.push_back({position, position, velocity, settings_.restDensity, 0.0F});
    return true;
}

std::uint32_t PbfLiquidWorld::add_box(Float3 minimum, Float3 maximum, float spacing,
                                      Float3 initialVelocity, std::string* error) {
    if (!(spacing > 0.0F) || minimum.x > maximum.x || minimum.y > maximum.y ||
        minimum.z > maximum.z) {
        set_error(error, "PBF box emission parameters are invalid");
        return 0U;
    }
    std::uint32_t added{};
    for (float z = minimum.z; z <= maximum.z + spacing * 0.25F; z += spacing) {
        for (float y = minimum.y; y <= maximum.y + spacing * 0.25F; y += spacing) {
            for (float x = minimum.x; x <= maximum.x + spacing * 0.25F; x += spacing) {
                if (!add_particle({x, y, z}, initialVelocity, error)) return added;
                ++added;
            }
        }
    }
    return added;
}

void PbfLiquidWorld::clear() noexcept { particles_.clear(); }

SimulationStepPlan PbfLiquidWorld::plan_step(float deltaSeconds) const {
    float maximumSpeed{};
    for (const auto& particle : particles_)
        maximumSpeed = std::max(maximumSpeed, length(particle.velocity));
    return plan_simulation_step({deltaSeconds, maximumSpeed, settings_.particleRadius * 2.0F,
        settings_.maximumDisplacementFraction, 1.0F / 60.0F, settings_.maximumSubsteps});
}

PbfLiquidTelemetry PbfLiquidWorld::step(float deltaSeconds) {
    PbfLiquidTelemetry telemetry;
    if (particles_.empty() || !(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds))
        return telemetry;
    const auto stepPlan = plan_step(deltaSeconds);
    if (!stepPlan.enabled) return telemetry;
    telemetry.substeps = stepPlan.substeps;
    telemetry.substepBudgetClamped = stepPlan.clamped;
    const float dt = stepPlan.substepSeconds;
    for (std::uint32_t substep = 0U; substep < stepPlan.substeps; ++substep) {
        for (auto& particle : particles_) {
            particle.velocity = add(particle.velocity, multiply(settings_.gravity, dt));
            particle.predictedPosition = add(particle.position, multiply(particle.velocity, dt));
            apply_bounds(particle, settings_, telemetry);
            ++telemetry.particlesIntegrated;
        }
        NeighborList neighbors = build_neighbors(particles_, settings_.kernelRadius,
                                                  settings_.maximumNeighbors, telemetry);
        const float selfKernel = poly6(0.0F, settings_.kernelRadius);
        const float alpha = settings_.constraintCompliance / (dt * dt);
        const float referenceKernel = poly6(
            settings_.kernelRadius * settings_.kernelRadius *
                settings_.artificialPressureRadiusRatio *
                settings_.artificialPressureRadiusRatio,
            settings_.kernelRadius);
        std::vector<Float3> corrections(particles_.size());
        for (std::uint32_t iteration = 0U; iteration < settings_.solverIterations; ++iteration) {
            if (iteration > 0U && settings_.rebuildNeighborsEachIteration) {
                neighbors = build_neighbors(particles_, settings_.kernelRadius,
                                            settings_.maximumNeighbors, telemetry);
            }
            for (std::uint32_t index = 0U; index < particles_.size(); ++index) {
                auto& particle = particles_[index];
                float density = settings_.particleMass * selfKernel;
                Float3 gradientI{};
                float gradientSquared{};
                for (const std::uint32_t other : neighbors[index]) {
                    const Float3 difference = subtract(particle.predictedPosition,
                                                       particles_[other].predictedPosition);
                    density += settings_.particleMass *
                               poly6(length_squared(difference), settings_.kernelRadius);
                    const Float3 gradientJ = multiply(spiky_gradient(
                        difference, settings_.kernelRadius),
                        -settings_.particleMass / settings_.restDensity);
                    gradientI = subtract(gradientI, gradientJ);
                    gradientSquared += length_squared(gradientJ);
                }
                gradientSquared += length_squared(gradientI);
                particle.density = density;
                const float constraint = density / settings_.restDensity - 1.0F;
                telemetry.maximumDensityError = std::max(telemetry.maximumDensityError,
                                                          std::abs(constraint));
                particle.lambda = -constraint /
                    (gradientSquared + settings_.lambdaRelaxation + alpha);
                ++telemetry.densityConstraints;
            }
            std::fill(corrections.begin(), corrections.end(), Float3{});
            const float maximumCorrection = settings_.kernelRadius *
                                            settings_.correctionClampRatio;
            for (std::uint32_t index = 0U; index < particles_.size(); ++index) {
                Float3 correction{};
                for (const std::uint32_t other : neighbors[index]) {
                    const Float3 difference = subtract(particles_[index].predictedPosition,
                                                       particles_[other].predictedPosition);
                    const float kernel = poly6(length_squared(difference),
                                               settings_.kernelRadius);
                    float pressureCorrection{};
                    if (referenceKernel > 0.0F) {
                        const float ratio = kernel / referenceKernel;
                        pressureCorrection = -settings_.artificialPressure *
                                             ratio * ratio * ratio * ratio;
                    }
                    correction = add(correction,
                        multiply(spiky_gradient(difference, settings_.kernelRadius),
                                 particles_[index].lambda + particles_[other].lambda +
                                     pressureCorrection));
                }
                correction = multiply(correction, 1.0F / settings_.restDensity);
                const float magnitude = length(correction);
                if (magnitude > maximumCorrection && magnitude > 0.0F)
                    correction = multiply(correction, maximumCorrection / magnitude);
                corrections[index] = correction;
            }
            for (std::size_t index = 0U; index < particles_.size(); ++index) {
                particles_[index].predictedPosition = add(
                    particles_[index].predictedPosition, corrections[index]);
                apply_bounds(particles_[index], settings_, telemetry);
                ++telemetry.positionCorrections;
            }
        }
        if (settings_.rebuildNeighborsEachIteration)
            neighbors = build_neighbors(particles_, settings_.kernelRadius,
                                        settings_.maximumNeighbors, telemetry);
        std::vector<Float3> newVelocities(particles_.size());
        for (std::size_t index = 0U; index < particles_.size(); ++index) {
            newVelocities[index] = multiply(
                subtract(particles_[index].predictedPosition, particles_[index].position),
                1.0F / dt);
        }
        if (settings_.viscosity > 0.0F) {
            std::vector<Float3> viscosityDelta(particles_.size());
            for (std::uint32_t index = 0U; index < particles_.size(); ++index) {
                Float3 delta{};
                float weightSum{};
                for (const std::uint32_t other : neighbors[index]) {
                    const float weight = poly6(length_squared(subtract(
                        particles_[index].predictedPosition,
                        particles_[other].predictedPosition)), settings_.kernelRadius);
                    delta = add(delta, multiply(subtract(newVelocities[other],
                                                         newVelocities[index]), weight));
                    weightSum += weight;
                }
                if (weightSum > 1.0e-12F)
                    viscosityDelta[index] = multiply(delta,
                        settings_.viscosity / weightSum);
            }
            for (std::size_t index = 0U; index < particles_.size(); ++index)
                newVelocities[index] = add(newVelocities[index], viscosityDelta[index]);
        }
        if (settings_.vorticityConfinement > 0.0F) {
            std::vector<Float3> vorticity(particles_.size());
            for (std::uint32_t index = 0U; index < particles_.size(); ++index) {
                Float3 omega{};
                for (const std::uint32_t other : neighbors[index]) {
                    const Float3 difference = subtract(particles_[index].predictedPosition,
                                                       particles_[other].predictedPosition);
                    omega = add(omega, cross(subtract(newVelocities[other],
                                                     newVelocities[index]),
                                             spiky_gradient(difference,
                                                            settings_.kernelRadius)));
                }
                vorticity[index] = omega;
            }
            for (std::uint32_t index = 0U; index < particles_.size(); ++index) {
                Float3 magnitudeGradient{};
                const float ownMagnitude = length(vorticity[index]);
                for (const std::uint32_t other : neighbors[index]) {
                    const Float3 difference = subtract(particles_[index].predictedPosition,
                                                       particles_[other].predictedPosition);
                    magnitudeGradient = add(magnitudeGradient,
                        multiply(spiky_gradient(difference, settings_.kernelRadius),
                                 length(vorticity[other]) - ownMagnitude));
                }
                const float gradientLength = length(magnitudeGradient);
                if (gradientLength > 1.0e-8F) {
                    const Float3 normal = multiply(magnitudeGradient,
                                                   1.0F / gradientLength);
                    const Float3 confinement = multiply(cross(normal, vorticity[index]),
                        settings_.vorticityConfinement * dt);
                    newVelocities[index] = add(newVelocities[index], confinement);
                    ++telemetry.vorticityUpdates;
                }
            }
        }
        for (std::size_t index = 0U; index < particles_.size(); ++index) {
            auto& particle = particles_[index];
            particle.position = particle.predictedPosition;
            particle.velocity = newVelocities[index];
            if (!std::isfinite(particle.position.x) ||
                !std::isfinite(particle.position.y) ||
                !std::isfinite(particle.position.z) ||
                !std::isfinite(particle.velocity.x) ||
                !std::isfinite(particle.velocity.y) ||
                !std::isfinite(particle.velocity.z)) {
                particle.position = {0.0F,
                    settings_.boundsMaximum.y - settings_.particleRadius, 0.0F};
                particle.predictedPosition = particle.position;
                particle.velocity = {};
                ++telemetry.nonFiniteCorrections;
            }
        }
    }
    return telemetry;
}

bool PbfGpuFramePlan::validate(std::string* error) const {
    if (!enabled) return true;
    if (particleCount == 0U || paddedSortCount < particleCount || solverIterations == 0U ||
        dispatches.empty() || estimatedBytes == 0U) {
        set_error(error, "enabled PBF GPU plan is incomplete");
        return false;
    }
    if (dispatches.front().pass != PbfGpuPassKind::Predict ||
        (dispatches.back().pass != PbfGpuPassKind::ApplyViscosity &&
         dispatches.back().pass != PbfGpuPassKind::BuildSurfaceData)) {
        set_error(error, "PBF GPU pass ordering is invalid");
        return false;
    }
    return true;
}

PbfGpuFramePlan plan_pbf_gpu_frame(const PbfLiquidSettings& settings,
                                   std::uint32_t particleCount,
                                   bool buildSurfaceData) {
    PbfGpuFramePlan plan;
    plan.enabled = settings.validate(nullptr) && particleCount > 0U &&
                   particleCount <= settings.maximumParticles;
    plan.particleCount = particleCount;
    plan.paddedSortCount = next_power_of_two(std::max(1U, particleCount));
    plan.solverIterations = settings.solverIterations;
    if (!plan.enabled) return plan;
    constexpr std::uint64_t particleStride = 64U;
    plan.estimatedBytes = static_cast<std::uint64_t>(plan.paddedSortCount) *
        (particleStride * 2U + sizeof(std::uint32_t) * 6U) +
        static_cast<std::uint64_t>(particleCount) * sizeof(Float3) * 2U;
    const std::uint32_t groups = ceil_div(particleCount, 128U);
    const std::uint32_t sortGroups = ceil_div(plan.paddedSortCount, 256U);
    plan.dispatches.push_back({PbfGpuPassKind::Predict, 0U, groups});
    plan.dispatches.push_back({PbfGpuPassKind::BuildSpatialKeys, 0U, groups});
    plan.dispatches.push_back({PbfGpuPassKind::RadixSort, 0U, sortGroups});
    plan.dispatches.push_back({PbfGpuPassKind::BuildCellRanges, 0U, groups});
    for (std::uint32_t iteration = 0U; iteration < settings.solverIterations; ++iteration) {
        plan.dispatches.push_back({PbfGpuPassKind::ComputeDensityLambda, iteration, groups});
        plan.dispatches.push_back({PbfGpuPassKind::CorrectPositions, iteration, groups});
        plan.dispatches.push_back({PbfGpuPassKind::ApplyBoundaries, iteration, groups});
    }
    plan.dispatches.push_back({PbfGpuPassKind::UpdateVelocity, 0U, groups});
    plan.dispatches.push_back({PbfGpuPassKind::ApplyViscosity, 0U, groups});
    if (buildSurfaceData)
        plan.dispatches.push_back({PbfGpuPassKind::BuildSurfaceData, 0U, groups});
    return plan;
}


bool SpatiotemporalFlipPlan::validate(std::string* error) const {
    if (!enabled) return true;
    if (temporalSamples == 0U || timeSlabs == 0U || samples.empty() ||
        !std::isfinite(jitterAmplitude) || jitterAmplitude < 0.0F ||
        jitterAmplitude > 0.5F) {
        set_error(error, "enabled spatiotemporal FLIP plan is invalid");
        return false;
    }
    for (const auto& sample : samples) {
        if (sample.normalizedTime < 0.0F || sample.normalizedTime > 1.0F ||
            !(sample.weight > 0.0F) || !std::isfinite(sample.normalizedTime) ||
            !std::isfinite(sample.weight)) {
            set_error(error, "spatiotemporal FLIP sample is invalid");
            return false;
        }
    }
    return true;
}

SpatiotemporalFlipPlan plan_spatiotemporal_flip_samples(
    std::uint32_t particleCount, std::uint32_t temporalSamples,
    std::uint32_t timeSlabs, float jitterAmplitude, std::uint64_t seed) {
    SpatiotemporalFlipPlan plan;
    if (particleCount == 0U || temporalSamples == 0U || temporalSamples > 16U ||
        timeSlabs == 0U || timeSlabs > 16U || !std::isfinite(jitterAmplitude) ||
        jitterAmplitude < 0.0F || jitterAmplitude > 0.5F) {
        return plan;
    }
    plan.enabled = true;
    plan.temporalSamples = temporalSamples;
    plan.timeSlabs = timeSlabs;
    plan.jitterAmplitude = jitterAmplitude;
    plan.samples.reserve(static_cast<std::size_t>(particleCount) * temporalSamples);
    auto hash01local = [](std::uint64_t value) {
        value ^= value >> 30U; value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U; value *= 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<float>(value & 0xFFFFFFULL) /
               static_cast<float>(0x1000000ULL);
    };
    for (std::uint32_t particle = 0U; particle < particleCount; ++particle) {
        for (std::uint32_t sample = 0U; sample < temporalSamples; ++sample) {
            const float center = (static_cast<float>(sample) + 0.5F) /
                                 static_cast<float>(temporalSamples);
            const float jitter = (hash01local(seed ^
                (static_cast<std::uint64_t>(particle) << 32U) ^ sample) * 2.0F - 1.0F) *
                jitterAmplitude / static_cast<float>(temporalSamples);
            plan.samples.push_back({particle, std::clamp(center + jitter, 0.0F, 1.0F),
                                    1.0F / static_cast<float>(temporalSamples)});
        }
    }
    return plan;
}

} // namespace dve
