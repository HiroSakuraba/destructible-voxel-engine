#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "dve/connectivity.hpp"
#include "dve/dvox.hpp"
#include "dve/job_system.hpp"
#include "dve/packed_brickmap.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/runtime_brickmap_world.hpp"

namespace dve {

using RuntimeSceneHandle = std::uint32_t;
constexpr RuntimeSceneHandle kInvalidRuntimeSceneHandle = 0xFFFFFFFFU;

enum class RuntimeSceneErrorCode : std::uint8_t {
    NoError,
    Io,
    Json,
    InvalidManifest,
    UnsupportedVersion,
    LimitExceeded,
    DuplicateObjectId,
    DuplicateObjectIndex,
    InvalidParent,
    CyclicHierarchy,
    InvalidTransform,
    PathEscape,
    MissingAsset,
    DuplicateAssetPath,
    DvoxReadFailed,
    ObjectIdMismatch,
    InvalidMaterialTable,
    InvalidMaterialReference,
    VoxelSizeMismatch,
    DerivedDataFailure,
    RendererPublicationFailure,
    PhysicsPublicationFailure,
    Cancelled,
    StagingMemoryExceeded,
    WrongThread,
    InvalidStaging,
    SavedStateIo,
    SavedStateInvalid,
    AssetIncompatible,
    StateRestoreFailure,
    HotReloadIncompatible,
    HotReloadRollbackFailure,
    ObjectIdAlreadyReserved,
    UnknownRequestedObject,
    InvalidHandle,
    QueueFull,
    DeadlineExceeded,
    PublicationQueueFull,
};

struct RuntimeSceneError {
    RuntimeSceneErrorCode code{RuntimeSceneErrorCode::NoError};
    std::string message;
    std::filesystem::path path;
    std::optional<std::uint64_t> objectId;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != RuntimeSceneErrorCode::NoError;
    }
};

struct RuntimeSceneLoadOptions {
    // By default every object is loaded and validated before the scene becomes visible.
    // Set false and provide initiallyLoadedObjectIds to establish a streamable scene whose
    // remaining stable IDs are reserved but whose payloads are deferred.
    bool loadAllObjects{true};
    std::vector<std::uint64_t> initiallyLoadedObjectIds;

    bool requireUniformVoxelSize{false};
    float expectedVoxelSizeMeters{}; // zero means no external policy
    float voxelSizeTolerance{1.0e-6F};

    bool createDynamicBodies{true};
    bool createStaticBodies{true};
    bool rejectProxyOverBudget{false};
    std::size_t maximumProxyBoxes{256};

    std::size_t maximumObjects{4096};
    std::uint64_t maximumManifestBytes{16ULL * 1024ULL * 1024ULL};
    std::uint64_t maximumDvoxBytesPerObject{2ULL * 1024ULL * 1024ULL * 1024ULL};
};

enum class RuntimeSceneLoadPhase : std::uint8_t {
    Manifest,
    AssetIo,
    DerivedData,
    RendererPublication,
    PhysicsPublication,
    Commit,
    Complete,
};

struct RuntimeSceneProgress {
    RuntimeSceneLoadPhase phase{RuntimeSceneLoadPhase::Manifest};
    std::size_t completed{};
    std::size_t total{};
    std::optional<std::uint64_t> objectId;
};

class RuntimeSceneCancellationToken {
public:
    RuntimeSceneCancellationToken()
        : state_(std::make_shared<std::atomic_bool>(false)) {}

    void request_cancel() const noexcept { state_->store(true, std::memory_order_release); }
    [[nodiscard]] bool cancellation_requested() const noexcept {
        return state_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

using RuntimeSceneProgressCallback = std::function<void(const RuntimeSceneProgress&)>;

enum class RuntimeSceneJobPriority : std::uint8_t {
    Critical = 0,
    High = 1,
    Normal = 2,
    Background = 3,
};

struct RuntimeSceneSubmissionOptions {
    RuntimeSceneJobPriority priority{RuntimeSceneJobPriority::Normal};
    std::optional<std::chrono::steady_clock::time_point> deadline{};
};

struct RuntimeSceneStagingExecutorConfig {
    std::size_t workerCount{};
    std::size_t maximumRetainedStagingBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximumPendingJobs{128U};
    // Zero selects maximumRetainedStagingBytes. The coordinator stops starting new work while
    // completed staging is retained above this threshold, applying publication backpressure.
    std::size_t publicationBackpressureBytes{};
};

struct RuntimeSceneStageOptions {
    // Number of worker threads created for a standalone staging job. Zero selects the engine
    // default. RuntimeSceneStagingExecutor ignores this field and reuses its fixed worker budget.
    // The coordinator thread participates in parallel work in both modes.
    std::size_t workerCount{};
    std::size_t maximumStagingBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
    RuntimeSceneCancellationToken cancellation{};
    RuntimeSceneProgressCallback progress{};
};

struct RuntimeSceneCheckpointOptions {
    RuntimeSceneLoadOptions loadOptions{};
    RuntimeSceneStageOptions stageOptions{};
    bool restoreBodyStates{true};
    std::uint64_t maximumStateBytes{64ULL * 1024ULL * 1024ULL};
};

struct RuntimeSceneHotReloadOptions {
    RuntimeSceneStageOptions stageOptions{};
    bool allowVoxelTopologyChanges{false};
    bool preserveDynamicBodyState{true};
};

struct RuntimeSceneHotReloadResult {
    RuntimeSceneError error{};
    std::vector<std::uint64_t> changedObjectIds{};

    [[nodiscard]] explicit operator bool() const noexcept { return !error; }
};

struct RuntimeSceneObjectMetadata {
    std::size_t index{};
    std::uint64_t id{};
    std::string name;
    std::string nodePath;
    std::filesystem::path relativeFile;
    std::optional<std::size_t> parentIndex;
    std::optional<std::uint64_t> parentId;
    bool anchored{};
    bool structural{true};
    bool generateCollision{true};
    RigidTransform worldTransform{};
};

struct RuntimeSceneObjectStats {
    std::uint64_t voxels{};
    std::size_t bricks{};
    std::size_t materialCount{};
    std::size_t connectivityComponents{};
    std::size_t collisionBoxes{};
    bool collisionProxyOverBudget{};
    std::uint64_t authorityHash{};
    std::uint64_t assetContentHash{};
    std::uint64_t packedBrickmapHash{};
    std::uint64_t rendererReadbackHash{};
};

class RuntimeSceneObject {
public:
    RuntimeSceneObject() = default;
    RuntimeSceneObject(const RuntimeSceneObject&) = delete;
    RuntimeSceneObject& operator=(const RuntimeSceneObject&) = delete;
    RuntimeSceneObject(RuntimeSceneObject&&) noexcept = default;
    RuntimeSceneObject& operator=(RuntimeSceneObject&&) noexcept = default;

    [[nodiscard]] const RuntimeSceneObjectMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] bool loaded() const noexcept { return asset_.has_value(); }
    [[nodiscard]] const CookedVoxelAsset* asset() const noexcept {
        return asset_ ? &*asset_ : nullptr;
    }
    [[nodiscard]] const PackedBrickmapScene* packed_brickmap() const noexcept {
        return packedBrickmap_ ? &*packedBrickmap_ : nullptr;
    }
    [[nodiscard]] const IncrementalConnectivityCache* connectivity() const noexcept {
        return connectivity_ ? &*connectivity_ : nullptr;
    }
    [[nodiscard]] std::span<const VoxelBox> collision_boxes() const noexcept {
        return collisionBoxes_;
    }
    [[nodiscard]] const std::optional<RigidBodyCreateDesc>& initial_body_desc() const noexcept {
        return initialBodyDesc_;
    }
    [[nodiscard]] const std::optional<StaticRigidBodyCreateDesc>& initial_static_body_desc() const noexcept {
        return initialStaticBodyDesc_;
    }
    [[nodiscard]] RuntimeBrickmapHandle renderer_handle() const noexcept { return rendererHandle_; }
    [[nodiscard]] RigidBodyHandle body_handle() const noexcept { return bodyHandle_; }
    [[nodiscard]] std::uint64_t residency_generation() const noexcept { return residencyGeneration_; }
    [[nodiscard]] RuntimeSceneObjectStats stats() const noexcept;

private:
    friend class RuntimeSceneWorld;
    friend struct RuntimeSceneParserAccess;

    RuntimeSceneObjectMetadata metadata_{};
    std::filesystem::path resolvedAssetPath_{};
    std::optional<CookedVoxelAsset> asset_{};
    std::optional<PackedBrickmapScene> packedBrickmap_{};
    std::optional<IncrementalConnectivityCache> connectivity_{};
    std::vector<VoxelBox> collisionBoxes_{};
    bool collisionProxyOverBudget_{};
    std::optional<RigidBodyCreateDesc> initialBodyDesc_{};
    std::optional<StaticRigidBodyCreateDesc> initialStaticBodyDesc_{};
    RuntimeBrickmapHandle rendererHandle_{kInvalidRuntimeBrickmapHandle};
    std::uint64_t rendererReadbackHash_{};
    RigidBodyHandle bodyHandle_{kInvalidRigidBodyHandle};
    std::uint64_t residencyGeneration_{1};
};

class RuntimeScene {
public:
    RuntimeScene() = default;
    RuntimeScene(const RuntimeScene&) = delete;
    RuntimeScene& operator=(const RuntimeScene&) = delete;
    RuntimeScene(RuntimeScene&&) noexcept = default;
    RuntimeScene& operator=(RuntimeScene&&) noexcept = default;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept { return manifestPath_; }
    [[nodiscard]] std::span<const RuntimeSceneObject> objects() const noexcept { return objects_; }
    [[nodiscard]] std::size_t loaded_object_count() const noexcept;
    [[nodiscard]] float uniform_voxel_size_meters() const noexcept { return uniformVoxelSizeMeters_; }
    [[nodiscard]] bool mixed_voxel_sizes() const noexcept { return mixedVoxelSizes_; }
    [[nodiscard]] const RuntimeSceneObject* find_object(std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::uint64_t state_hash() const noexcept;

private:
    friend class RuntimeSceneWorld;
    friend struct RuntimeSceneParserAccess;

    std::string name_;
    std::filesystem::path manifestPath_;
    std::filesystem::path packageRoot_;
    std::vector<RuntimeSceneObject> objects_;
    RuntimeSceneLoadOptions options_{};
    float uniformVoxelSizeMeters_{};
    bool mixedVoxelSizes_{};
};

class RuntimeSceneStaging {
public:
    RuntimeSceneStaging() = default;
    RuntimeSceneStaging(const RuntimeSceneStaging&) = delete;
    RuntimeSceneStaging& operator=(const RuntimeSceneStaging&) = delete;
    RuntimeSceneStaging(RuntimeSceneStaging&&) noexcept = default;
    RuntimeSceneStaging& operator=(RuntimeSceneStaging&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return scene_.has_value(); }
    [[nodiscard]] std::size_t estimated_bytes() const noexcept { return estimatedBytes_; }
    [[nodiscard]] std::uint64_t staged_scene_hash() const noexcept {
        return scene_ ? scene_->state_hash() : 0U;
    }
private:
    friend class RuntimeSceneWorld;
    friend class RuntimeSceneStagingExecutor;
    std::optional<RuntimeScene> scene_{};
    std::size_t estimatedBytes_{};
    RuntimeSceneCancellationToken cancellation_{};
    RuntimeSceneProgressCallback progress_{};
    std::shared_ptr<void> memoryReservation_{};
};

class RuntimeDeferredObjectStaging {
public:
    RuntimeDeferredObjectStaging() = default;
    RuntimeDeferredObjectStaging(const RuntimeDeferredObjectStaging&) = delete;
    RuntimeDeferredObjectStaging& operator=(const RuntimeDeferredObjectStaging&) = delete;
    RuntimeDeferredObjectStaging(RuntimeDeferredObjectStaging&&) noexcept = default;
    RuntimeDeferredObjectStaging& operator=(RuntimeDeferredObjectStaging&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return object_.has_value(); }
    [[nodiscard]] RuntimeSceneHandle scene_handle() const noexcept { return sceneHandle_; }
    [[nodiscard]] std::uint64_t object_id() const noexcept { return objectId_; }
    [[nodiscard]] std::size_t estimated_bytes() const noexcept { return estimatedBytes_; }
private:
    friend class RuntimeSceneWorld;
    friend class RuntimeSceneStagingExecutor;
    RuntimeSceneHandle sceneHandle_{kInvalidRuntimeSceneHandle};
    std::uint64_t objectId_{};
    std::uint64_t expectedResidencyGeneration_{};
    RuntimeSceneLoadOptions options_{};
    std::optional<RuntimeSceneObject> object_{};
    std::size_t estimatedBytes_{};
    RuntimeSceneCancellationToken cancellation_{};
    RuntimeSceneProgressCallback progress_{};
    std::shared_ptr<void> memoryReservation_{};
};

struct RuntimeDeferredObjectStageResult {
    RuntimeDeferredObjectStaging staging{};
    RuntimeSceneError error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return staging.valid() && !error;
    }
};

struct RuntimeSceneStageResult {
    RuntimeSceneStaging staging{};
    RuntimeSceneError error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return staging.valid() && !error;
    }
};

struct RuntimeSceneStagingExecutorStats {
    std::size_t workerThreads{};
    std::size_t queuedSceneJobs{};
    std::size_t queuedDeferredJobs{};
    std::array<std::size_t, 4> queuedByPriority{};
    std::size_t runningJobs{};
    std::size_t retainedStagingBytes{};
    std::size_t peakRetainedStagingBytes{};
    std::uint64_t submittedSceneJobs{};
    std::uint64_t submittedDeferredJobs{};
    std::array<std::uint64_t, 4> submittedByPriority{};
    std::uint64_t completedJobs{};
    std::uint64_t failedJobs{};
    std::uint64_t cancelledJobs{};
    std::uint64_t queueRejectedJobs{};
    std::uint64_t deadlineExpiredJobs{};
    std::uint64_t backpressureWaitCount{};
    std::uint64_t totalQueueWaitMicroseconds{};
    std::uint64_t maximumQueueWaitMicroseconds{};
    std::uint64_t totalStageMicroseconds{};
    std::uint64_t maximumStageMicroseconds{};
};

struct RuntimeSceneLoadResult {
    RuntimeSceneHandle handle{kInvalidRuntimeSceneHandle};
    RuntimeSceneError error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle != kInvalidRuntimeSceneHandle && !error;
    }
};

class RuntimeSceneStagingExecutor;

// Transactional owner for runtime scene packages. CPU authority, packed-brickmap mirrors,
// connectivity caches, collision proxies, and optional dynamic-body publication are all staged
// before the scene is inserted. A failed load does not reserve a handle or object ID.
class RuntimeSceneWorld {
public:
    explicit RuntimeSceneWorld(
        IRigidBodyWorld* physicsWorld = nullptr,
        IRuntimeBrickmapWorld* rendererWorld = nullptr) noexcept
        : physicsWorld_(physicsWorld), rendererWorld_(rendererWorld),
          authorityThread_(std::this_thread::get_id()) {}
    ~RuntimeSceneWorld();

    [[nodiscard]] RuntimeSceneStageResult stage_scene_package(
        const std::filesystem::path& manifestPath,
        const RuntimeSceneLoadOptions& options = {},
        const RuntimeSceneStageOptions& stageOptions = {}) const;

    [[nodiscard]] std::future<RuntimeSceneStageResult> stage_scene_package_async(
        std::filesystem::path manifestPath,
        RuntimeSceneLoadOptions options = {},
        RuntimeSceneStageOptions stageOptions = {}) const;

    [[nodiscard]] RuntimeDeferredObjectStageResult stage_deferred_object(
        RuntimeSceneHandle sceneHandle,
        std::uint64_t objectId,
        const RuntimeSceneStageOptions& stageOptions = {}) const;

    [[nodiscard]] std::future<RuntimeDeferredObjectStageResult> stage_deferred_object_async(
        RuntimeSceneHandle sceneHandle,
        std::uint64_t objectId,
        RuntimeSceneStageOptions stageOptions = {}) const;

    [[nodiscard]] RuntimeSceneLoadResult publish_staged_scene(
        RuntimeSceneStaging&& staging);

    [[nodiscard]] bool publish_staged_deferred_object(
        RuntimeDeferredObjectStaging&& staging,
        RuntimeSceneError* error = nullptr);

    [[nodiscard]] RuntimeSceneLoadResult load_scene_package(
        const std::filesystem::path& manifestPath,
        const RuntimeSceneLoadOptions& options = {});

    [[nodiscard]] bool load_deferred_object(
        RuntimeSceneHandle sceneHandle,
        std::uint64_t objectId,
        RuntimeSceneError* error = nullptr);

    [[nodiscard]] bool unload_object(
        RuntimeSceneHandle sceneHandle,
        std::uint64_t objectId,
        RuntimeSceneError* error = nullptr);

    [[nodiscard]] bool unload_scene(
        RuntimeSceneHandle sceneHandle,
        RuntimeSceneError* error = nullptr);

    // Writes a self-contained checkpoint directory containing a generated scene manifest,
    // one DVOX file per reserved object, and complete rigid-body interpolation state.
    [[nodiscard]] bool save_scene_checkpoint(
        RuntimeSceneHandle sceneHandle,
        const std::filesystem::path& checkpointDirectory,
        RuntimeSceneError* error = nullptr) const;

    // Stages and validates the self-contained checkpoint before publishing it. Loaded/deferred
    // status is restored exactly; body state is applied only after asset validation succeeds.
    [[nodiscard]] RuntimeSceneLoadResult restore_scene_checkpoint(
        const std::filesystem::path& checkpointDirectory,
        const RuntimeSceneCheckpointOptions& options = {});

    // Transactionally replaces a scene package while retaining the same scene handle and stable
    // object IDs. The object hierarchy, anchored/dynamic classification, and voxel size must be
    // compatible. Occupancy changes require an explicit migration opt-in.
    [[nodiscard]] RuntimeSceneHotReloadResult hot_reload_scene(
        RuntimeSceneHandle sceneHandle,
        const std::filesystem::path& replacementManifest,
        const RuntimeSceneHotReloadOptions& options = {});

    [[nodiscard]] const RuntimeScene* scene(RuntimeSceneHandle handle) const noexcept;
    [[nodiscard]] RuntimeScene* scene(RuntimeSceneHandle handle) noexcept;
    [[nodiscard]] std::size_t scene_count() const noexcept;
    [[nodiscard]] std::size_t reserved_object_count() const noexcept;
    [[nodiscard]] std::size_t loaded_object_count() const noexcept;
    [[nodiscard]] bool object_id_reserved(std::uint64_t objectId) const noexcept;
    [[nodiscard]] std::uint64_t state_hash() const noexcept;

private:
    friend class RuntimeSceneStagingExecutor;

    struct DeferredStageSnapshot {
        RuntimeSceneHandle sceneHandle{kInvalidRuntimeSceneHandle};
        std::uint64_t objectId{};
        std::uint64_t residencyGeneration{};
        RuntimeSceneLoadOptions options{};
        RuntimeSceneObjectMetadata metadata{};
        std::filesystem::path resolvedAssetPath{};
    };

    struct SceneSlot {
        bool alive{};
        RuntimeScene scene{};
    };

    IRigidBodyWorld* physicsWorld_{};
    IRuntimeBrickmapWorld* rendererWorld_{};
    std::thread::id authorityThread_{};
    std::vector<SceneSlot> scenes_{};
    std::vector<RuntimeSceneHandle> freeHandles_{};

    [[nodiscard]] RuntimeSceneHandle commit_scene(RuntimeScene&& scene);
    [[nodiscard]] RuntimeSceneStageResult stage_scene_package_with_jobs(
        const std::filesystem::path& manifestPath,
        const RuntimeSceneLoadOptions& options,
        const RuntimeSceneStageOptions& stageOptions,
        JobSystem& jobs) const;
    [[nodiscard]] std::optional<DeferredStageSnapshot> snapshot_deferred_object(
        RuntimeSceneHandle sceneHandle,
        std::uint64_t objectId,
        RuntimeSceneError& error) const;
    [[nodiscard]] static RuntimeDeferredObjectStageResult stage_deferred_snapshot(
        DeferredStageSnapshot snapshot,
        const RuntimeSceneStageOptions& stageOptions);
    [[nodiscard]] static bool load_object_asset(
        RuntimeSceneObject& object,
        const RuntimeSceneLoadOptions& options,
        RuntimeSceneError& error);
    [[nodiscard]] static bool derive_object(
        RuntimeSceneObject& object,
        const RuntimeSceneLoadOptions& options,
        RuntimeSceneError& error);
    [[nodiscard]] static std::size_t estimate_staging_bytes(
        const RuntimeScene& scene) noexcept;
    [[nodiscard]] bool publish_renderer_batch(
        RuntimeScene& scene,
        RuntimeSceneError& error);
    [[nodiscard]] bool publish_physics_batch(
        RuntimeScene& scene,
        RuntimeSceneError& error);
    [[nodiscard]] bool publish_renderer(
        RuntimeSceneObject& object,
        RuntimeSceneError& error);
    [[nodiscard]] bool publish_body(
        RuntimeSceneObject& object,
        const RuntimeSceneLoadOptions& options,
        RuntimeSceneError& error);
    [[nodiscard]] bool destroy_renderer(RuntimeSceneObject& object, RuntimeSceneError& error);
    [[nodiscard]] bool destroy_body(RuntimeSceneObject& object, RuntimeSceneError& error);
    [[nodiscard]] bool on_authority_thread() const noexcept {
        return std::this_thread::get_id() == authorityThread_;
    }
};


// Process-owned staging executor. One coordinator drains two fair queues and reuses a fixed
// JobSystem for all full-scene jobs. Completed staging memory remains charged to the executor
// until the staging object is published or destroyed.
class RuntimeSceneStagingExecutor {
public:
    explicit RuntimeSceneStagingExecutor(
        std::size_t workerCount = 0,
        std::size_t maximumRetainedStagingBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL);
    explicit RuntimeSceneStagingExecutor(const RuntimeSceneStagingExecutorConfig& config);
    ~RuntimeSceneStagingExecutor();

    RuntimeSceneStagingExecutor(const RuntimeSceneStagingExecutor&) = delete;
    RuntimeSceneStagingExecutor& operator=(const RuntimeSceneStagingExecutor&) = delete;

    [[nodiscard]] std::future<RuntimeSceneStageResult> submit_scene(
        std::filesystem::path manifestPath,
        RuntimeSceneLoadOptions options = {},
        RuntimeSceneStageOptions stageOptions = {},
        RuntimeSceneSubmissionOptions submissionOptions = {});

    // Must be called on world's authority thread so the deferred reservation can be snapshotted
    // without racing scene unload or hot reload. Heavy I/O and derivation run on the executor.
    [[nodiscard]] std::future<RuntimeDeferredObjectStageResult> submit_deferred_object(
        const RuntimeSceneWorld& world,
        RuntimeSceneHandle sceneHandle,
        std::uint64_t objectId,
        RuntimeSceneStageOptions stageOptions = {},
        RuntimeSceneSubmissionOptions submissionOptions = {});

    [[nodiscard]] RuntimeSceneStagingExecutorStats stats() const noexcept;

    // Public only so translation-unit implementation helpers can own release tokens. These are
    // incomplete implementation types and are not part of the stable engine interface.
    struct SharedState;
    struct Task;

private:
    std::shared_ptr<SharedState> state_{};
    JobSystem jobs_;
    std::thread coordinator_{};

    void coordinator_loop();
};

enum class RuntimeScenePublicationKind : std::uint8_t { Scene, DeferredObject };

struct RuntimeScenePublicationResult {
    RuntimeScenePublicationKind kind{RuntimeScenePublicationKind::Scene};
    RuntimeSceneHandle sceneHandle{kInvalidRuntimeSceneHandle};
    std::optional<std::uint64_t> objectId{};
    RuntimeSceneError error{};
    [[nodiscard]] explicit operator bool() const noexcept { return !error; }
};

struct RuntimeScenePublicationQueueConfig {
    std::size_t maximumPendingPublications{64U};
    std::size_t maximumRetainedBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct RuntimeScenePublicationQueueStats {
    std::size_t pending{};
    std::size_t retainedBytes{};
    std::size_t peakRetainedBytes{};
    std::uint64_t enqueued{};
    std::uint64_t published{};
    std::uint64_t rejected{};
    std::uint64_t expired{};
    std::uint64_t failed{};
};

// Authority-thread queue that owns completed staging until publication. It applies a second,
// explicit bound after background staging and selects critical/deadline work before background
// objects. This prevents a burst of completed jobs from becoming unbounded main-thread debt.
class RuntimeScenePublicationQueue {
public:
    explicit RuntimeScenePublicationQueue(
        const RuntimeScenePublicationQueueConfig& config = {});

    [[nodiscard]] bool enqueue_scene(
        RuntimeSceneStageResult&& result,
        RuntimeSceneSubmissionOptions options = {},
        RuntimeSceneError* error = nullptr);
    [[nodiscard]] bool enqueue_deferred_object(
        RuntimeDeferredObjectStageResult&& result,
        RuntimeSceneSubmissionOptions options = {},
        RuntimeSceneError* error = nullptr);

    [[nodiscard]] std::optional<RuntimeScenePublicationResult> publish_next(
        RuntimeSceneWorld& world);
    [[nodiscard]] RuntimeScenePublicationQueueStats stats() const noexcept;
    void clear() noexcept;

private:
    struct Entry {
        RuntimeScenePublicationKind kind{RuntimeScenePublicationKind::Scene};
        RuntimeSceneSubmissionOptions options{};
        std::chrono::steady_clock::time_point enqueuedAt{};
        std::uint64_t sequence{};
        std::size_t bytes{};
        std::variant<RuntimeSceneStaging, RuntimeDeferredObjectStaging> staging{};
    };

    RuntimeScenePublicationQueueConfig config_{};
    std::thread::id authorityThread_{};
    std::vector<Entry> entries_{};
    RuntimeScenePublicationQueueStats stats_{};
    std::uint64_t nextSequence_{};
};

[[nodiscard]] const char* to_string(RuntimeSceneErrorCode code) noexcept;
[[nodiscard]] const char* to_string(RuntimeSceneJobPriority priority) noexcept;

} // namespace dve
