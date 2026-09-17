#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dve/transform.hpp"

namespace dve {

enum class VfxModulePhase : std::uint8_t { Spawn, Update, Render };
enum class VfxModuleKind : std::uint8_t {
    SpawnRate,
    SpawnBurst,
    InitializeBox,
    InitializeSphere,
    VelocityCone,
    LifetimeRange,
    Gravity,
    Drag,
    CurlNoise,
    GroundCollision,
    ColorOverLife,
    SizeOverLife,
    EmissionOverLife,
    KillOutsideBounds,
    AttractorPoint,
    VelocityLimit,
    SphereCollision,
    BillboardRenderer,
    MeshRenderer,
    RibbonRenderer,
};

struct VfxModule {
    std::uint32_t id{};
    VfxModuleKind kind{VfxModuleKind::SpawnRate};
    VfxModulePhase phase{VfxModulePhase::Spawn};
    bool enabled{true};
    std::array<float, 8> parameters{};
    std::uint64_t resourceId{};
};

struct VfxParticleGraphAsset {
    std::string name{"Particle Effect"};
    std::uint32_t maximumParticles{65'536U};
    float durationSeconds{5.0F};
    bool looping{true};
    std::uint64_t seed{1U};
    std::vector<VfxModule> modules;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

enum class VfxOpcode : std::uint8_t {
    SpawnRate,
    SpawnBurst,
    InitializeBox,
    InitializeSphere,
    VelocityCone,
    LifetimeRange,
    Gravity,
    Drag,
    CurlNoise,
    GroundCollision,
    ColorOverLife,
    SizeOverLife,
    EmissionOverLife,
    KillOutsideBounds,
    AttractorPoint,
    VelocityLimit,
    SphereCollision,
    BillboardRenderer,
    MeshRenderer,
    RibbonRenderer,
};

struct VfxInstruction {
    VfxOpcode opcode{VfxOpcode::SpawnRate};
    std::array<float, 8> operands{};
    std::uint64_t resourceId{};
};

struct VfxProgram {
    std::uint64_t sourceHash{};
    std::uint32_t maximumParticles{};
    float durationSeconds{};
    bool looping{};
    std::uint64_t seed{};
    std::vector<VfxInstruction> spawn;
    std::vector<VfxInstruction> update;
    std::vector<VfxInstruction> render;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct VfxCompileResult {
    std::optional<VfxProgram> program;
    std::vector<std::string> warnings;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return program.has_value(); }
};

[[nodiscard]] VfxCompileResult compile_vfx_particle_graph(const VfxParticleGraphAsset& asset);

struct VfxParticleState {
    Float3 position{};
    float age{};
    Float3 velocity{};
    float lifetime{1.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    float size{1.0F};
    float emission{};
    std::uint32_t generation{};
    bool alive{};
};


enum class VfxEventKind : std::uint8_t { Spawn, Death, Collision };

struct VfxEvent {
    VfxEventKind kind{VfxEventKind::Spawn};
    Float3 position{};
    Float3 velocity{};
    std::uint32_t generation{};
};

struct VfxRuntimeTelemetry {
    std::uint64_t spawned{};
    std::uint64_t killed{};
    std::uint64_t updated{};
    std::uint64_t rejectedSpawns{};
    std::uint64_t collisions{};
    std::uint64_t eventsWritten{};
    std::uint64_t eventOverflow{};
    std::uint32_t alive{};
};

class VfxCpuRuntime {
public:
    explicit VfxCpuRuntime(VfxProgram program);
    [[nodiscard]] VfxRuntimeTelemetry step(float deltaSeconds);
    void reset() noexcept;
    [[nodiscard]] const std::vector<VfxParticleState>& particles() const noexcept {
        return particles_;
    }
    [[nodiscard]] float effect_time_seconds() const noexcept { return effectTimeSeconds_; }
    [[nodiscard]] const std::vector<VfxEvent>& events() const noexcept { return events_; }
    void set_event_capacity(std::uint32_t capacity) { eventCapacity_ = capacity; }

private:
    VfxProgram program_;
    std::vector<VfxParticleState> particles_;
    float effectTimeSeconds_{};
    float spawnAccumulator_{};
    std::uint64_t randomState_{};
    bool burstFired_{};
    std::vector<VfxEvent> events_;
    std::uint32_t eventCapacity_{4096U};
};

enum class VfxGpuPassKind : std::uint8_t {
    ResetCounters,
    Spawn,
    Update,
    BuildEvents,
    Compact,
    Sort,
    BuildIndirectDraw,
};

struct VfxGpuDispatch {
    VfxGpuPassKind pass{VfxGpuPassKind::Update};
    std::uint32_t groupsX{1U};
    std::uint32_t groupsY{1U};
    std::uint32_t groupsZ{1U};
};

struct VfxGpuFramePlan {
    bool enabled{};
    bool requiresSorting{};
    std::uint32_t maximumParticles{};
    std::uint64_t estimatedBytes{};
    std::vector<VfxGpuDispatch> dispatches;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] VfxGpuFramePlan plan_vfx_gpu_frame(const VfxProgram& program,
                                                  bool transparentSorting);
[[nodiscard]] std::string vfx_module_kind_name(VfxModuleKind kind);

} // namespace dve
