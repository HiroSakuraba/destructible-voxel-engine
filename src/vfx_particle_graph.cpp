#include "dve/vfx_particle_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <utility>

namespace dve {
namespace {

constexpr float kPi = 3.14159265358979323846F;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= prime;
    }
}

template <class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(T));
}

[[nodiscard]] VfxOpcode opcode(VfxModuleKind kind) noexcept {
    return static_cast<VfxOpcode>(kind);
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] float random01(std::uint64_t& state) noexcept {
    const std::uint32_t bits = static_cast<std::uint32_t>(next_random(state) >> 40U);
    return static_cast<float>(bits) / static_cast<float>(0xFFFFFFU);
}

[[nodiscard]] float random_range(std::uint64_t& state, float minimum, float maximum) noexcept {
    return minimum + (maximum - minimum) * random01(state);
}

[[nodiscard]] Float3 curl_noise(Float3 position, float time, float frequency) noexcept {
    const float x = std::sin((position.y + time) * frequency) -
                    std::cos((position.z - time) * frequency);
    const float y = std::sin((position.z + time * 0.7F) * frequency) -
                    std::cos((position.x - time) * frequency);
    const float z = std::sin((position.x + time * 1.3F) * frequency) -
                    std::cos((position.y - time) * frequency);
    return {x, y, z};
}

[[nodiscard]] std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) noexcept {
    return (value + divisor - 1U) / divisor;
}

} // namespace

bool VfxParticleGraphAsset::validate(std::string* error) const {
    if (maximumParticles == 0U || maximumParticles > 16'777'216U) {
        set_error(error, "VFX maximum particle count must be in [1, 16777216]");
        return false;
    }
    if (!(durationSeconds > 0.0F) || !std::isfinite(durationSeconds)) {
        set_error(error, "VFX duration must be finite and positive");
        return false;
    }
    std::set<std::uint32_t> ids;
    std::uint32_t rendererCount{};
    for (const auto& module : modules) {
        if (module.id == 0U || !ids.insert(module.id).second) {
            set_error(error, "VFX module identifiers must be nonzero and unique");
            return false;
        }
        for (const float value : module.parameters) {
            if (!std::isfinite(value)) {
                set_error(error, "VFX module contains a non-finite parameter");
                return false;
            }
        }
        const bool renderer = module.kind == VfxModuleKind::BillboardRenderer ||
                              module.kind == VfxModuleKind::MeshRenderer ||
                              module.kind == VfxModuleKind::RibbonRenderer;
        if (renderer && module.enabled) ++rendererCount;
        if (renderer && module.phase != VfxModulePhase::Render) {
            set_error(error, "VFX renderer modules must be in the render phase");
            return false;
        }
    }
    if (rendererCount > 1U) {
        set_error(error, "VFX graph currently supports one active renderer module");
        return false;
    }
    return true;
}

void VfxParticleGraphAsset::recompute_hash() noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_bytes(hash, name.data(), name.size());
    hash_value(hash, maximumParticles);
    hash_value(hash, durationSeconds);
    hash_value(hash, looping);
    hash_value(hash, seed);
    for (const auto& module : modules) hash_value(hash, module);
    contentHash = hash == 0U ? 1U : hash;
}

bool VfxProgram::validate(std::string* error) const {
    if (sourceHash == 0U || maximumParticles == 0U || !(durationSeconds > 0.0F)) {
        set_error(error, "VFX program header is invalid");
        return false;
    }
    if (spawn.empty() && update.empty() && render.empty()) {
        set_error(error, "VFX program has no instructions");
        return false;
    }
    return true;
}

VfxCompileResult compile_vfx_particle_graph(const VfxParticleGraphAsset& inputAsset) {
    VfxCompileResult result;
    VfxParticleGraphAsset asset = inputAsset;
    asset.recompute_hash();
    if (!asset.validate(&result.error)) return result;
    VfxProgram program;
    program.sourceHash = asset.contentHash;
    program.maximumParticles = asset.maximumParticles;
    program.durationSeconds = asset.durationSeconds;
    program.looping = asset.looping;
    program.seed = asset.seed;
    std::vector<VfxModule> ordered = asset.modules;
    std::stable_sort(ordered.begin(), ordered.end(), [](const VfxModule& a, const VfxModule& b) {
        if (a.phase != b.phase) return a.phase < b.phase;
        return a.id < b.id;
    });
    bool hasLifetime{};
    bool hasRenderer{};
    for (const auto& module : ordered) {
        if (!module.enabled) continue;
        VfxInstruction instruction{opcode(module.kind), module.parameters, module.resourceId};
        switch (module.phase) {
        case VfxModulePhase::Spawn: program.spawn.push_back(instruction); break;
        case VfxModulePhase::Update: program.update.push_back(instruction); break;
        case VfxModulePhase::Render: program.render.push_back(instruction); break;
        }
        hasLifetime = hasLifetime || module.kind == VfxModuleKind::LifetimeRange;
        hasRenderer = hasRenderer || module.kind == VfxModuleKind::BillboardRenderer ||
                      module.kind == VfxModuleKind::MeshRenderer ||
                      module.kind == VfxModuleKind::RibbonRenderer;
    }
    if (!hasLifetime) result.warnings.emplace_back(
        "no lifetime module: particles use the runtime default of one second");
    if (!hasRenderer) result.warnings.emplace_back(
        "no renderer module: simulation runs but produces no draw packet");
    if (!program.validate(&result.error)) return result;
    result.program = std::move(program);
    return result;
}

VfxCpuRuntime::VfxCpuRuntime(VfxProgram program)
    : program_(std::move(program)), particles_(program_.maximumParticles),
      randomState_(program_.seed == 0U ? 1U : program_.seed) {}

void VfxCpuRuntime::reset() noexcept {
    for (auto& particle : particles_) particle = {};
    effectTimeSeconds_ = 0.0F;
    spawnAccumulator_ = 0.0F;
    randomState_ = program_.seed == 0U ? 1U : program_.seed;
    burstFired_ = false;
}

VfxRuntimeTelemetry VfxCpuRuntime::step(float deltaSeconds) {
    VfxRuntimeTelemetry telemetry;
    events_.clear();
    auto emitEvent = [&](VfxEventKind kind, const VfxParticleState& particle) {
        if (events_.size() < eventCapacity_) {
            events_.push_back({kind, particle.position, particle.velocity, particle.generation});
            ++telemetry.eventsWritten;
        } else {
            ++telemetry.eventOverflow;
        }
    };
    if (!(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds)) return telemetry;
    const float dt = std::min(deltaSeconds, 0.1F);
    effectTimeSeconds_ += dt;
    if (effectTimeSeconds_ > program_.durationSeconds) {
        if (program_.looping) {
            effectTimeSeconds_ = std::fmod(effectTimeSeconds_, program_.durationSeconds);
            burstFired_ = false;
        } else {
            effectTimeSeconds_ = program_.durationSeconds;
        }
    }
    std::uint32_t requestedSpawns{};
    for (const auto& instruction : program_.spawn) {
        if (instruction.opcode == VfxOpcode::SpawnRate) {
            spawnAccumulator_ += std::max(0.0F, instruction.operands[0]) * dt;
            const float whole = std::floor(spawnAccumulator_);
            requestedSpawns += static_cast<std::uint32_t>(whole);
            spawnAccumulator_ -= whole;
        } else if (instruction.opcode == VfxOpcode::SpawnBurst && !burstFired_) {
            requestedSpawns += static_cast<std::uint32_t>(std::max(0.0F,
                                                                  instruction.operands[0]));
            burstFired_ = true;
        }
    }
    for (auto& particle : particles_) {
        if (requestedSpawns == 0U) break;
        if (particle.alive) continue;
        particle = {};
        particle.alive = true;
        particle.lifetime = 1.0F;
        particle.color = {1.0F, 1.0F, 1.0F, 1.0F};
        particle.size = 1.0F;
        ++particle.generation;
        for (const auto& instruction : program_.spawn) {
            switch (instruction.opcode) {
            case VfxOpcode::InitializeBox:
                particle.position = {
                    random_range(randomState_, instruction.operands[0], instruction.operands[1]),
                    random_range(randomState_, instruction.operands[2], instruction.operands[3]),
                    random_range(randomState_, instruction.operands[4], instruction.operands[5])};
                break;
            case VfxOpcode::InitializeSphere: {
                const float azimuth = random01(randomState_) * 2.0F * kPi;
                const float z = random01(randomState_) * 2.0F - 1.0F;
                const float radial = std::sqrt(std::max(0.0F, 1.0F - z * z));
                const float radius = std::cbrt(random01(randomState_)) *
                                     std::max(0.0F, instruction.operands[0]);
                particle.position = {radius * radial * std::cos(azimuth),
                                     radius * radial * std::sin(azimuth), radius * z};
                break;
            }
            case VfxOpcode::VelocityCone: {
                const float speed = random_range(randomState_, instruction.operands[0],
                                                  instruction.operands[1]);
                const float angle = random01(randomState_) * 2.0F * kPi;
                const float cone = std::clamp(instruction.operands[2], 0.0F, kPi);
                const float cosTheta = random_range(randomState_, std::cos(cone), 1.0F);
                const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
                particle.velocity = multiply({sinTheta * std::cos(angle), cosTheta,
                                              sinTheta * std::sin(angle)}, speed);
                break;
            }
            case VfxOpcode::LifetimeRange:
                particle.lifetime = std::max(0.001F, random_range(
                    randomState_, instruction.operands[0], instruction.operands[1]));
                break;
            default: break;
            }
        }
        --requestedSpawns;
        ++telemetry.spawned;
        emitEvent(VfxEventKind::Spawn, particle);
    }
    telemetry.rejectedSpawns = requestedSpawns;

    for (auto& particle : particles_) {
        if (!particle.alive) continue;
        particle.age += dt;
        if (particle.age >= particle.lifetime) {
            emitEvent(VfxEventKind::Death, particle);
            particle.alive = false;
            ++telemetry.killed;
            continue;
        }
        const float normalizedAge = std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
        for (const auto& instruction : program_.update) {
            switch (instruction.opcode) {
            case VfxOpcode::Gravity:
                particle.velocity = add(particle.velocity,
                                        multiply({instruction.operands[0], instruction.operands[1],
                                                  instruction.operands[2]}, dt));
                break;
            case VfxOpcode::Drag:
                particle.velocity = multiply(particle.velocity,
                    std::exp(-std::max(0.0F, instruction.operands[0]) * dt));
                break;
            case VfxOpcode::CurlNoise:
                particle.velocity = add(particle.velocity,
                    multiply(curl_noise(particle.position, effectTimeSeconds_,
                                        std::max(0.001F, instruction.operands[0])),
                             instruction.operands[1] * dt));
                break;
            case VfxOpcode::GroundCollision:
                if (particle.position.y < instruction.operands[0] && particle.velocity.y < 0.0F) {
                    particle.position.y = instruction.operands[0];
                    particle.velocity.y = -particle.velocity.y *
                                          std::clamp(instruction.operands[1], 0.0F, 1.0F);
                    particle.velocity.x *= std::clamp(1.0F - instruction.operands[2], 0.0F, 1.0F);
                    particle.velocity.z *= std::clamp(1.0F - instruction.operands[2], 0.0F, 1.0F);
                    ++telemetry.collisions;
                    emitEvent(VfxEventKind::Collision, particle);
                }
                break;
            case VfxOpcode::ColorOverLife:
                for (std::size_t channel = 0U; channel < 4U; ++channel) {
                    particle.color[channel] = instruction.operands[channel] +
                        (instruction.operands[channel + 4U] - instruction.operands[channel]) *
                        normalizedAge;
                }
                break;
            case VfxOpcode::SizeOverLife:
                particle.size = instruction.operands[0] +
                    (instruction.operands[1] - instruction.operands[0]) * normalizedAge;
                break;
            case VfxOpcode::EmissionOverLife:
                particle.emission = instruction.operands[0] +
                    (instruction.operands[1] - instruction.operands[0]) * normalizedAge;
                break;
            case VfxOpcode::KillOutsideBounds:
                if (particle.position.x < instruction.operands[0] ||
                    particle.position.x > instruction.operands[1] ||
                    particle.position.y < instruction.operands[2] ||
                    particle.position.y > instruction.operands[3] ||
                    particle.position.z < instruction.operands[4] ||
                    particle.position.z > instruction.operands[5]) {
                    emitEvent(VfxEventKind::Death, particle);
                    particle.alive = false;
                    ++telemetry.killed;
                }
                break;
            case VfxOpcode::AttractorPoint: {
                const Float3 target{instruction.operands[0], instruction.operands[1],
                                    instruction.operands[2]};
                const Float3 delta = subtract(target, particle.position);
                const float distanceSquared = std::max(length_squared(delta), 1.0e-6F);
                const float maximumAcceleration = std::max(0.0F, instruction.operands[4]);
                const float acceleration = std::min(maximumAcceleration,
                    std::max(0.0F, instruction.operands[3]) / distanceSquared);
                particle.velocity = add(particle.velocity,
                    multiply(delta, acceleration * dt / std::sqrt(distanceSquared)));
                break;
            }
            case VfxOpcode::VelocityLimit: {
                const float maximumSpeed = std::max(0.0F, instruction.operands[0]);
                const float speed = length(particle.velocity);
                if (maximumSpeed > 0.0F && speed > maximumSpeed)
                    particle.velocity = multiply(particle.velocity, maximumSpeed / speed);
                break;
            }
            case VfxOpcode::SphereCollision: {
                const Float3 center{instruction.operands[0], instruction.operands[1],
                                    instruction.operands[2]};
                const float radius = std::max(0.0F, instruction.operands[3]);
                const Float3 offset = subtract(particle.position, center);
                const float distance = length(offset);
                if (distance < radius && distance > 1.0e-6F) {
                    const Float3 normal = multiply(offset, 1.0F / distance);
                    particle.position = add(center, multiply(normal, radius));
                    const float normalVelocity = dot(particle.velocity, normal);
                    if (normalVelocity < 0.0F) {
                        particle.velocity = subtract(particle.velocity,
                            multiply(normal, (1.0F + std::clamp(instruction.operands[4],
                                                               0.0F, 1.0F)) *
                                             normalVelocity));
                    }
                    ++telemetry.collisions;
                    emitEvent(VfxEventKind::Collision, particle);
                }
                break;
            }
            default: break;
            }
            if (!particle.alive) break;
        }
        if (!particle.alive) continue;
        particle.position = add(particle.position, multiply(particle.velocity, dt));
        ++telemetry.updated;
    }
    telemetry.alive = static_cast<std::uint32_t>(std::count_if(
        particles_.begin(), particles_.end(), [](const VfxParticleState& p) { return p.alive; }));
    return telemetry;
}

bool VfxGpuFramePlan::validate(std::string* error) const {
    if (!enabled) return true;
    if (maximumParticles == 0U || dispatches.size() < 5U || estimatedBytes == 0U) {
        set_error(error, "enabled VFX GPU plan is incomplete");
        return false;
    }
    if (dispatches.front().pass != VfxGpuPassKind::ResetCounters ||
        dispatches.back().pass != VfxGpuPassKind::BuildIndirectDraw) {
        set_error(error, "VFX GPU pass ordering is invalid");
        return false;
    }
    return true;
}

VfxGpuFramePlan plan_vfx_gpu_frame(const VfxProgram& program, bool transparentSorting) {
    VfxGpuFramePlan plan;
    plan.enabled = program.validate(nullptr);
    plan.requiresSorting = transparentSorting;
    plan.maximumParticles = program.maximumParticles;
    if (!plan.enabled) return plan;
    constexpr std::uint64_t particleStride = 64U;
    plan.estimatedBytes = static_cast<std::uint64_t>(program.maximumParticles) *
                          (particleStride * 2U + sizeof(std::uint32_t) * 4U) + 4096U;
    const std::uint32_t groups = ceil_div(program.maximumParticles, 64U);
    plan.dispatches.push_back({VfxGpuPassKind::ResetCounters, 1U, 1U, 1U});
    plan.dispatches.push_back({VfxGpuPassKind::Spawn, groups, 1U, 1U});
    plan.dispatches.push_back({VfxGpuPassKind::Update, groups, 1U, 1U});
    plan.dispatches.push_back({VfxGpuPassKind::BuildEvents, groups, 1U, 1U});
    plan.dispatches.push_back({VfxGpuPassKind::Compact, groups, 1U, 1U});
    if (transparentSorting)
        plan.dispatches.push_back({VfxGpuPassKind::Sort, groups, 1U, 1U});
    plan.dispatches.push_back({VfxGpuPassKind::BuildIndirectDraw, 1U, 1U, 1U});
    return plan;
}

std::string vfx_module_kind_name(VfxModuleKind kind) {
    switch (kind) {
    case VfxModuleKind::SpawnRate: return "Spawn Rate";
    case VfxModuleKind::SpawnBurst: return "Spawn Burst";
    case VfxModuleKind::InitializeBox: return "Initialize Box";
    case VfxModuleKind::InitializeSphere: return "Initialize Sphere";
    case VfxModuleKind::VelocityCone: return "Velocity Cone";
    case VfxModuleKind::LifetimeRange: return "Lifetime Range";
    case VfxModuleKind::Gravity: return "Gravity";
    case VfxModuleKind::Drag: return "Drag";
    case VfxModuleKind::CurlNoise: return "Curl Noise";
    case VfxModuleKind::GroundCollision: return "Ground Collision";
    case VfxModuleKind::ColorOverLife: return "Color Over Life";
    case VfxModuleKind::SizeOverLife: return "Size Over Life";
    case VfxModuleKind::EmissionOverLife: return "Emission Over Life";
    case VfxModuleKind::KillOutsideBounds: return "Kill Outside Bounds";
    case VfxModuleKind::AttractorPoint: return "Attractor Point";
    case VfxModuleKind::VelocityLimit: return "Velocity Limit";
    case VfxModuleKind::SphereCollision: return "Sphere Collision";
    case VfxModuleKind::BillboardRenderer: return "Billboard Renderer";
    case VfxModuleKind::MeshRenderer: return "Mesh Renderer";
    case VfxModuleKind::RibbonRenderer: return "Ribbon Renderer";
    }
    return "Unknown";
}

} // namespace dve
