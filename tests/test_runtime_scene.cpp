#include "dve/runtime_scene.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error(std::string("CHECK failed: ") + #condition + \
        " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
} while (false)

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("unable to write " + path.string());
    output << text;
}

[[nodiscard]] std::filesystem::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dve_runtime_scene_tests_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

void copy_house_package(const std::filesystem::path& destination) {
    const std::filesystem::path source = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples";
    for (const char* file : {
             "multi_object_house.dvoxscene.json",
             "multi_object_house_Foundation_3e9.dvox",
             "multi_object_house_UpperBlock_3ea.dvox",
             "multi_object_house_Furniture_3eb.dvox"}) {
        std::filesystem::copy_file(source / file, destination / file,
                                   std::filesystem::copy_options::overwrite_existing);
    }
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const std::size_t position = text.find(from);
    if (position == std::string::npos) throw std::runtime_error("replacement source not found: " + from);
    text.replace(position, from.size(), to);
}

class FailingRigidBodyWorld final : public dve::IRigidBodyWorld {
public:
    explicit FailingRigidBodyWorld(std::size_t failOnAttempt) : failOnAttempt_(failOnAttempt) {}

    dve::RigidBodyHandle create_body(const dve::RigidBodyCreateDesc& desc) override {
        ++attempts_;
        if (attempts_ == failOnAttempt_) return dve::kInvalidRigidBodyHandle;
        return world_.create_body(desc);
    }
    dve::RigidBodyHandle create_static_body(
        const dve::StaticRigidBodyCreateDesc& desc) override {
        return world_.create_static_body(desc);
    }
    bool destroy_body(dve::RigidBodyHandle handle) override { return world_.destroy_body(handle); }
    std::optional<dve::RigidBodyState> state(dve::RigidBodyHandle handle) const override {
        return world_.state(handle);
    }
    bool set_state(dve::RigidBodyHandle handle, const dve::RigidBodyState& state) override {
        return world_.set_state(handle, state);
    }
    bool apply_impulse(dve::RigidBodyHandle handle, dve::Float3 impulse) override {
        return world_.apply_impulse(handle, impulse);
    }
    bool apply_force(dve::RigidBodyHandle handle, dve::Float3 force) override {
        return world_.apply_force(handle, force);
    }
    void step(float delta) override { world_.step(delta); }
    [[nodiscard]] std::size_t body_count() const noexcept { return world_.body_count(); }

private:
    std::size_t failOnAttempt_{};
    std::size_t attempts_{};
    dve::ReferenceRigidBodyWorld world_{};
};

class FailingRuntimeBrickmapWorld final : public dve::IRuntimeBrickmapWorld {
public:
    explicit FailingRuntimeBrickmapWorld(std::size_t failOnAttempt)
        : failOnAttempt_(failOnAttempt) {}

    dve::RuntimeBrickmapHandle create_object(
        const dve::RuntimeBrickmapCreateDesc& desc) override {
        ++attempts_;
        if (attempts_ == failOnAttempt_) return dve::kInvalidRuntimeBrickmapHandle;
        return world_.create_object(desc);
    }
    bool destroy_object(dve::RuntimeBrickmapHandle handle) override {
        return world_.destroy_object(handle);
    }
    std::optional<std::uint64_t> readback_hash(
        dve::RuntimeBrickmapHandle handle) const override {
        return world_.readback_hash(handle);
    }
    void set_fail_on_attempt(std::size_t attempt) noexcept { failOnAttempt_ = attempt; }
    [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }
    [[nodiscard]] std::size_t object_count() const noexcept { return world_.object_count(); }

private:
    std::size_t failOnAttempt_{};
    std::size_t attempts_{};
    dve::ReferenceRuntimeBrickmapWorld world_{};
};


[[nodiscard]] bool contains_id(
    const std::vector<std::uint64_t>& values,
    std::uint64_t id) {
    return std::find(values.begin(), values.end(), id) != values.end();
}

void modify_asset_material(
    const std::filesystem::path& path,
    float newRed,
    float densityDelta = 0.0F) {
    dve::DvoxReadResult read = dve::read_dvox(path);
    CHECK(read.success);
    CHECK(read.asset.materials.size() > 1U);
    read.asset.materials[1].baseColor.x = newRed;
    read.asset.materials[1].densityKilogramsPerCubicMeter += densityDelta;
    std::string error;
    CHECK(dve::write_dvox(path, read.asset, {}, &error));
}

void add_asset_voxel(
    const std::filesystem::path& path,
    dve::Int3 voxel) {
    dve::DvoxReadResult read = dve::read_dvox(path);
    CHECK(read.success);
    CHECK(read.asset.materials.size() > 1U);
    CHECK(!read.asset.object.occupied_at(voxel));
    (void)read.asset.object.set_voxel(voxel, 1);
    std::string error;
    CHECK(dve::write_dvox(path, read.asset, {}, &error));
}

void set_distinct_body_state(
    dve::ReferenceRigidBodyWorld& physics,
    dve::RigidBodyHandle handle) {
    std::optional<dve::RigidBodyState> current = physics.state(handle);
    CHECK(current.has_value());
    current->previousTransform.position = {11.0F, 12.0F, 13.0F};
    current->currentTransform.position = {14.0F, 15.0F, 16.0F};
    current->linearVelocity = {1.25F, -2.5F, 3.75F};
    current->angularVelocity = {-0.5F, 0.75F, 1.0F};
    current->sleeping = false;
    CHECK(physics.set_state(handle, *current));
}

void check_distinct_body_state(const dve::RigidBodyState& state) {
    CHECK(std::abs(state.previousTransform.position.x - 11.0F) < 1.0e-6F);
    CHECK(std::abs(state.currentTransform.position.y - 15.0F) < 1.0e-6F);
    CHECK(std::abs(state.currentTransform.position.z - 16.0F) < 1.0e-6F);
    CHECK(std::abs(state.linearVelocity.x - 1.25F) < 1.0e-6F);
    CHECK(std::abs(state.linearVelocity.y + 2.5F) < 1.0e-6F);
    CHECK(std::abs(state.angularVelocity.z - 1.0F) < 1.0e-6F);
    CHECK(!state.sleeping);
}

void test_valid_transaction_and_unload() {
    using namespace dve;
    ReferenceRigidBodyWorld physics;
    RuntimeSceneWorld world(&physics);
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult result = world.load_scene_package(manifest);
    if (!result) std::cerr << "valid load error: " << to_string(result.error.code) << ": " << result.error.message << " path=" << result.error.path << "\n";
    CHECK(result);
    CHECK(world.scene_count() == 1);
    CHECK(world.reserved_object_count() == 3);
    CHECK(world.loaded_object_count() == 3);
    CHECK(physics.body_count() == 3); // one anchored static body and two dynamic bodies
    const RuntimeScene* scene = world.scene(result.handle);
    CHECK(scene != nullptr);
    CHECK(scene->name() == "MultiObjectHouse");
    CHECK(scene->objects().size() == 3);
    CHECK(scene->loaded_object_count() == 3);
    CHECK(scene->uniform_voxel_size_meters() == 0.0F); // package deliberately mixes 0.5 m and 0.25 m assets
    CHECK(scene->mixed_voxel_sizes());
    for (const RuntimeSceneObject& object : scene->objects()) {
        CHECK(object.loaded());
        CHECK(object.asset() != nullptr);
        CHECK(object.packed_brickmap() != nullptr);
        CHECK(object.connectivity() != nullptr);
        CHECK(object.packed_brickmap()->validate_against(object.asset()->object));
        CHECK(object.connectivity()->validate(object.asset()->object));
        CHECK(object.stats().authorityHash != 0);
        CHECK(object.stats().packedBrickmapHash != 0);
        CHECK(object.body_handle() != kInvalidRigidBodyHandle);
    }
    const std::uint64_t hash = scene->state_hash();
    CHECK(hash != 0);
    CHECK(world.unload_scene(result.handle));
    CHECK(world.scene_count() == 0);
    CHECK(world.reserved_object_count() == 0);
    CHECK(physics.body_count() == 0);

    const RuntimeSceneLoadResult repeated = world.load_scene_package(manifest);
    CHECK(repeated);
    CHECK(world.scene(repeated.handle)->state_hash() == hash);
    CHECK(world.unload_scene(repeated.handle));
}

void test_deferred_loading_preserves_ids() {
    using namespace dve;
    ReferenceRigidBodyWorld physics;
    RuntimeSceneWorld world(&physics);
    RuntimeSceneLoadOptions options;
    options.loadAllObjects = false;
    options.initiallyLoadedObjectIds = {1001};
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult result = world.load_scene_package(manifest, options);
    CHECK(result);
    CHECK(world.reserved_object_count() == 3);
    CHECK(world.loaded_object_count() == 1);
    CHECK(physics.body_count() == 1);
    const RuntimeScene* scene = world.scene(result.handle);
    CHECK(scene->find_object(1001)->loaded());
    CHECK(!scene->find_object(1002)->loaded());
    CHECK(!scene->find_object(1003)->loaded());

    RuntimeSceneError error;
    CHECK(world.load_deferred_object(result.handle, 1002, &error));
    CHECK(world.loaded_object_count() == 2);
    CHECK(physics.body_count() == 2);
    CHECK(world.scene(result.handle)->find_object(1002)->metadata().id == 1002);
    CHECK(world.load_deferred_object(result.handle, 1003, &error));
    CHECK(physics.body_count() == 3);
    CHECK(world.unload_object(result.handle, 1002, &error));
    CHECK(world.reserved_object_count() == 3);
    CHECK(world.loaded_object_count() == 2);
    CHECK(physics.body_count() == 2);
    CHECK(world.scene(result.handle)->find_object(1002)->metadata().id == 1002);
    CHECK(world.load_deferred_object(result.handle, 1002, &error));
    CHECK(physics.body_count() == 3);
    CHECK(world.unload_scene(result.handle));
}

void test_rigid_matrix_rotation_and_translation() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "rotated_scene.json";
    std::string text = read_text(temp / "multi_object_house.dvoxscene.json");
    replace_once(text,
        "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]",
        "[0,1,0,0,-1,0,0,0,0,0,1,0,2,3,4,1]");
    write_text(manifest, text);

    RuntimeSceneWorld world;
    const RuntimeSceneLoadResult result = world.load_scene_package(manifest);
    CHECK(result);
    const RuntimeSceneObject* foundation = world.scene(result.handle)->find_object(1001);
    CHECK(foundation != nullptr);
    const Float3 rotated = transform_vector(foundation->metadata().worldTransform, {1.0F, 0.0F, 0.0F});
    CHECK(std::abs(rotated.x) < 1.0e-5F);
    CHECK(std::abs(rotated.y - 1.0F) < 1.0e-5F);
    CHECK(std::abs(rotated.z) < 1.0e-5F);
    const Float3 origin = transform_point(foundation->metadata().worldTransform, {});
    CHECK(std::abs(origin.x - 2.0F) < 1.0e-5F);
    CHECK(std::abs(origin.y - 3.0F) < 1.0e-5F);
    CHECK(std::abs(origin.z - 4.0F) < 1.0e-5F);
    CHECK(world.unload_scene(result.handle));
    std::filesystem::remove_all(temp);
}

void test_manifest_rejections_are_transactional() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path validPath = temp / "multi_object_house.dvoxscene.json";
    const std::string valid = read_text(validPath);

    const auto expect_failure = [&](const std::string& filename, std::string content,
                                    RuntimeSceneErrorCode code) {
        const std::filesystem::path path = temp / filename;
        write_text(path, content);
        ReferenceRigidBodyWorld physics;
        RuntimeSceneWorld world(&physics);
        const std::uint64_t before = world.state_hash();
        const RuntimeSceneLoadResult result = world.load_scene_package(path);
        CHECK(!result);
        CHECK(result.error.code == code);
        CHECK(world.scene_count() == 0);
        CHECK(world.reserved_object_count() == 0);
        CHECK(world.loaded_object_count() == 0);
        CHECK(world.state_hash() == before);
        CHECK(physics.body_count() == 0);
    };

    {
        std::string text = valid;
        replace_once(text, "\"id\":1002", "\"id\":1001");
        expect_failure("duplicate_id.json", text, RuntimeSceneErrorCode::DuplicateObjectId);
    }
    {
        std::string text = valid;
        replace_once(text, "\"parent\":null", "\"parent\":2");
        expect_failure("cycle.json", text, RuntimeSceneErrorCode::CyclicHierarchy);
    }
    {
        std::string text = valid;
        replace_once(text, "multi_object_house_Foundation_3e9.dvox", "../escape.dvox");
        expect_failure("escape.json", text, RuntimeSceneErrorCode::PathEscape);
    }
    {
        std::string text = valid;
        replace_once(text, "multi_object_house_Foundation_3e9.dvox", "missing.dvox");
        expect_failure("missing.json", text, RuntimeSceneErrorCode::MissingAsset);
    }
    {
        std::string text = valid;
        replace_once(text,
            "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]",
            "[2,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]");
        expect_failure("scaled.json", text, RuntimeSceneErrorCode::InvalidTransform);
    }
    std::filesystem::remove_all(temp);
}

void test_corrupt_dvox_and_duplicate_world_ids() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    {
        std::fstream file(temp / "multi_object_house_Furniture_3eb.dvox",
                          std::ios::in | std::ios::out | std::ios::binary);
        CHECK(file.good());
        file.seekg(-1, std::ios::end);
        char byte{};
        file.read(&byte, 1);
        byte ^= 0x5A;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
    }
    ReferenceRigidBodyWorld physics;
    RuntimeSceneWorld world(&physics);
    const RuntimeSceneLoadResult corrupt = world.load_scene_package(manifest);
    CHECK(!corrupt);
    CHECK(corrupt.error.code == RuntimeSceneErrorCode::DvoxReadFailed);
    CHECK(world.scene_count() == 0);
    CHECK(physics.body_count() == 0);

    std::filesystem::remove_all(temp);
    const auto valid = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult first = world.load_scene_package(valid);
    CHECK(first);
    const std::uint64_t before = world.state_hash();
    const RuntimeSceneLoadResult duplicate = world.load_scene_package(valid);
    CHECK(!duplicate);
    CHECK(duplicate.error.code == RuntimeSceneErrorCode::ObjectIdAlreadyReserved);
    CHECK(world.scene_count() == 1);
    CHECK(world.state_hash() == before);
    CHECK(physics.body_count() == 3);
    CHECK(world.unload_scene(first.handle));
}

void test_physics_publication_rolls_back() {
    using namespace dve;
    FailingRigidBodyWorld physics(2);
    RuntimeSceneWorld world(&physics);
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult result = world.load_scene_package(manifest);
    CHECK(!result);
    CHECK(result.error.code == RuntimeSceneErrorCode::PhysicsPublicationFailure);
    CHECK(world.scene_count() == 0);
    CHECK(world.reserved_object_count() == 0);
    CHECK(physics.body_count() == 0);
}


void test_voxel_size_policy_and_uint64_string_ids() {
    using namespace dve;
    const auto house = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    {
        RuntimeSceneWorld world;
        RuntimeSceneLoadOptions options;
        options.requireUniformVoxelSize = true;
        const RuntimeSceneLoadResult result = world.load_scene_package(house, options);
        CHECK(!result);
        CHECK(result.error.code == RuntimeSceneErrorCode::VoxelSizeMismatch);
        CHECK(world.scene_count() == 0);
    }
    {
        RuntimeSceneWorld world;
        RuntimeSceneLoadOptions options;
        options.expectedVoxelSizeMeters = 0.5F;
        const RuntimeSceneLoadResult result = world.load_scene_package(house, options);
        CHECK(!result);
        CHECK(result.error.code == RuntimeSceneErrorCode::VoxelSizeMismatch);
        CHECK(world.scene_count() == 0);
    }

    const std::filesystem::path temp = make_temp_dir();
    constexpr std::uint64_t largeId = 18446744073709550000ULL;
    CookedVoxelAsset asset(largeId);
    asset.voxelSizeMeters = 0.25F;
    asset.materials.resize(2);
    asset.materials[0].name = "air";
    asset.materials[0].densityKilogramsPerCubicMeter = 0.0F;
    asset.materials[1].name = "solid";
    asset.materials[1].densityKilogramsPerCubicMeter = 1000.0F;
    asset.object.set_voxel({0, 0, 0}, 1);
    std::string writeError;
    CHECK(write_dvox(temp / "large_id.dvox", asset, {}, &writeError));
    write_text(temp / "large_id.dvoxscene.json", R"json({
  "format":"DVOXSCENE","version":1,"name":"LargeId",
  "objects":[{"index":0,"id":"18446744073709550000","name":"Large","nodePath":"/Large","file":"large_id.dvox","parent":null,"anchored":true,"structural":true,"generateCollision":false,"worldMatrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]
}
)json");
    RuntimeSceneWorld world;
    const RuntimeSceneLoadResult result = world.load_scene_package(temp / "large_id.dvoxscene.json");
    CHECK(result);
    CHECK(world.scene(result.handle)->find_object(largeId) != nullptr);
    CHECK(world.unload_scene(result.handle));
    std::filesystem::remove_all(temp);
}


void test_deferred_asset_size_is_rechecked() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    RuntimeSceneWorld world;
    RuntimeSceneLoadOptions options;
    options.loadAllObjects = false;
    options.maximumDvoxBytesPerObject =
        std::filesystem::file_size(temp / "multi_object_house_Foundation_3e9.dvox");
    const RuntimeSceneLoadResult result = world.load_scene_package(manifest, options);
    CHECK(result);
    {
        std::ofstream output(temp / "multi_object_house_Foundation_3e9.dvox",
                             std::ios::binary | std::ios::app);
        CHECK(output.good());
        output.put('x');
    }
    RuntimeSceneError error;
    CHECK(!world.load_deferred_object(result.handle, 1001, &error));
    CHECK(error.code == RuntimeSceneErrorCode::DvoxReadFailed);
    CHECK(!world.scene(result.handle)->find_object(1001)->loaded());
    CHECK(world.loaded_object_count() == 0);
    CHECK(world.unload_scene(result.handle));
    std::filesystem::remove_all(temp);
}

void test_world_destructor_releases_bodies() {
    using namespace dve;
    ReferenceRigidBodyWorld physics;
    {
        RuntimeSceneWorld world(&physics);
        const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
            "multi_object_house.dvoxscene.json";
        const RuntimeSceneLoadResult result = world.load_scene_package(manifest);
        CHECK(result);
        CHECK(physics.body_count() == 3);
    }
    CHECK(physics.body_count() == 0);
}

void test_unknown_deferred_selection() {
    using namespace dve;
    RuntimeSceneWorld world;
    RuntimeSceneLoadOptions options;
    options.loadAllObjects = false;
    options.initiallyLoadedObjectIds = {999999};
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult result = world.load_scene_package(manifest, options);
    CHECK(!result);
    CHECK(result.error.code == RuntimeSceneErrorCode::UnknownRequestedObject);
    CHECK(world.scene_count() == 0);
}


void test_renderer_publication_and_deferred_slot_reuse() {
    using namespace dve;
    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    RuntimeSceneLoadOptions options;
    options.loadAllObjects = false;
    options.initiallyLoadedObjectIds = {1001};
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";

    const RuntimeSceneLoadResult result = world.load_scene_package(manifest, options);
    CHECK(result);
    CHECK(renderer.object_count() == 1);
    CHECK(physics.body_counts().staticBodies == 1);
    const RuntimeSceneObject* foundation = world.scene(result.handle)->find_object(1001);
    CHECK(foundation != nullptr);
    CHECK(foundation->renderer_handle() != kInvalidRuntimeBrickmapHandle);
    CHECK(foundation->stats().rendererReadbackHash == foundation->stats().packedBrickmapHash);

    RuntimeSceneError error;
    CHECK(world.load_deferred_object(result.handle, 1002, &error));
    CHECK(renderer.object_count() == 2);
    CHECK(physics.body_count() == 2);
    const RuntimeBrickmapHandle firstHandle =
        world.scene(result.handle)->find_object(1002)->renderer_handle();
    CHECK(world.unload_object(result.handle, 1002, &error));
    CHECK(renderer.object_count() == 1);
    CHECK(world.load_deferred_object(result.handle, 1002, &error));
    const RuntimeBrickmapHandle reusedHandle =
        world.scene(result.handle)->find_object(1002)->renderer_handle();
    CHECK(reusedHandle == firstHandle);
    CHECK(renderer.full_rebuild_count() == 0);
    CHECK(world.unload_scene(result.handle));
    CHECK(renderer.object_count() == 0);
    CHECK(physics.body_count() == 0);
}

void test_async_staging_is_deterministic_across_worker_counts() {
    using namespace dve;
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    std::uint64_t expectedHash = 0;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        ReferenceRigidBodyWorld physics;
        ReferenceRuntimeBrickmapWorld renderer;
        RuntimeSceneWorld world(&physics, &renderer);
        RuntimeSceneStageOptions stageOptions;
        stageOptions.workerCount = workers;
        std::atomic<std::size_t> progressEvents{};
        stageOptions.progress = [&](const RuntimeSceneProgress&) { ++progressEvents; };
        std::future<RuntimeSceneStageResult> future =
            world.stage_scene_package_async(manifest, {}, stageOptions);
        RuntimeSceneStageResult staged = future.get();
        CHECK(staged);
        CHECK(staged.staging.estimated_bytes() > 0);
        CHECK(progressEvents.load() >= 6U);
        const std::uint64_t stagedHash = staged.staging.staged_scene_hash();
        if (expectedHash == 0U) expectedHash = stagedHash;
        CHECK(stagedHash == expectedHash);
        CHECK(world.scene_count() == 0);
        CHECK(renderer.object_count() == 0);
        CHECK(physics.body_count() == 0);

        const RuntimeSceneLoadResult published =
            world.publish_staged_scene(std::move(staged.staging));
        CHECK(published);
        CHECK(world.scene(published.handle)->state_hash() == expectedHash);
        CHECK(renderer.object_count() == 3);
        const RigidBodyCounts counts = physics.body_counts();
        CHECK(counts.staticBodies == 1);
        CHECK(counts.dynamicBodies == 2);
        CHECK(world.unload_scene(published.handle));
    }
}

void test_cancellation_and_staging_memory_leave_world_unchanged() {
    using namespace dve;
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";

    for (const RuntimeSceneLoadPhase phase : {
             RuntimeSceneLoadPhase::AssetIo,
             RuntimeSceneLoadPhase::DerivedData}) {
        RuntimeSceneWorld world;
        const std::uint64_t before = world.state_hash();
        RuntimeSceneStageOptions stageOptions;
        stageOptions.workerCount = 4;
        stageOptions.progress = [token = stageOptions.cancellation, phase](
                                    const RuntimeSceneProgress& progress) {
            if (progress.phase == phase && progress.completed == 0U) token.request_cancel();
        };
        const RuntimeSceneStageResult staged =
            world.stage_scene_package(manifest, {}, stageOptions);
        CHECK(!staged);
        CHECK(staged.error.code == RuntimeSceneErrorCode::Cancelled);
        CHECK(world.state_hash() == before);
        CHECK(world.scene_count() == 0);
    }

    {
        RuntimeSceneWorld world;
        RuntimeSceneStageOptions stageOptions;
        stageOptions.maximumStagingBytes = 1U;
        const RuntimeSceneStageResult staged =
            world.stage_scene_package(manifest, {}, stageOptions);
        CHECK(!staged);
        CHECK(staged.error.code == RuntimeSceneErrorCode::StagingMemoryExceeded);
        CHECK(world.scene_count() == 0);
    }

    for (const RuntimeSceneLoadPhase phase : {
             RuntimeSceneLoadPhase::RendererPublication,
             RuntimeSceneLoadPhase::PhysicsPublication,
             RuntimeSceneLoadPhase::Commit}) {
        ReferenceRigidBodyWorld physics;
        ReferenceRuntimeBrickmapWorld renderer;
        RuntimeSceneWorld world(&physics, &renderer);
        RuntimeSceneStageOptions stageOptions;
        stageOptions.progress = [token = stageOptions.cancellation, phase](
                                    const RuntimeSceneProgress& progress) {
            if (progress.phase == phase && progress.completed == 0U) token.request_cancel();
        };
        RuntimeSceneStageResult staged = world.stage_scene_package(manifest, {}, stageOptions);
        CHECK(staged);
        const RuntimeSceneLoadResult published =
            world.publish_staged_scene(std::move(staged.staging));
        CHECK(!published);
        CHECK(published.error.code == RuntimeSceneErrorCode::Cancelled);
        CHECK(world.scene_count() == 0);
        CHECK(renderer.object_count() == 0);
        CHECK(physics.body_count() == 0);
    }
}

void test_wrong_thread_publication_is_rejected() {
    using namespace dve;
    RuntimeSceneWorld world;
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    RuntimeSceneStageResult staged = world.stage_scene_package(manifest);
    CHECK(staged);
    auto future = std::async(
        std::launch::async,
        [&world, staging = std::move(staged.staging)]() mutable {
            return world.publish_staged_scene(std::move(staging));
        });
    const RuntimeSceneLoadResult result = future.get();
    CHECK(!result);
    CHECK(result.error.code == RuntimeSceneErrorCode::WrongThread);
    CHECK(world.scene_count() == 0);
}

void test_renderer_publication_rolls_back() {
    using namespace dve;
    ReferenceRigidBodyWorld physics;
    FailingRuntimeBrickmapWorld renderer(2);
    RuntimeSceneWorld world(&physics, &renderer);
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult result = world.load_scene_package(manifest);
    CHECK(!result);
    CHECK(result.error.code == RuntimeSceneErrorCode::RendererPublicationFailure);
    CHECK(world.scene_count() == 0);
    CHECK(renderer.object_count() == 0);
    CHECK(physics.body_count() == 0);
}


void test_self_contained_checkpoint_restores_loaded_state_and_bodies() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    const std::filesystem::path checkpoint = temp / "saved_scene";
    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
    CHECK(loaded);

    RuntimeSceneError error;
    CHECK(world.unload_object(loaded.handle, 1002, &error));
    const RuntimeSceneObject* furniture = world.scene(loaded.handle)->find_object(1003);
    CHECK(furniture != nullptr);
    const RigidBodyHandle furnitureHandle = furniture->body_handle();
    CHECK(furnitureHandle != kInvalidRigidBodyHandle);
    RigidBodyState savedState = *physics.state(furnitureHandle);
    savedState.previousTransform.position = {3.0F, 4.0F, 5.0F};
    savedState.currentTransform.position = {3.5F, 4.5F, 5.5F};
    savedState.linearVelocity = {1.0F, 2.0F, 3.0F};
    savedState.angularVelocity = {0.25F, 0.5F, 0.75F};
    savedState.sleeping = false;
    CHECK(physics.set_state(furnitureHandle, savedState));

    CHECK(world.save_scene_checkpoint(loaded.handle, checkpoint, &error));
    CHECK(std::filesystem::is_regular_file(checkpoint / "checkpoint.dvstate"));
    CHECK(std::filesystem::is_regular_file(checkpoint / "checkpoint.dvoxscene.json"));
    CHECK(world.unload_scene(loaded.handle));
    CHECK(renderer.object_count() == 0);
    CHECK(physics.body_count() == 0);

    const RuntimeSceneLoadResult restored = world.restore_scene_checkpoint(checkpoint);
    if (!restored) {
        std::cerr << "checkpoint restore error: " << to_string(restored.error.code)
                  << ": " << restored.error.message << '\n';
    }
    CHECK(restored);
    const RuntimeScene* restoredScene = world.scene(restored.handle);
    CHECK(restoredScene->loaded_object_count() == 2);
    CHECK(!restoredScene->find_object(1002)->loaded());
    CHECK(restoredScene->find_object(1001)->loaded());
    CHECK(restoredScene->find_object(1003)->loaded());
    CHECK(renderer.object_count() == 2);
    CHECK(physics.body_count() == 2); // static foundation plus dynamic furniture
    const RigidBodyHandle restoredHandle = restoredScene->find_object(1003)->body_handle();
    const std::optional<RigidBodyState> restoredState = physics.state(restoredHandle);
    CHECK(restoredState.has_value());
    CHECK(std::abs(restoredState->previousTransform.position.x - 3.0F) < 1.0e-6F);
    CHECK(std::abs(restoredState->currentTransform.position.z - 5.5F) < 1.0e-6F);
    CHECK(std::abs(restoredState->linearVelocity.y - 2.0F) < 1.0e-6F);
    CHECK(std::abs(restoredState->angularVelocity.z - 0.75F) < 1.0e-6F);
    CHECK(world.unload_scene(restored.handle));
    std::filesystem::remove_all(temp);
}

void test_checkpoint_corruption_and_metadata_changes_are_transactional() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    const std::filesystem::path checkpoint = temp / "saved_scene";
    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
    CHECK(loaded);
    RuntimeSceneError error;
    CHECK(world.save_scene_checkpoint(loaded.handle, checkpoint, &error));
    CHECK(world.unload_scene(loaded.handle));

    const std::filesystem::path statePath = checkpoint / "checkpoint.dvstate";
    {
        std::fstream stateFile(statePath, std::ios::in | std::ios::out | std::ios::binary);
        CHECK(stateFile.good());
        stateFile.seekg(-1, std::ios::end);
        char byte{};
        stateFile.read(&byte, 1);
        byte ^= 0x5A;
        stateFile.seekp(-1, std::ios::end);
        stateFile.write(&byte, 1);
    }
    const RuntimeSceneLoadResult corrupt = world.restore_scene_checkpoint(checkpoint);
    CHECK(!corrupt);
    CHECK(corrupt.error.code == RuntimeSceneErrorCode::SavedStateInvalid);
    CHECK(world.scene_count() == 0);
    CHECK(renderer.object_count() == 0);
    CHECK(physics.body_count() == 0);

    std::filesystem::remove_all(checkpoint);
    const RuntimeSceneLoadResult reloaded = world.load_scene_package(manifest);
    CHECK(reloaded);
    CHECK(world.save_scene_checkpoint(reloaded.handle, checkpoint, &error));
    CHECK(world.unload_scene(reloaded.handle));
    const std::filesystem::path checkpointManifest = checkpoint / "checkpoint.dvoxscene.json";
    std::string text = read_text(checkpointManifest);
    replace_once(text,
        "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]",
        "[1,0,0,0,0,1,0,0,0,0,1,0,1,0,0,1]");
    write_text(checkpointManifest, text);
    const RuntimeSceneLoadResult incompatible = world.restore_scene_checkpoint(checkpoint);
    CHECK(!incompatible);
    CHECK(incompatible.error.code == RuntimeSceneErrorCode::AssetIncompatible);
    CHECK(world.scene_count() == 0);
    CHECK(renderer.object_count() == 0);
    CHECK(physics.body_count() == 0);
    std::filesystem::remove_all(temp);
}


void test_material_only_hot_reload_preserves_body_state() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    const std::filesystem::path furnitureAsset = temp / "multi_object_house_Furniture_3eb.dvox";

    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
    CHECK(loaded);
    const RuntimeSceneObject* beforeObject = world.scene(loaded.handle)->find_object(1003);
    CHECK(beforeObject != nullptr);
    CHECK(beforeObject->body_handle() != kInvalidRigidBodyHandle);
    set_distinct_body_state(physics, beforeObject->body_handle());
    const std::uint64_t beforeSceneHash = world.scene(loaded.handle)->state_hash();

    modify_asset_material(furnitureAsset, 0.137F, 125.0F);
    const RuntimeSceneHotReloadResult reloaded =
        world.hot_reload_scene(loaded.handle, manifest);
    if (!reloaded) {
        std::cerr << "material hot reload error: " << to_string(reloaded.error.code)
                  << ": " << reloaded.error.message << '\n';
    }
    CHECK(reloaded);
    CHECK(contains_id(reloaded.changedObjectIds, 1003));
    CHECK(world.scene_count() == 1);
    CHECK(world.scene(loaded.handle) != nullptr);
    CHECK(world.scene(loaded.handle)->state_hash() != beforeSceneHash);
    CHECK(renderer.object_count() == 3);
    CHECK(physics.body_counts().staticBodies == 1);
    CHECK(physics.body_counts().dynamicBodies == 2);

    const RuntimeSceneObject* afterObject = world.scene(loaded.handle)->find_object(1003);
    CHECK(afterObject != nullptr);
    CHECK(afterObject->asset() != nullptr);
    CHECK(std::abs(afterObject->asset()->materials[1].baseColor.x - 0.137F) < 1.0e-6F);
    CHECK(afterObject->stats().rendererReadbackHash == afterObject->stats().packedBrickmapHash);
    const std::optional<RigidBodyState> restored = physics.state(afterObject->body_handle());
    CHECK(restored.has_value());
    check_distinct_body_state(*restored);

    CHECK(world.unload_scene(loaded.handle));
    std::filesystem::remove_all(temp);
}


void test_persistent_renderer_hot_reload_keeps_handles_and_uses_updates() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    const std::filesystem::path furnitureAsset = temp / "multi_object_house_Furniture_3eb.dvox";

    ReferenceRigidBodyWorld physics;
    PersistentRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
    CHECK(loaded);

    std::vector<std::pair<std::uint64_t, RuntimeBrickmapHandle>> handles;
    for (const std::uint64_t id : {1001ULL, 1002ULL, 1003ULL}) {
        const RuntimeSceneObject* object = world.scene(loaded.handle)->find_object(id);
        CHECK(object != nullptr);
        CHECK(object->renderer_handle() != kInvalidRuntimeBrickmapHandle);
        handles.emplace_back(id, object->renderer_handle());
    }
    const PersistentRuntimeBrickmapWorldStats before = renderer.stats();
    CHECK(before.createCalls == 3U);
    CHECK(before.updateCalls == 0U);

    modify_asset_material(furnitureAsset, 0.423F, 50.0F);
    const RuntimeSceneHotReloadResult reloaded = world.hot_reload_scene(loaded.handle, manifest);
    if (!reloaded) {
        std::cerr << "persistent renderer hot reload error: "
                  << to_string(reloaded.error.code) << ": " << reloaded.error.message << '\n';
    }
    CHECK(reloaded);
    CHECK(contains_id(reloaded.changedObjectIds, 1003));

    const PersistentRuntimeBrickmapWorldStats after = renderer.stats();
    CHECK(after.createCalls == before.createCalls);
    CHECK(after.updateCalls == handles.size());
    CHECK(after.live.objects == handles.size());
    for (const auto& [id, handle] : handles) {
        const RuntimeSceneObject* object = world.scene(loaded.handle)->find_object(id);
        CHECK(object != nullptr);
        CHECK(object->renderer_handle() == handle);
        CHECK(renderer.object_id(handle).has_value());
        CHECK(*renderer.object_id(handle) == id);
        CHECK(renderer.readback_hash(handle).has_value());
        CHECK(*renderer.readback_hash(handle) == object->stats().packedBrickmapHash);
    }

    CHECK(world.unload_scene(loaded.handle));
    std::filesystem::remove_all(temp);
}

void test_hot_reload_rejects_topology_change_without_opt_in() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    const std::filesystem::path furnitureAsset = temp / "multi_object_house_Furniture_3eb.dvox";

    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
    CHECK(loaded);
    const std::uint64_t beforeWorldHash = world.state_hash();
    const std::uint64_t beforeSceneHash = world.scene(loaded.handle)->state_hash();
    const std::size_t beforeRendererCount = renderer.object_count();
    const RigidBodyCounts beforeBodyCounts = physics.body_counts();

    add_asset_voxel(furnitureAsset, {64, 64, 64});
    const RuntimeSceneHotReloadResult reloaded =
        world.hot_reload_scene(loaded.handle, manifest);
    CHECK(!reloaded);
    CHECK(reloaded.error.code == RuntimeSceneErrorCode::HotReloadIncompatible);
    CHECK(world.state_hash() == beforeWorldHash);
    CHECK(world.scene(loaded.handle)->state_hash() == beforeSceneHash);
    CHECK(renderer.object_count() == beforeRendererCount);
    CHECK(physics.body_counts().staticBodies == beforeBodyCounts.staticBodies);
    CHECK(physics.body_counts().dynamicBodies == beforeBodyCounts.dynamicBodies);

    CHECK(world.unload_scene(loaded.handle));
    std::filesystem::remove_all(temp);
}

void test_hot_reload_allows_topology_change_with_opt_in() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    const std::filesystem::path furnitureAsset = temp / "multi_object_house_Furniture_3eb.dvox";

    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
    CHECK(loaded);
    const RuntimeSceneObject* beforeObject = world.scene(loaded.handle)->find_object(1003);
    CHECK(beforeObject != nullptr);
    const std::uint64_t beforeVoxels = beforeObject->stats().voxels;
    set_distinct_body_state(physics, beforeObject->body_handle());

    add_asset_voxel(furnitureAsset, {64, 64, 64});
    RuntimeSceneHotReloadOptions options;
    options.allowVoxelTopologyChanges = true;
    const RuntimeSceneHotReloadResult reloaded =
        world.hot_reload_scene(loaded.handle, manifest, options);
    CHECK(reloaded);
    CHECK(contains_id(reloaded.changedObjectIds, 1003));
    const RuntimeSceneObject* afterObject = world.scene(loaded.handle)->find_object(1003);
    CHECK(afterObject != nullptr);
    CHECK(afterObject->stats().voxels == beforeVoxels + 1U);
    CHECK(afterObject->stats().rendererReadbackHash == afterObject->stats().packedBrickmapHash);
    const std::optional<RigidBodyState> restored = physics.state(afterObject->body_handle());
    CHECK(restored.has_value());
    check_distinct_body_state(*restored);
    CHECK(renderer.object_count() == 3);
    CHECK(physics.body_count() == 3);

    CHECK(world.unload_scene(loaded.handle));
    std::filesystem::remove_all(temp);
}

void test_hot_reload_backend_failure_restores_old_scene() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    const std::filesystem::path furnitureAsset = temp / "multi_object_house_Furniture_3eb.dvox";

    ReferenceRigidBodyWorld physics;
    FailingRuntimeBrickmapWorld renderer(std::numeric_limits<std::size_t>::max());
    RuntimeSceneWorld world(&physics, &renderer);
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest);
    CHECK(loaded);
    CHECK(renderer.attempts() == 3U);
    const RuntimeSceneObject* furniture = world.scene(loaded.handle)->find_object(1003);
    CHECK(furniture != nullptr);
    set_distinct_body_state(physics, furniture->body_handle());
    const std::uint64_t beforeWorldHash = world.state_hash();
    const std::uint64_t beforeSceneHash = world.scene(loaded.handle)->state_hash();

    modify_asset_material(furnitureAsset, 0.271F);
    renderer.set_fail_on_attempt(5U); // replacement object 1 succeeds, object 2 fails; rollback then succeeds
    const RuntimeSceneHotReloadResult reloaded =
        world.hot_reload_scene(loaded.handle, manifest);
    CHECK(!reloaded);
    CHECK(reloaded.error.code == RuntimeSceneErrorCode::RendererPublicationFailure);
    CHECK(world.scene_count() == 1);
    CHECK(world.state_hash() == beforeWorldHash);
    CHECK(world.scene(loaded.handle)->state_hash() == beforeSceneHash);
    CHECK(renderer.object_count() == 3);
    CHECK(physics.body_count() == 3);
    const RuntimeSceneObject* restoredObject = world.scene(loaded.handle)->find_object(1003);
    CHECK(restoredObject != nullptr);
    const std::optional<RigidBodyState> restored = physics.state(restoredObject->body_handle());
    CHECK(restored.has_value());
    check_distinct_body_state(*restored);

    CHECK(world.unload_scene(loaded.handle));
    std::filesystem::remove_all(temp);
}


void test_async_deferred_staging_and_stale_rejection() {
    using namespace dve;
    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    RuntimeSceneLoadOptions options;
    options.loadAllObjects = false;
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest, options);
    CHECK(loaded);
    CHECK(world.loaded_object_count() == 0U);

    std::future<RuntimeDeferredObjectStageResult> future =
        world.stage_deferred_object_async(loaded.handle, 1002);
    RuntimeDeferredObjectStageResult staged = future.get();
    CHECK(staged);
    CHECK(staged.staging.object_id() == 1002U);
    CHECK(staged.staging.estimated_bytes() > 0U);

    // A competing publication makes the older background result stale.
    RuntimeSceneError error;
    CHECK(world.load_deferred_object(loaded.handle, 1002, &error));
    const std::size_t bodiesBefore = physics.body_count();
    const std::size_t rendererBefore = renderer.object_count();
    CHECK(!world.publish_staged_deferred_object(std::move(staged.staging), &error));
    CHECK(error.code == RuntimeSceneErrorCode::InvalidStaging);
    CHECK(physics.body_count() == bodiesBefore);
    CHECK(renderer.object_count() == rendererBefore);

    CHECK(world.unload_object(loaded.handle, 1002, &error));
    RuntimeDeferredObjectStageResult stagedAgain =
        world.stage_deferred_object_async(loaded.handle, 1002).get();
    CHECK(stagedAgain);
    CHECK(world.publish_staged_deferred_object(std::move(stagedAgain.staging), &error));
    CHECK(world.scene(loaded.handle)->find_object(1002)->loaded());

    RuntimeSceneCancellationToken cancelled;
    cancelled.request_cancel();
    RuntimeSceneStageOptions cancelledOptions;
    cancelledOptions.cancellation = cancelled;
    CHECK(world.unload_object(loaded.handle, 1002, &error));
    RuntimeDeferredObjectStageResult cancelledResult =
        world.stage_deferred_object_async(loaded.handle, 1002, cancelledOptions).get();
    CHECK(!cancelledResult);
    CHECK(cancelledResult.error.code == RuntimeSceneErrorCode::Cancelled);
    CHECK(!world.scene(loaded.handle)->find_object(1002)->loaded());
    CHECK(world.unload_scene(loaded.handle));
}


void test_hot_reload_invalidates_deferred_staging() {
    using namespace dve;
    const std::filesystem::path temp = make_temp_dir();
    copy_house_package(temp);
    const std::filesystem::path manifest = temp / "multi_object_house.dvoxscene.json";
    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    RuntimeSceneLoadOptions options;
    options.loadAllObjects = false;
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest, options);
    CHECK(loaded);

    RuntimeDeferredObjectStageResult staged =
        world.stage_deferred_object_async(loaded.handle, 1002).get();
    CHECK(staged);
    const std::uint64_t generationBefore =
        world.scene(loaded.handle)->find_object(1002)->residency_generation();
    const RuntimeSceneHotReloadResult reloaded =
        world.hot_reload_scene(loaded.handle, manifest);
    CHECK(reloaded);
    CHECK(world.scene(loaded.handle)->find_object(1002)->residency_generation() > generationBefore);

    RuntimeSceneError error;
    CHECK(!world.publish_staged_deferred_object(std::move(staged.staging), &error));
    CHECK(error.code == RuntimeSceneErrorCode::InvalidStaging);
    CHECK(!world.scene(loaded.handle)->find_object(1002)->loaded());
    CHECK(world.unload_scene(loaded.handle));
    std::filesystem::remove_all(temp);
}


void test_persistent_executor_shutdown_cancels_jobs() {
    using namespace dve;
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    std::promise<void> startedPromise;
    std::future<void> started = startedPromise.get_future();
    std::atomic_bool signalled{false};
    std::future<RuntimeSceneStageResult> running;
    std::future<RuntimeSceneStageResult> queued;
    {
        RuntimeSceneStagingExecutor executor(1U, 64ULL * 1024ULL * 1024ULL);
        RuntimeSceneStageOptions options;
        options.progress = [&](const RuntimeSceneProgress& progress) {
            if (progress.phase == RuntimeSceneLoadPhase::Manifest && progress.completed == 0U &&
                !signalled.exchange(true)) {
                startedPromise.set_value();
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        };
        running = executor.submit_scene(manifest, {}, options);
        queued = executor.submit_scene(manifest);
        started.wait();
    }
    RuntimeSceneStageResult runningResult = running.get();
    RuntimeSceneStageResult queuedResult = queued.get();
    CHECK(!runningResult);
    CHECK(!queuedResult);
    CHECK(runningResult.error.code == RuntimeSceneErrorCode::Cancelled);
    CHECK(queuedResult.error.code == RuntimeSceneErrorCode::Cancelled);
}

void test_persistent_staging_executor() {
    using namespace dve;
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";

    ReferenceRigidBodyWorld physics;
    ReferenceRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld streamingWorld(&physics, &renderer);
    RuntimeSceneLoadOptions deferredOptions;
    deferredOptions.loadAllObjects = false;
    const RuntimeSceneLoadResult base = streamingWorld.load_scene_package(manifest, deferredOptions);
    CHECK(base);

    RuntimeSceneStagingExecutor executor(2U, 64ULL * 1024ULL * 1024ULL);
    std::future<RuntimeSceneStageResult> sceneFuture = executor.submit_scene(manifest);
    std::future<RuntimeDeferredObjectStageResult> deferredFuture =
        executor.submit_deferred_object(streamingWorld, base.handle, 1003);

    RuntimeSceneStageResult sceneStage = sceneFuture.get();
    RuntimeDeferredObjectStageResult deferredStage = deferredFuture.get();
    CHECK(sceneStage);
    CHECK(deferredStage);
    RuntimeSceneStagingExecutorStats heldStats = executor.stats();
    CHECK(heldStats.submittedSceneJobs == 1U);
    CHECK(heldStats.submittedDeferredJobs == 1U);
    CHECK(heldStats.completedJobs == 2U);
    CHECK(heldStats.failedJobs == 0U);
    CHECK(heldStats.peakRetainedStagingBytes >= sceneStage.staging.estimated_bytes());
    CHECK(heldStats.retainedStagingBytes >= deferredStage.staging.estimated_bytes());

    RuntimeSceneError error;
    CHECK(streamingWorld.publish_staged_deferred_object(std::move(deferredStage.staging), &error));
    CHECK(streamingWorld.scene(base.handle)->find_object(1003)->loaded());

    ReferenceRigidBodyWorld secondPhysics;
    ReferenceRuntimeBrickmapWorld secondRenderer;
    RuntimeSceneWorld publicationWorld(&secondPhysics, &secondRenderer);
    const RuntimeSceneLoadResult published =
        publicationWorld.publish_staged_scene(std::move(sceneStage.staging));
    CHECK(published);
    CHECK(publicationWorld.unload_scene(published.handle));
    CHECK(streamingWorld.unload_scene(base.handle));

    // Retained-memory accounting is released with the staging objects.
    CHECK(executor.stats().retainedStagingBytes == 0U);

    RuntimeSceneStagingExecutor tinyExecutor(1U, 1U);
    RuntimeSceneStageResult rejected = tinyExecutor.submit_scene(manifest).get();
    CHECK(!rejected);
    CHECK(rejected.error.code == RuntimeSceneErrorCode::StagingMemoryExceeded);
    const RuntimeSceneStagingExecutorStats tinyStats = tinyExecutor.stats();
    CHECK(tinyStats.completedJobs == 1U);
    CHECK(tinyStats.failedJobs == 1U);
}


void test_persistent_byte_heap_and_brickmap_publication() {
    using namespace dve;
    PersistentByteHeap heap(1024U, 16U);
    const auto a = heap.allocate(100U, 16U);
    const auto b = heap.allocate(200U, 64U);
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK((a->offset % 16U) == 0U);
    CHECK((b->offset % 64U) == 0U);
    const PersistentAllocationHandle stale = a->handle;
    CHECK(heap.release(a->handle));
    const auto c = heap.allocate(80U, 16U);
    CHECK(c.has_value());
    CHECK(c->handle.slot == stale.slot);
    CHECK(c->handle.generation != stale.generation);
    CHECK(!heap.release(stale));
    CHECK(heap.release(c->handle));
    CHECK(heap.release(b->handle));
    const PersistentByteHeapStats heapStats = heap.stats();
    CHECK(heapStats.liveAllocations == 0U);
    CHECK(heapStats.freeBytes == 1024U);
    CHECK(heapStats.largestFreeSpan == 1024U);
    CHECK(heapStats.externalFragmentation == 0.0);

    const auto assetPath = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" / "unit_cube.dvox";
    DvoxReadResult read = read_dvox(assetPath);
    CHECK(read.success);
    PackedBrickmapScene scene;
    scene.rebuild(read.asset.object);

    PersistentRuntimeBrickmapWorldConfig config;
    config.indexHeapBytes = 1024U * 1024U;
    config.recordHeapBytes = 1024U * 1024U;
    config.materialHeapBytes = 1024U * 1024U;
    config.allocationAlignment = 64U;
    PersistentRuntimeBrickmapWorld renderer(config);
    RuntimeBrickmapCreateDesc desc{read.asset.object.id(), {}, &scene};
    const RuntimeBrickmapHandle handle = renderer.create_object(desc);
    CHECK(handle != kInvalidRuntimeBrickmapHandle);
    CHECK(renderer.readback_hash(handle) == scene.readback_hash());
    const RuntimeBrickmapUploadStats initial = *renderer.last_upload_stats(handle);
    CHECK(initial.total_bytes_uploaded() > 0U);

    // A one-voxel material edit keeps all allocations stable and uploads only changed bytes.
    CHECK(read.asset.object.set_voxel({0, 0, 0}, kAirMaterial).changed);
    PackedBrickmapScene edited;
    edited.rebuild(read.asset.object);
    RuntimeBrickmapUpdateDesc update{read.asset.object.id(), {}, &edited};
    CHECK(renderer.update_object(handle, update));
    const RuntimeBrickmapUploadStats small = *renderer.last_upload_stats(handle);
    const std::size_t fullBytes = edited.index_grid().size_bytes() + edited.records().size_bytes() +
        edited.material_arena().size_bytes();
    CHECK(small.total_bytes_uploaded() < fullBytes);
    CHECK(small.relocatedAllocationCount == 0U);
    CHECK(renderer.readback_hash(handle) == edited.readback_hash());

    // Growing to an adjacent brick forces allocate-copy-swap while preserving the object handle.
    read.asset.object.fill_brick({1, 0, 0}, 1);
    PackedBrickmapScene grown;
    grown.rebuild(read.asset.object);
    update.packedScene = &grown;
    CHECK(renderer.update_object(handle, update));
    const RuntimeBrickmapUploadStats growth = *renderer.last_upload_stats(handle);
    CHECK(growth.relocatedAllocationCount >= 1U);
    CHECK(renderer.object_id(handle) == read.asset.object.id());
    CHECK(renderer.readback_hash(handle) == grown.readback_hash());
    CHECK(renderer.stats().totalRelocations >= 1U);
    CHECK(renderer.destroy_object(handle));
    const PersistentRuntimeBrickmapWorldStats finalStats = renderer.stats();
    CHECK(finalStats.live.objects == 0U);
    CHECK(finalStats.indexHeap.liveAllocations == 0U);
    CHECK(finalStats.recordHeap.liveAllocations == 0U);
    CHECK(finalStats.materialHeap.liveAllocations == 0U);
}

void test_priority_deadline_backpressure_and_queue_limit() {
    using namespace dve;
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";

    RuntimeSceneStagingExecutorConfig config;
    config.workerCount = 1U;
    config.maximumRetainedStagingBytes = 64ULL * 1024ULL * 1024ULL;
    config.maximumPendingJobs = 8U;
    config.publicationBackpressureBytes = 1U;
    RuntimeSceneStagingExecutor executor(config);

    std::promise<void> startedPromise;
    std::future<void> started = startedPromise.get_future();
    std::atomic_bool signalled{false};
    RuntimeSceneStageOptions blockerOptions;
    blockerOptions.progress = [&](const RuntimeSceneProgress& progress) {
        if (progress.phase == RuntimeSceneLoadPhase::Manifest && !signalled.exchange(true)) {
            startedPromise.set_value();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    };
    auto blocker = executor.submit_scene(manifest, {}, blockerOptions,
        {RuntimeSceneJobPriority::Normal, std::nullopt});
    started.wait();

    auto background = executor.submit_scene(manifest, {}, {},
        {RuntimeSceneJobPriority::Background, std::nullopt});
    auto critical = executor.submit_scene(manifest, {}, {},
        {RuntimeSceneJobPriority::Critical, std::nullopt});
    auto expired = executor.submit_scene(manifest, {}, {},
        {RuntimeSceneJobPriority::High, std::chrono::steady_clock::now() - std::chrono::milliseconds(1)});

    RuntimeSceneStageResult blockerResult = blocker.get();
    CHECK(blockerResult);
    // The completed blocker owns retained memory, so no queued work can start yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(critical.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
    blockerResult.staging = {};

    CHECK(critical.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    RuntimeSceneStageResult criticalResult = critical.get();
    CHECK(criticalResult);
    CHECK(background.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
    criticalResult.staging = {};

    RuntimeSceneStageResult expiredResult = expired.get();
    CHECK(!expiredResult);
    CHECK(expiredResult.error.code == RuntimeSceneErrorCode::DeadlineExceeded);
    RuntimeSceneStageResult backgroundResult = background.get();
    CHECK(backgroundResult);
    backgroundResult.staging = {};

    const RuntimeSceneStagingExecutorStats stats = executor.stats();
    CHECK(stats.submittedByPriority[static_cast<std::size_t>(RuntimeSceneJobPriority::Critical)] == 1U);
    CHECK(stats.deadlineExpiredJobs == 1U);
    CHECK(stats.backpressureWaitCount >= 1U);
    CHECK(stats.maximumQueueWaitMicroseconds > 0U);
    CHECK(stats.maximumStageMicroseconds > 0U);

    RuntimeSceneStagingExecutorConfig limitedConfig;
    limitedConfig.workerCount = 1U;
    limitedConfig.maximumRetainedStagingBytes = 64ULL * 1024ULL * 1024ULL;
    limitedConfig.maximumPendingJobs = 1U;
    RuntimeSceneStagingExecutor limited(limitedConfig);
    std::promise<void> limitedStartedPromise;
    auto limitedStarted = limitedStartedPromise.get_future();
    std::atomic_bool limitedSignalled{false};
    RuntimeSceneStageOptions slow;
    slow.progress = [&](const RuntimeSceneProgress& progress) {
        if (progress.phase == RuntimeSceneLoadPhase::Manifest && !limitedSignalled.exchange(true)) {
            limitedStartedPromise.set_value();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };
    auto running = limited.submit_scene(manifest, {}, slow);
    limitedStarted.wait();
    auto queued = limited.submit_scene(manifest);
    auto rejected = limited.submit_scene(manifest);
    RuntimeSceneStageResult rejectedResult = rejected.get();
    CHECK(!rejectedResult);
    CHECK(rejectedResult.error.code == RuntimeSceneErrorCode::QueueFull);
    RuntimeSceneStageResult runningResult = running.get();
    CHECK(runningResult);
    runningResult.staging = {};
    RuntimeSceneStageResult queuedResult = queued.get();
    CHECK(queuedResult);
    queuedResult.staging = {};
    CHECK(limited.stats().queueRejectedJobs == 1U);
}

void test_authority_publication_queue_priority() {
    using namespace dve;
    const auto manifest = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples" /
        "multi_object_house.dvoxscene.json";
    ReferenceRigidBodyWorld physics;
    PersistentRuntimeBrickmapWorld renderer;
    RuntimeSceneWorld world(&physics, &renderer);
    RuntimeSceneLoadOptions options;
    options.loadAllObjects = false;
    options.initiallyLoadedObjectIds = {1001};
    const RuntimeSceneLoadResult loaded = world.load_scene_package(manifest, options);
    CHECK(loaded);

    RuntimeDeferredObjectStageResult furniture = world.stage_deferred_object(loaded.handle, 1003);
    RuntimeDeferredObjectStageResult upper = world.stage_deferred_object(loaded.handle, 1002);
    CHECK(furniture);
    CHECK(upper);

    RuntimeScenePublicationQueueConfig queueConfig;
    queueConfig.maximumPendingPublications = 4U;
    queueConfig.maximumRetainedBytes = 64ULL * 1024ULL * 1024ULL;
    RuntimeScenePublicationQueue queue(queueConfig);
    CHECK(queue.enqueue_deferred_object(std::move(furniture),
        {RuntimeSceneJobPriority::Background, std::nullopt}));
    CHECK(queue.enqueue_deferred_object(std::move(upper),
        {RuntimeSceneJobPriority::Critical, std::nullopt}));

    auto first = queue.publish_next(world);
    CHECK(first.has_value());
    CHECK(*first);
    CHECK(first->objectId == 1002U);
    CHECK(world.scene(loaded.handle)->find_object(1002)->loaded());
    CHECK(!world.scene(loaded.handle)->find_object(1003)->loaded());

    auto second = queue.publish_next(world);
    CHECK(second.has_value());
    CHECK(*second);
    CHECK(second->objectId == 1003U);
    CHECK(world.scene(loaded.handle)->find_object(1003)->loaded());
    CHECK(queue.stats().published == 2U);
    CHECK(queue.stats().pending == 0U);
    CHECK(world.unload_scene(loaded.handle));
}

} // namespace

int main() {
    try {
        test_valid_transaction_and_unload();
        test_deferred_loading_preserves_ids();
        test_rigid_matrix_rotation_and_translation();
        test_manifest_rejections_are_transactional();
        test_corrupt_dvox_and_duplicate_world_ids();
        test_physics_publication_rolls_back();
        test_voxel_size_policy_and_uint64_string_ids();
        test_deferred_asset_size_is_rechecked();
        test_world_destructor_releases_bodies();
        test_unknown_deferred_selection();
        test_renderer_publication_and_deferred_slot_reuse();
        test_async_staging_is_deterministic_across_worker_counts();
        test_cancellation_and_staging_memory_leave_world_unchanged();
        test_wrong_thread_publication_is_rejected();
        test_renderer_publication_rolls_back();
        test_self_contained_checkpoint_restores_loaded_state_and_bodies();
        test_checkpoint_corruption_and_metadata_changes_are_transactional();
        test_material_only_hot_reload_preserves_body_state();
        test_persistent_renderer_hot_reload_keeps_handles_and_uses_updates();
        test_hot_reload_rejects_topology_change_without_opt_in();
        test_hot_reload_allows_topology_change_with_opt_in();
        test_hot_reload_backend_failure_restores_old_scene();
        test_async_deferred_staging_and_stale_rejection();
        test_hot_reload_invalidates_deferred_staging();
        test_persistent_executor_shutdown_cancels_jobs();
        test_persistent_staging_executor();
        test_persistent_byte_heap_and_brickmap_publication();
        test_priority_deadline_backpressure_and_queue_limit();
        test_authority_publication_queue_priority();
        std::cout << "dve_runtime_scene_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_runtime_scene_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
