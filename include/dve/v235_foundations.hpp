#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "dve/animation.hpp"
#include "dve/navigation_mesh.hpp"
#include "dve/replication_contract.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace dve {

// -----------------------------------------------------------------------------
// Navigation agents and destruction-aware navigation

enum class NavigationAgentStatus : std::uint8_t {
    Idle,
    Following,
    WaitingForOffMeshTraversal,
    ReplanPending,
    Stuck,
    Arrived,
    Failed,
};

struct NavigationAgentConfig {
    float maximumSpeedMetersPerSecond{3.5F};
    float maximumAccelerationMetersPerSecondSquared{14.0F};
    float waypointRadiusMeters{0.20F};
    float arrivalRadiusMeters{0.12F};
    float slowdownRadiusMeters{1.0F};
    float stuckDistanceMeters{0.03F};
    float stuckSeconds{1.0F};
    float automaticReplanSeconds{0.25F};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct NavigationAgentOutput {
    Float3 desiredVelocity{};
    NavigationAgentStatus status{NavigationAgentStatus::Idle};
    std::optional<NavigationPathPoint> offMeshTraversal;
    std::size_t waypointIndex{};
    bool pathChanged{};
};

class NavigationAgent {
public:
    explicit NavigationAgent(NavigationAgentConfig config = {});
    [[nodiscard]] bool set_target(Float3 target, std::string* error = nullptr);
    void clear_target() noexcept;
    void notify_navigation_revision(std::uint64_t revision) noexcept;
    [[nodiscard]] NavigationAgentOutput tick(
        const NavigationMesh& mesh, std::uint64_t meshRevision, Float3 position,
        Float3 currentVelocity, float deltaSeconds,
        const NavigationQueryFilter& filter = {},
        std::span<const NavigationObstacle> obstacles = {});
    [[nodiscard]] bool complete_off_mesh_traversal(Float3 landedPosition) noexcept;
    [[nodiscard]] NavigationAgentStatus status() const noexcept { return status_; }
    [[nodiscard]] const NavigationPath& path() const noexcept { return path_; }
    [[nodiscard]] std::optional<Float3> target() const noexcept { return target_; }
    [[nodiscard]] std::size_t waypoint_index() const noexcept { return waypointIndex_; }
    [[nodiscard]] std::uint64_t replan_count() const noexcept { return replanCount_; }

private:
    [[nodiscard]] bool replan(
        const NavigationMesh& mesh, std::uint64_t meshRevision, Float3 position,
        const NavigationQueryFilter& filter,
        std::span<const NavigationObstacle> obstacles);

    NavigationAgentConfig config_{};
    NavigationAgentStatus status_{NavigationAgentStatus::Idle};
    std::optional<Float3> target_;
    NavigationPath path_{};
    std::size_t waypointIndex_{};
    std::uint64_t plannedRevision_{};
    std::uint64_t observedRevision_{};
    std::uint64_t replanCount_{};
    Float3 lastProgressPosition_{};
    float stuckTimer_{};
    float replanTimer_{};
    bool haveProgressSample_{};
};

struct NavigationDirtyTile {
    std::int32_t x{};
    std::int32_t z{};
    auto operator<=>(const NavigationDirtyTile&) const = default;
};

struct DynamicNavigationTelemetry {
    std::uint64_t revision{};
    std::uint64_t editsObserved{};
    std::uint64_t rebuilds{};
    std::uint64_t rejectedRebuilds{};
    std::size_t dirtyTileCount{};
    std::size_t polygonCount{};
    std::size_t lastRebuiltTileCount{};
    std::size_t lastCandidateTriangleCount{};
    std::size_t lastReusedPolygonCount{};
    std::size_t lastRebuiltPolygonCount{};
};

class DynamicNavigationWorld {
public:
    explicit DynamicNavigationWorld(NavigationBuildSettings settings = {});
    [[nodiscard]] bool set_source_geometry(
        std::vector<NavigationTriangle> triangles,
        std::vector<NavigationOffMeshLink> links = {},
        std::string* error = nullptr);
    void mark_dirty(const NavigationBounds& bounds);
    void mark_voxel_edit(Int3 minimumVoxel, Int3 maximumVoxel, float metersPerVoxel);
    [[nodiscard]] bool replace_region(
        const NavigationBounds& bounds,
        std::span<const NavigationTriangle> replacement,
        std::string* error = nullptr);
    [[nodiscard]] bool rebuild_dirty(std::string* error = nullptr);
    [[nodiscard]] const NavigationMesh& mesh() const noexcept { return mesh_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return telemetry_.revision; }
    [[nodiscard]] std::vector<NavigationDirtyTile> dirty_tiles() const;
    [[nodiscard]] const DynamicNavigationTelemetry& telemetry() const noexcept { return telemetry_; }

private:
    NavigationBuildSettings settings_{};
    NavigationMesh mesh_{};
    std::vector<NavigationTriangle> source_{};
    std::vector<NavigationOffMeshLink> links_{};
    std::set<NavigationDirtyTile> dirty_{};
    DynamicNavigationTelemetry telemetry_{};
};

// -----------------------------------------------------------------------------
// Deterministic package archive

struct DvePakBuildOptions {
    bool stripEditorOnly{true};
    bool incremental{true};
    std::vector<std::string> editorOnlyPrefixes{"editor/", "docs/", "tests/", "artifacts/"};
};

struct DvePakEntry {
    std::string path;
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t contentHash{};
};

struct DvePakManifest {
    std::uint32_t version{1U};
    std::uint64_t packageHash{};
    std::vector<DvePakEntry> entries;
};

[[nodiscard]] bool build_dvepak(
    const std::filesystem::path& root,
    std::span<const std::filesystem::path> inputs,
    const std::filesystem::path& output,
    const DvePakBuildOptions& options = {},
    DvePakManifest* manifest = nullptr,
    std::string* error = nullptr);
[[nodiscard]] std::optional<DvePakManifest> inspect_dvepak(
    const std::filesystem::path& package, std::string* error = nullptr);

class DvePakMount {
public:
    [[nodiscard]] bool mount(const std::filesystem::path& package, std::string* error = nullptr);
    [[nodiscard]] bool contains(std::string_view path) const noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>> read(
        std::string_view path, std::string* error = nullptr) const;
    [[nodiscard]] const DvePakManifest& manifest() const noexcept { return manifest_; }
private:
    std::filesystem::path package_;
    DvePakManifest manifest_{};
};

// -----------------------------------------------------------------------------
// Low-overhead profiler capture

struct ProfileEvent {
    std::string name;
    std::uint64_t threadId{};
    std::uint64_t startNanoseconds{};
    std::uint64_t durationNanoseconds{};
};

struct ProfileFrame {
    std::uint64_t frameIndex{};
    std::vector<ProfileEvent> events;
    std::map<std::string, double, std::less<>> counters;
    std::map<std::string, std::int64_t, std::less<>> memoryBytes;
};

class Profiler {
public:
    static Profiler& instance();
    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    void begin_frame(std::uint64_t frameIndex);
    [[nodiscard]] ProfileFrame end_frame();
    void record(ProfileEvent event);
    void set_counter(std::string name, double value);
    void add_memory(std::string category, std::int64_t deltaBytes);
    [[nodiscard]] std::string frame_json(const ProfileFrame& frame) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Profiler();
    ~Profiler();
};

class ProfileScope {
public:
    explicit ProfileScope(std::string_view name) noexcept;
    ~ProfileScope();
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
private:
    std::string name_;
    std::chrono::steady_clock::time_point start_{};
    bool active_{};
};

// -----------------------------------------------------------------------------
// Asset dependencies and deterministic reimport

struct AssetDependencyNode {
    std::string id;
    std::uint64_t contentHash{};
    std::vector<std::string> dependencies;
    std::uint64_t generation{1U};
};

struct AssetDependencyIssue {
    std::string asset;
    std::string dependency;
    std::string reason;
};

class AssetDependencyGraph {
public:
    [[nodiscard]] bool upsert(AssetDependencyNode node, std::string* error = nullptr);
    [[nodiscard]] bool erase(std::string_view id);
    [[nodiscard]] bool rename(std::string_view oldId, std::string newId, std::string* error = nullptr);
    [[nodiscard]] std::vector<AssetDependencyIssue> validate() const;
    // Returns a dependency-first, lexically deterministic closure for packaging.
    [[nodiscard]] std::vector<std::string> dependency_closure(
        std::span<const std::string> roots, std::string* error = nullptr) const;
    [[nodiscard]] std::vector<std::string> deterministic_reimport_order(
        std::span<const std::string> changed, std::string* error = nullptr) const;
    [[nodiscard]] const AssetDependencyNode* find(std::string_view id) const noexcept;
private:
    std::map<std::string, AssetDependencyNode, std::less<>> nodes_;
};

struct SourceFingerprint {
    std::filesystem::path path;
    std::uint64_t size{};
    std::int64_t modifiedTicks{};
    std::uint64_t contentHash{};
};

class SourceMonitor {
public:
    [[nodiscard]] std::vector<SourceFingerprint> poll(
        std::span<const std::filesystem::path> files, bool hashContents = true);
private:
    std::map<std::filesystem::path, SourceFingerprint> known_;
};

// -----------------------------------------------------------------------------
// Input actions and rebinding

enum class InputTrigger : std::uint8_t { Press, Release, Hold, Tap, DoubleTap };

struct InputCompositePart {
    std::string control;
    float scale{1.0F};
};

struct InputBinding {
    InputBinding() = default;
    InputBinding(std::string actionValue, std::string primaryValue,
                 std::vector<std::string> chordValue = {},
                 InputTrigger triggerValue = InputTrigger::Press,
                 float holdSecondsValue = 0.35F,
                 float doubleTapSecondsValue = 0.25F,
                 float scaleValue = 1.0F,
                 float actuationThresholdValue = 0.5F,
                 std::vector<InputCompositePart> compositeValue = {})
        : action(std::move(actionValue)), primary(std::move(primaryValue)),
          chord(std::move(chordValue)), trigger(triggerValue), holdSeconds(holdSecondsValue),
          doubleTapSeconds(doubleTapSecondsValue), scale(scaleValue),
          actuationThreshold(actuationThresholdValue), composite(std::move(compositeValue)) {}

    std::string action;
    std::string primary;
    std::vector<std::string> chord;
    InputTrigger trigger{InputTrigger::Press};
    float holdSeconds{0.35F};
    float doubleTapSeconds{0.25F};
    float scale{1.0F};
    float actuationThreshold{0.5F};
    // When populated, the weighted parts replace primary. This supports deterministic
    // digital or analog 1D composites such as A/D, S/W, or two controller triggers.
    std::vector<InputCompositePart> composite;
};

struct InputContext {
    std::string name;
    std::int32_t priority{};
    bool enabled{true};
    bool consume{true};
    std::vector<InputBinding> bindings;
};

struct InputActionState {
    float value{};
    bool pressed{};
    bool released{};
    bool held{};
};

class InputActionSystem {
public:
    [[nodiscard]] bool set_context(InputContext context, std::string* error = nullptr);
    [[nodiscard]] bool remove_context(std::string_view name);
    void set_control(std::string control, float value);
    void begin_frame(float deltaSeconds);
    [[nodiscard]] const InputActionState* action(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string> conflicts(const InputBinding& candidate) const;
    [[nodiscard]] bool rebind(
        std::string_view context, std::string_view action, InputBinding replacement,
        bool allowConflict, std::string* error = nullptr);
    [[nodiscard]] bool save_bindings(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] bool load_bindings(const std::filesystem::path& path, std::string* error = nullptr);
private:
    struct BindingHistory {
        bool previousDown{};
        bool currentDown{};
        float heldSeconds{};
        float releasedHeldSeconds{};
        float sinceRelease{1000.0F};
    };
    std::map<std::string, InputContext, std::less<>> contexts_;
    std::map<std::string, float, std::less<>> controls_;
    std::map<std::string, BindingHistory, std::less<>> bindingHistory_;
    std::map<std::string, InputActionState, std::less<>> actions_;
};

// -----------------------------------------------------------------------------
// Save games and migrations

struct SaveGameDocument {
    std::uint32_t schemaVersion{1U};
    std::uint64_t sequence{};
    std::map<std::string, std::vector<std::byte>, std::less<>> sections;
};

using SaveMigration = std::function<bool(SaveGameDocument&, std::string*)>;

class SaveGameStore {
public:
    explicit SaveGameStore(std::uint32_t currentVersion = 1U) : currentVersion_(currentVersion) {}
    [[nodiscard]] bool register_migration(
        std::uint32_t fromVersion, SaveMigration migration, std::string* error = nullptr);
    [[nodiscard]] bool write_atomic(
        const std::filesystem::path& slot, SaveGameDocument document,
        std::string* error = nullptr) const;
    [[nodiscard]] std::optional<SaveGameDocument> read_recover(
        const std::filesystem::path& slot, std::string* error = nullptr) const;
private:
    std::uint32_t currentVersion_{};
    std::map<std::uint32_t, SaveMigration> migrations_;
};

// -----------------------------------------------------------------------------
// Animation extensions

enum class HumanoidBone : std::uint8_t {
    Hips, Spine, Chest, Neck, Head,
    LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand,
    RightShoulder, RightUpperArm, RightLowerArm, RightHand,
    LeftUpperLeg, LeftLowerLeg, LeftFoot,
    RightUpperLeg, RightLowerLeg, RightFoot,
};

struct HumanoidRigMap {
    std::map<HumanoidBone, BoneIndex> bones;
    float referenceHeightMeters{1.8F};
};

[[nodiscard]] AnimationValidationResult validate_humanoid_rig(
    const SkeletonAsset& skeleton, const HumanoidRigMap& rig) noexcept;
[[nodiscard]] LocalPose retarget_humanoid_pose(
    const SkeletonAsset& sourceSkeleton, std::span<const RigidTransform> sourcePose,
    const HumanoidRigMap& sourceRig, const SkeletonAsset& targetSkeleton,
    const HumanoidRigMap& targetRig, std::string* error = nullptr);

struct MorphDelta { std::uint32_t vertex{}; Float3 position{}; Float3 normal{}; };
struct MorphTarget { std::string name; std::vector<MorphDelta> deltas; };
[[nodiscard]] bool apply_morph_targets(
    std::span<const SkinVertex> base, std::span<const MorphTarget> targets,
    const std::map<std::string, float, std::less<>>& weights,
    std::vector<SkinVertex>& output, std::string* error = nullptr);

struct CcdIkRequest {
    std::vector<BoneIndex> chain;
    Float3 targetModel{};
    std::uint32_t iterations{12U};
    float toleranceMeters{0.002F};
    float weight{1.0F};
};
[[nodiscard]] bool solve_ccd_ik(
    const SkeletonAsset& skeleton, LocalPose& pose, const CcdIkRequest& request,
    std::string* error = nullptr);

// -----------------------------------------------------------------------------
// Neutral physics query/material foundation

struct PhysicsMaterial {
    std::string name;
    float friction{0.5F};
    float restitution{};
    float densityKilogramsPerCubicMeter{1000.0F};
};

class PhysicsMaterialLibrary {
public:
    [[nodiscard]] bool set(std::uint16_t id, PhysicsMaterial material, std::string* error = nullptr);
    [[nodiscard]] const PhysicsMaterial* get(std::uint16_t id) const noexcept;
private:
    std::map<std::uint16_t, PhysicsMaterial> materials_;
};

struct PhysicsQueryHit {
    RigidBodyHandle body{kInvalidRigidBodyHandle};
    Float3 point{};
    Float3 normal{};
    float fraction{1.0F};
    float distance{};
    std::uint16_t material{};
    std::uint32_t subShape{};
};

struct PhysicsQueryFilter {
    std::set<RigidBodyHandle> ignoredBodies;
    std::set<std::uint16_t> acceptedMaterials;
};

[[nodiscard]] std::vector<PhysicsQueryHit> filter_and_sort_physics_hits(
    std::span<const PhysicsQueryHit> hits, const PhysicsQueryFilter& filter = {});

// -----------------------------------------------------------------------------
// Artificial intelligence runtime

using BlackboardValue = std::variant<bool, std::int64_t, double, std::string, Float3>;

class Blackboard {
public:
    void set(std::string key, BlackboardValue value) { values_[std::move(key)] = std::move(value); }
    [[nodiscard]] const BlackboardValue* get(std::string_view key) const noexcept;
    [[nodiscard]] bool erase(std::string_view key);
private:
    std::map<std::string, BlackboardValue, std::less<>> values_;
};

enum class BehaviorStatus : std::uint8_t { Success, Failure, Running };
using BehaviorAction = std::function<BehaviorStatus(Blackboard&, float)>;
using BehaviorCondition = std::function<bool(const Blackboard&)>;

class BehaviorTree {
public:
    using NodeId = std::uint32_t;
    enum class Kind : std::uint8_t { Sequence, Selector, Condition, Action };
    struct Node {
        Kind kind{Kind::Action};
        std::vector<NodeId> children;
        BehaviorCondition condition;
        BehaviorAction action;
    };
    [[nodiscard]] NodeId add(Node node);
    void set_root(NodeId root) noexcept { root_ = root; }
    [[nodiscard]] BehaviorStatus tick(Blackboard& blackboard, float deltaSeconds);
    void reset() noexcept;
private:
    [[nodiscard]] BehaviorStatus tick_node(NodeId id, Blackboard& blackboard, float deltaSeconds);
    std::vector<Node> nodes_;
    std::map<NodeId, std::size_t> runningChild_;
    NodeId root_{0U};
};

struct SteeringRequest {
    Float3 position{};
    Float3 velocity{};
    Float3 target{};
    float maximumSpeed{3.5F};
    float maximumAcceleration{12.0F};
    float slowdownRadius{1.0F};
};
[[nodiscard]] Float3 steering_arrive(const SteeringRequest& request) noexcept;

struct PerceptionCandidate { std::uint64_t id{}; Float3 position{}; };
struct PerceptionHit { std::uint64_t id{}; Float3 position{}; float distance{}; float score{}; };
[[nodiscard]] std::vector<PerceptionHit> query_perception(
    Float3 origin, Float3 forward, float radius, float halfAngleRadians,
    std::span<const PerceptionCandidate> candidates,
    const std::function<bool(Float3, Float3)>& visible = {});

// -----------------------------------------------------------------------------
// Networking extensions

struct NetworkTransformSample {
    std::uint64_t tick{};
    Float3 position{};
    Float3 velocity{};
};

class NetworkTransformInterpolator {
public:
    explicit NetworkTransformInterpolator(std::size_t capacity = 32U) : capacity_(capacity) {}
    [[nodiscard]] bool push(NetworkTransformSample sample) noexcept;
    [[nodiscard]] std::optional<NetworkTransformSample> sample(double tick) const noexcept;
private:
    std::size_t capacity_{};
    std::vector<NetworkTransformSample> samples_;
};

class ReplicationRelevancyGrid {
public:
    explicit ReplicationRelevancyGrid(float cellSizeMeters = 32.0F) : cellSize_(cellSizeMeters) {}
    void upsert(NetworkObjectId id, Float3 position);
    void erase(NetworkObjectId id);
    [[nodiscard]] std::vector<NetworkObjectId> query(Float3 center, float radius) const;
private:
    float cellSize_{};
    std::map<NetworkObjectId, Float3> objects_;
};

template<class State, class Input>
class RollbackBuffer {
public:
    struct Frame { std::uint64_t tick{}; State state{}; Input input{}; };
    explicit RollbackBuffer(std::size_t capacity = 256U) : capacity_(capacity) {}
    void push(Frame frame) {
        auto it = std::lower_bound(frames_.begin(), frames_.end(), frame.tick,
            [](const Frame& item, std::uint64_t tick) { return item.tick < tick; });
        if (it != frames_.end() && it->tick == frame.tick) *it = std::move(frame);
        else frames_.insert(it, std::move(frame));
        if (frames_.size() > capacity_) frames_.erase(frames_.begin(), frames_.begin() +
            static_cast<std::ptrdiff_t>(frames_.size() - capacity_));
    }
    [[nodiscard]] const Frame* find(std::uint64_t tick) const noexcept {
        auto it = std::lower_bound(frames_.begin(), frames_.end(), tick,
            [](const Frame& item, std::uint64_t value) { return item.tick < value; });
        return it != frames_.end() && it->tick == tick ? &*it : nullptr;
    }
    [[nodiscard]] std::vector<Input> inputs_after(std::uint64_t tick) const {
        std::vector<Input> out;
        for (const auto& frame : frames_) if (frame.tick > tick) out.push_back(frame.input);
        return out;
    }
private:
    std::size_t capacity_{};
    std::vector<Frame> frames_;
};

// -----------------------------------------------------------------------------
// Editor transactions and plugin registration

struct EditorOperation {
    std::string label;
    std::function<bool(std::string*)> apply;
    std::function<bool(std::string*)> revert;
};

class EditorOperationHistory {
public:
    [[nodiscard]] bool execute(EditorOperation operation, std::string* error = nullptr);
    [[nodiscard]] bool undo(std::string* error = nullptr);
    [[nodiscard]] bool redo(std::string* error = nullptr);
    [[nodiscard]] std::size_t undo_count() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_count() const noexcept { return redo_.size(); }
private:
    std::vector<EditorOperation> undo_;
    std::vector<EditorOperation> redo_;
};

struct PluginDescriptor {
    std::string owner;
    std::string id;
    std::string displayName;
    std::string kind;
};

class PluginRegistry {
public:
    [[nodiscard]] bool register_extension(PluginDescriptor descriptor, std::string* error = nullptr);
    [[nodiscard]] std::size_t unregister_owner(std::string_view owner);
    [[nodiscard]] const PluginDescriptor* find(std::string_view kind, std::string_view id) const noexcept;
    [[nodiscard]] std::vector<PluginDescriptor> list(std::string_view kind = {}) const;
private:
    std::map<std::pair<std::string, std::string>, PluginDescriptor> extensions_;
};

} // namespace dve
