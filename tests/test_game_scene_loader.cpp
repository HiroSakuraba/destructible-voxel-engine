// Runtime scene loading into GameWorld from a ContentSource, plus in-memory loader parity:
//  - read_dvox/read_dmesh(bytes) == read_dvox/read_dmesh(path) (results and errors)
//  - parse_dvoxscene_manifest(text) == RuntimeSceneWorld's path parser (metadata and errors)
//  - the same scene from a loose folder and from a .dvepak gives identical GameWorld state
//  - pak integrity failures, missing assets and validation errors fail cleanly (no objects)
//  - GameWorld::render_objects() exposes voxels/materials/transforms read-only
//  - the versioned per-object "extensions" block (components, attachment, polygon geometry):
//    round trip, strict validation, RuntimeSceneWorld rejecting polygon objects
//  - attachChildrenToParents policy and attached bodies riding their parents (Jolt: an
//    attachment overlapping its parent does not push it)

#include "dve/content_source.hpp"
#include "dve/dvox.hpp"
#include "dve/game_scene_loader.hpp"
#include "dve/game_world.hpp"
#include "dve/geometry_build.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/runtime_scene.hpp"
#include "dve/v235_foundations.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <variant>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

const std::filesystem::path kExamples = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples";
const char* const kHouseFiles[] = {"multi_object_house.dvoxscene.json", "multi_object_house_Foundation_3e9.dvox",
                                   "multi_object_house_UpperBlock_3ea.dvox", "multi_object_house_Furniture_3eb.dvox"};
constexpr const char* kScene = "scenes/multi_object_house.dvoxscene.json";

std::filesystem::path make_temp_dir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / (std::string("dve_game_scene_loader_") + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::transform(chars.begin(), chars.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

std::string read_string(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_string(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) { std::cerr << "replace source missing: " << from << "\n"; ++failures; return; }
    text.replace(at, from.size(), to);
}

std::uint64_t voxel_hash(const dve::VoxelObject& object) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) { for (int i = 0; i < 8; ++i) { hash ^= (v >> (i * 8)) & 0xFFU; hash *= 1099511628211ULL; } };
    for (const auto& [key, brick] : object.bricks()) {
        mix(static_cast<std::uint32_t>(key.x)); mix(static_cast<std::uint32_t>(key.y)); mix(static_cast<std::uint32_t>(key.z));
        for (const dve::MaterialId m : brick.materials()) mix(m);
    }
    return hash;
}

std::filesystem::path make_project(const std::filesystem::path& base, const std::string& manifestOverride = {}) {
    const auto root = base / "project";
    std::filesystem::create_directories(root / "scenes");
    for (const char* file : kHouseFiles) std::filesystem::copy_file(kExamples / file, root / "scenes" / file);
    if (!manifestOverride.empty()) write_string(root / kScene, manifestOverride);
    write_string(root / "game.dvegame", "DVE_GAME 1\nname=House\nversion=1\nentryScene=scenes/multi_object_house.dvoxscene.json\n");
    write_string(root / ".autosave" / "stale.dvescene", "autosave");
    return root;
}

std::filesystem::path make_pak(const std::filesystem::path& root, const std::filesystem::path& output) {
    std::vector<std::filesystem::path> inputs;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) inputs.push_back(std::filesystem::relative(entry.path(), root));
    }
    std::string error;
    dve::DvePakManifest manifest;
    CHECK(dve::build_dvepak(root, inputs, output, {}, &manifest, &error));
    return output;
}

std::unique_ptr<dve::GameWorld> make_world() {
    return std::make_unique<dve::GameWorld>(dve::create_physics3d_world(dve::Physics3DBackend::Reference));
}

bool same_transform(const dve::RigidTransform& a, const dve::RigidTransform& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
           a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w;
}

// Full observable GameWorld state that a renderer or script could see.
struct WorldSnapshot {
    std::vector<dve::GameObjectId> ids;
    std::vector<std::string> names;
    std::vector<dve::RigidTransform> transforms;
    std::vector<std::uint64_t> voxelCounts;
    std::vector<std::uint64_t> voxelHashes;
    std::vector<std::size_t> materialCounts;
    std::vector<bool> dynamic;
};

WorldSnapshot snapshot(const dve::GameWorld& world) {
    WorldSnapshot s;
    for (const dve::GameRenderObject& object : world.render_objects()) {
        s.ids.push_back(object.id);
        s.names.push_back(*object.name);
        s.transforms.push_back(object.transform);
        s.voxelCounts.push_back(world.voxel_count(object.id).value_or(0U));
        s.voxelHashes.push_back(object.voxels ? voxel_hash(*object.voxels) : 0U);
        s.materialCounts.push_back(object.materials.size());
        s.dynamic.push_back(object.dynamic);
    }
    return s;
}

bool equal(const WorldSnapshot& a, const WorldSnapshot& b) {
    if (a.ids != b.ids || a.names != b.names || a.voxelCounts != b.voxelCounts || a.voxelHashes != b.voxelHashes ||
        a.materialCounts != b.materialCounts || a.dynamic != b.dynamic || a.transforms.size() != b.transforms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.transforms.size(); ++i) if (!same_transform(a.transforms[i], b.transforms[i])) return false;
    return true;
}

// ---------------------------------------------------------------------------------------

void test_dvox_memory_matches_path() {
    for (const char* file : {"multi_object_house_Foundation_3e9.dvox", "multi_object_house_UpperBlock_3ea.dvox",
                             "multi_object_house_Furniture_3eb.dvox", "unit_cube.dvox", "textured_checker.dvox"}) {
        const auto path = kExamples / file;
        const auto bytes = read_bytes(path);
        const dve::DvoxReadResult a = dve::read_dvox(path);
        const dve::DvoxReadResult b = dve::read_dvox(bytes);
        CHECK(a.success && b.success);
        if (!a.success || !b.success) continue;
        CHECK(a.asset.object.id() == b.asset.object.id());
        CHECK(a.asset.voxelSizeMeters == b.asset.voxelSizeMeters);
        CHECK(a.asset.materials.size() == b.asset.materials.size());
        CHECK(a.asset.object.occupied_voxel_count() == b.asset.object.occupied_voxel_count());
        CHECK(a.asset.object.brick_count() == b.asset.object.brick_count());
        CHECK(voxel_hash(a.asset.object) == voxel_hash(b.asset.object));
        for (std::size_t i = 0; i < a.asset.materials.size(); ++i) {
            CHECK(a.asset.materials[i].name == b.asset.materials[i].name);
            CHECK(a.asset.materials[i].densityKilogramsPerCubicMeter == b.asset.materials[i].densityKilogramsPerCubicMeter);
        }
        // Errors match too: size limit, truncation, corruption.
        const dve::DvoxReadResult limitedPath = dve::read_dvox(path, bytes.size() - 1U);
        const dve::DvoxReadResult limitedBytes = dve::read_dvox(bytes, bytes.size() - 1U);
        CHECK(!limitedPath.success && !limitedBytes.success && limitedPath.error == limitedBytes.error);
        CHECK(dve::read_dvox(bytes, bytes.size()).success);
        const std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2U));
        const auto temp = make_temp_dir("dvox");
        { std::ofstream out(temp / "t.dvox", std::ios::binary); out.write(reinterpret_cast<const char*>(truncated.data()), static_cast<std::streamsize>(truncated.size())); }
        const dve::DvoxReadResult truncatedPath = dve::read_dvox(temp / "t.dvox");
        const dve::DvoxReadResult truncatedBytes = dve::read_dvox(truncated);
        CHECK(!truncatedPath.success && !truncatedBytes.success && truncatedPath.error == truncatedBytes.error);
        std::vector<std::byte> corrupt = bytes;
        corrupt.back() ^= std::byte{0x40};
        const dve::DvoxReadResult corruptBytes = dve::read_dvox(corrupt);
        CHECK(!corruptBytes.success && !corruptBytes.error.empty());
        std::error_code ec;
        std::filesystem::remove_all(temp, ec);
    }
    CHECK(!dve::read_dvox(std::span<const std::byte>{}).success);
}

void test_dmesh_memory_matches_path() {
    dve::CookedPolygonAsset asset;
    asset.objectId = 77U;
    dve::VoxelMaterialDefinition material;
    material.name = "plain";
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.vertices = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
                      {{1, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
                      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 1}, {1, 1, 1, 1}}};
    asset.indices = {0U, 1U, 2U};
    asset.submeshes.push_back({"tri", 0U, 3U, 0U});
    asset.bounds = {{0, 0, 0}, {1, 1, 0}};
    asset.contentHash = dve::polygon_asset_content_hash(asset);
    const auto temp = make_temp_dir("dmesh");
    const auto path = temp / "tri.dmesh";
    std::string error;
    CHECK(dve::write_dmesh(path, asset, &error));
    const auto bytes = read_bytes(path);
    const dve::PolygonAssetReadResult a = dve::read_dmesh(path);
    const dve::PolygonAssetReadResult b = dve::read_dmesh(bytes);
    CHECK(static_cast<bool>(a) && static_cast<bool>(b));
    CHECK(a.asset.objectId == b.asset.objectId && a.asset.contentHash == b.asset.contentHash);
    CHECK(a.asset.vertices.size() == b.asset.vertices.size() && a.asset.indices == b.asset.indices);
    const dve::PolygonAssetReadResult limitedA = dve::read_dmesh(path, bytes.size() - 1U);
    const dve::PolygonAssetReadResult limitedB = dve::read_dmesh(bytes, bytes.size() - 1U);
    CHECK(!limitedA && !limitedB && limitedA.error == limitedB.error);
    const std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + 20);
    CHECK(!dve::read_dmesh(truncated));

    // spawn_asset_from_bytes(".dmesh") and spawn_polygon_asset(path) agree (success or error).
    if (dve::geometry_kind_supported(dve::GeometryKind::Polygon)) {
        auto byPath = make_world();
        auto byBytes = make_world();
        std::string pathError, bytesError;
        const auto idA = byPath->spawn_polygon_asset(path, "tri", {}, false, true, &pathError);
        const auto idB = byBytes->spawn_asset_from_bytes(bytes, ".dmesh", "tri", {}, false, true, &bytesError);
        CHECK((idA == dve::kInvalidGameObjectId) == (idB == dve::kInvalidGameObjectId));
        CHECK(pathError == bytesError);
        if (idA != dve::kInvalidGameObjectId && idB != dve::kInvalidGameObjectId) {
            CHECK(byPath->geometry_kind(idA) == byBytes->geometry_kind(idB));
            const auto render = byBytes->render_objects();
            CHECK(render.size() == 1U && render[0].polygon != nullptr && render[0].voxels == nullptr);
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

void test_spawn_from_bytes_matches_path() {
    const auto path = kExamples / "multi_object_house_UpperBlock_3ea.dvox";
    const auto bytes = read_bytes(path);
    auto byPath = make_world();
    auto byBytes = make_world();
    const auto transform = dve::make_rigid_transform({1.0F, 2.0F, 3.0F}, {});
    std::string error;
    const auto a = byPath->spawn_asset(path, "", transform, true, true, &error);
    const auto b = byBytes->spawn_asset_from_bytes(bytes, ".dvox", "multi_object_house_UpperBlock_3ea", transform, true, true, &error);
    CHECK(a != dve::kInvalidGameObjectId && b != dve::kInvalidGameObjectId);
    CHECK(equal(snapshot(*byPath), snapshot(*byBytes)));
    for (int i = 0; i < 30; ++i) { byPath->tick(1.0F / 60.0F); byBytes->tick(1.0F / 60.0F); }
    CHECK(equal(snapshot(*byPath), snapshot(*byBytes)));
    // Default name for byte spawns; unsupported extension and corrupt bytes are rejected.
    const auto c = byBytes->spawn_asset_from_bytes(bytes, ".dvox", "", transform, false, true, &error);
    CHECK(c != dve::kInvalidGameObjectId && byBytes->name_of(c) == "asset");
    CHECK(byBytes->spawn_asset_from_bytes(bytes, ".png", "x", transform, false, true, &error) == dve::kInvalidGameObjectId);
    CHECK(error.find("unsupported asset extension") != std::string::npos);
    std::vector<std::byte> corrupt(bytes.begin(), bytes.begin() + 40);
    CHECK(byBytes->spawn_asset_from_bytes(corrupt, ".dvox", "x", transform, false, true, &error) == dve::kInvalidGameObjectId);
    CHECK(error.find("DVOX") != std::string::npos);
}

void test_manifest_memory_matches_path() {
    const std::string original = read_string(kExamples / kHouseFiles[0]);
    // Metadata parity with the RuntimeSceneWorld path parser.
    {
        dve::RuntimeSceneError error;
        const auto parsed = dve::parse_dvoxscene_manifest(original, {}, &error);
        CHECK(parsed.has_value());
        dve::ReferenceRigidBodyWorld physics;
        dve::RuntimeSceneWorld runtime(&physics);
        const auto loaded = runtime.load_scene_package(kExamples / kHouseFiles[0]);
        CHECK(static_cast<bool>(loaded));
        if (parsed && loaded) {
            CHECK(parsed->name == "MultiObjectHouse" && runtime.scene(loaded.handle)->name() == parsed->name);
            const auto objects = runtime.scene(loaded.handle)->objects();
            CHECK(parsed->objects.size() == objects.size());
            for (std::size_t i = 0; i < std::min(objects.size(), parsed->objects.size()); ++i) {
                const auto& a = parsed->objects[i];
                const auto& b = objects[i].metadata();
                CHECK(a.index == b.index && a.id == b.id && a.name == b.name && a.nodePath == b.nodePath);
                CHECK(a.relativeFile == b.relativeFile && a.parentIndex == b.parentIndex && a.parentId == b.parentId);
                CHECK(a.anchored == b.anchored && a.structural == b.structural && a.generateCollision == b.generateCollision);
                CHECK(same_transform(a.worldTransform, b.worldTransform));
            }
        }
    }
    // Error-code parity for malformed manifests (package folder has all referenced files).
    struct Case { const char* from; const char* to; dve::RuntimeSceneErrorCode code; };
    const Case cases[] = {
        {"\"version\": 1", "\"version\": 2", dve::RuntimeSceneErrorCode::UnsupportedVersion},
        {"\"format\": \"DVOXSCENE\"", "\"format\": \"NOPE\"", dve::RuntimeSceneErrorCode::InvalidManifest},
        {"\"name\": \"MultiObjectHouse\"", "\"name\": \"MultiObjectHouse\", \"extra\": 1", dve::RuntimeSceneErrorCode::InvalidManifest},
        {"\"id\":1002", "\"id\":1001", dve::RuntimeSceneErrorCode::DuplicateObjectId},
        {"\"index\":2", "\"index\":7", dve::RuntimeSceneErrorCode::DuplicateObjectIndex},
        {"\"parent\":0", "\"parent\":9", dve::RuntimeSceneErrorCode::InvalidParent},
        {"\"parent\":null", "\"parent\":2", dve::RuntimeSceneErrorCode::CyclicHierarchy},
        {"\"file\":\"multi_object_house_Furniture_3eb.dvox\"", "\"file\":\"../escape.dvox\"", dve::RuntimeSceneErrorCode::PathEscape},
        {"\"file\":\"multi_object_house_Furniture_3eb.dvox\"", "\"file\":\"multi_object_house_Furniture_3eb.txt\"", dve::RuntimeSceneErrorCode::InvalidManifest},
        {"\"file\":\"multi_object_house_Furniture_3eb.dvox\"", "\"file\":\"multi_object_house_UpperBlock_3ea.dvox\"", dve::RuntimeSceneErrorCode::DuplicateAssetPath},
        {"\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,3,2,0,1]", "\"worldMatrix\":[2,0,0,0,0,1,0,0,0,0,1,0,3,2,0,1]", dve::RuntimeSceneErrorCode::InvalidTransform},
        {"\"objects\": [", "\"objects\": [,", dve::RuntimeSceneErrorCode::Json},
    };
    const auto temp = make_temp_dir("manifest");
    for (const char* file : kHouseFiles) std::filesystem::copy_file(kExamples / file, temp / file);
    for (const Case& c : cases) {
        std::string text = original;
        replace_once(text, c.from, c.to);
        write_string(temp / kHouseFiles[0], text);
        dve::RuntimeSceneError memoryError;
        CHECK(!dve::parse_dvoxscene_manifest(text, {}, &memoryError));
        dve::RuntimeSceneWorld runtime;
        const auto staged = runtime.stage_scene_package(temp / kHouseFiles[0]);
        CHECK(!staged);
        if (memoryError.code != c.code || staged.error.code != c.code) {
            std::cerr << "FAIL manifest case '" << c.to << "': memory=" << dve::to_string(memoryError.code)
                      << " path=" << dve::to_string(staged.error.code) << " expected=" << dve::to_string(c.code) << "\n";
            ++failures;
        }
    }
    dve::RuntimeSceneLoadOptions tiny;
    tiny.maximumManifestBytes = 16U;
    dve::RuntimeSceneError limitError;
    CHECK(!dve::parse_dvoxscene_manifest(original, tiny, &limitError));
    CHECK(limitError.code == dve::RuntimeSceneErrorCode::LimitExceeded);
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

void test_loose_and_pak_give_identical_worlds() {
    const auto base = make_temp_dir("parity");
    const auto root = make_project(base);
    const auto pakPath = make_pak(root, base / "game.dvepak");
    std::string error;
    auto loose = dve::LooseContentSource::open(root, &error);
    auto pak = dve::PakContentSource::open(pakPath, &error);
    CHECK(loose && pak);
    if (!loose || !pak) return;

    auto looseWorld = make_world();
    auto pakWorld = make_world();
    const dve::GameSceneLoadResult a = dve::load_scene_into_game_world(*loose, kScene, *looseWorld);
    const dve::GameSceneLoadResult b = dve::load_scene_into_game_world(*pak, kScene, *pakWorld);
    if (!a) std::cerr << "loose load: " << a.error.message << "\n";
    if (!b) std::cerr << "pak load: " << b.error.message << "\n";
    CHECK(a && b);
    CHECK(a.sceneName == "MultiObjectHouse" && b.sceneName == a.sceneName);
    CHECK(a.objects.size() == 3U && b.objects.size() == 3U);
    CHECK(looseWorld->object_count() == 3U && pakWorld->object_count() == 3U);
    const WorldSnapshot sa = snapshot(*looseWorld);
    const WorldSnapshot sb = snapshot(*pakWorld);
    CHECK(equal(sa, sb));
    CHECK(sa.ids == (std::vector<dve::GameObjectId>{1U, 2U, 3U}));
    CHECK(sa.names == (std::vector<std::string>{"Foundation", "UpperBlock", "Furniture"}));
    CHECK(sa.dynamic == (std::vector<bool>{false, true, true}));
    for (std::size_t i = 0; i < a.objects.size() && i < b.objects.size(); ++i) {
        CHECK(a.objects[i].gameObjectId == b.objects[i].gameObjectId);
        CHECK(a.objects[i].sceneObjectId == b.objects[i].sceneObjectId);
        CHECK(a.objects[i].assetPath == b.objects[i].assetPath);
        CHECK(a.objects[i].parentGameObjectId == b.objects[i].parentGameObjectId);
        CHECK(!a.objects[i].attached && a.objects[i].collision);
        CHECK(a.objects[i].assetPath.rfind("scenes/", 0) == 0);
    }
    CHECK(a.objects[0].sceneObjectId == 1001U && !a.objects[0].parentGameObjectId);
    CHECK(a.objects[1].parentGameObjectId == a.objects[0].gameObjectId);
    CHECK(a.objects[2].parentGameObjectId == a.objects[1].gameObjectId);
    CHECK(!looseWorld->parent_of(a.objects[1].gameObjectId)); // hierarchy is metadata by default

    // Same simulation from both sources.
    for (int i = 0; i < 90; ++i) { looseWorld->tick(1.0F / 60.0F); pakWorld->tick(1.0F / 60.0F); }
    CHECK(equal(snapshot(*looseWorld), snapshot(*pakWorld)));

    // Equivalent to spawning every object by path with the documented rules.
    auto manual = make_world();
    const auto parsed = dve::parse_dvoxscene_manifest(read_string(root / kScene));
    CHECK(parsed.has_value());
    if (parsed) {
        for (const auto& object : parsed->objects) {
            (void)manual->spawn_asset(root / "scenes" / object.relativeFile, object.name, object.worldTransform,
                                      !object.anchored, object.structural, &error);
        }
    }
    CHECK(equal(snapshot(*manual), sa));

    // render_objects(): read-only views agree with the per-id accessors.
    const auto render = pakWorld->render_objects();
    CHECK(render.size() == 3U);
    for (const dve::GameRenderObject& object : render) {
        CHECK(object.kind == dve::GameGeometryKind::Voxel);
        CHECK(object.voxels != nullptr && object.polygon == nullptr);
        CHECK(!object.materials.empty());
        CHECK(object.voxelSizeMeters > 0.0F && object.enabled);
        const auto transform = pakWorld->transform(object.id);
        CHECK(transform && same_transform(*transform, object.transform));
        CHECK(object.voxels->occupied_voxel_count() == pakWorld->voxel_count(object.id).value_or(0U));
    }
    // A marker object is listed with no geometry.
    dve::GameObjectDesc marker;
    marker.name = "spawn_point";
    const auto markerId = pakWorld->create_object(std::move(marker), &error);
    const auto withMarker = pakWorld->render_objects();
    CHECK(withMarker.size() == 4U && withMarker.back().id == markerId);
    CHECK(withMarker.back().kind == dve::GameGeometryKind::Marker && withMarker.back().voxels == nullptr);
    CHECK(withMarker.back().materials.empty());

    // Optional attachment mode mirrors EditorPlaySession's attachment pass.
    auto attachedWorld = make_world();
    dve::GameSceneLoadOptions attach;
    attach.attachChildrenToParents = true;
    const auto c = dve::load_scene_into_game_world(*pak, kScene, *attachedWorld, attach);
    CHECK(c);
    if (c) {
        CHECK(c.objects[1].attached && c.objects[2].attached && !c.objects[0].attached);
        CHECK(attachedWorld->parent_of(c.objects[1].gameObjectId) == c.objects[0].gameObjectId);
        CHECK(attachedWorld->parent_of(c.objects[2].gameObjectId) == c.objects[1].gameObjectId);
        const auto before = attachedWorld->transform(c.objects[2].gameObjectId);
        for (int i = 0; i < 30; ++i) attachedWorld->tick(1.0F / 60.0F);
        const auto after = attachedWorld->transform(c.objects[2].gameObjectId);
        CHECK(before && after && same_transform(*before, *after)); // held by the anchored root
    }
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

void test_failures_are_clean() {
    const auto base = make_temp_dir("failures");
    const auto root = make_project(base);
    const auto pakPath = make_pak(root, base / "game.dvepak");
    std::string error;

    // 1. Corrupted .dvox payload inside the pak: IntegrityFailure, nothing spawned.
    {
        const auto copy = base / "corrupt.dvepak";
        std::filesystem::copy_file(pakPath, copy);
        dve::DvePakMount mount;
        CHECK(mount.mount(copy, &error));
        const dve::DvePakEntry* entry = mount.find("scenes/multi_object_house_Furniture_3eb.dvox");
        CHECK(entry != nullptr);
        if (entry) {
            std::fstream file(copy, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(static_cast<std::streamoff>(entry->offset + entry->size - 1U));
            const char flipped = 0x7F;
            file.write(&flipped, 1);
        }
        auto pak = dve::PakContentSource::open(copy, &error);
        CHECK(pak != nullptr);
        if (pak) {
            auto world = make_world();
            const auto result = dve::load_scene_into_game_world(*pak, kScene, *world);
            CHECK(!result);
            CHECK(result.contentError.code == dve::ContentErrorCode::IntegrityFailure);
            CHECK(result.error.code == dve::RuntimeSceneErrorCode::DvoxReadFailed);
            CHECK(result.error.message.find("integrity") != std::string::npos);
            CHECK(result.error.message.find("Furniture") != std::string::npos);
            CHECK(result.error.objectId == std::optional<std::uint64_t>(1003U));
            CHECK(result.objects.empty());
            CHECK(world->object_count() == 0U);
        }
    }
    // 2. Corrupted scene manifest entry.
    {
        const auto copy = base / "corrupt_manifest.dvepak";
        std::filesystem::copy_file(pakPath, copy);
        dve::DvePakMount mount;
        CHECK(mount.mount(copy, &error));
        const dve::DvePakEntry* entry = mount.find(kScene);
        CHECK(entry != nullptr);
        if (entry) {
            std::fstream file(copy, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(static_cast<std::streamoff>(entry->offset + 2U));
            file.write("#", 1);
        }
        auto pak = dve::PakContentSource::open(copy, &error);
        if (pak) {
            auto world = make_world();
            const auto result = dve::load_scene_into_game_world(*pak, kScene, *world);
            CHECK(!result);
            CHECK(result.contentError.code == dve::ContentErrorCode::IntegrityFailure);
            CHECK(result.error.code == dve::RuntimeSceneErrorCode::Io);
            CHECK(world->object_count() == 0U);
        }
    }
    // 3. Missing scene / missing asset / invalid path / manifest-DVOX id mismatch / limits.
    {
        auto pak = dve::PakContentSource::open(pakPath, &error);
        auto world = make_world();
        auto missingScene = dve::load_scene_into_game_world(*pak, "scenes/nope.dvoxscene.json", *world);
        CHECK(!missingScene && missingScene.contentError.code == dve::ContentErrorCode::NotFound);
        auto escape = dve::load_scene_into_game_world(*pak, "../x.dvoxscene.json", *world);
        CHECK(!escape && escape.error.code == dve::RuntimeSceneErrorCode::PathEscape);
        dve::GameSceneLoadOptions tiny;
        tiny.limits.maximumDvoxBytesPerObject = 64U;
        auto limited = dve::load_scene_into_game_world(*pak, kScene, *world, tiny);
        CHECK(!limited && limited.error.code == dve::RuntimeSceneErrorCode::LimitExceeded);
        CHECK(world->object_count() == 0U);
    }
    {
        std::string text = read_string(kExamples / kHouseFiles[0]);
        replace_once(text, "\"file\":\"multi_object_house_Furniture_3eb.dvox\"", "\"file\":\"gone.dvox\"");
        const auto project = make_project(base / "missing", text);
        auto loose = dve::LooseContentSource::open(project, &error);
        auto world = make_world();
        const auto result = dve::load_scene_into_game_world(*loose, kScene, *world);
        CHECK(!result && result.error.code == dve::RuntimeSceneErrorCode::MissingAsset);
        CHECK(result.contentError.code == dve::ContentErrorCode::NotFound);
        CHECK(world->object_count() == 0U);
    }
    {
        std::string text = read_string(kExamples / kHouseFiles[0]);
        replace_once(text, "\"id\":1003", "\"id\":1099");
        const auto project = make_project(base / "mismatch", text);
        auto loose = dve::LooseContentSource::open(project, &error);
        auto world = make_world();
        const auto result = dve::load_scene_into_game_world(*loose, kScene, *world);
        CHECK(!result && result.error.code == dve::RuntimeSceneErrorCode::ObjectIdMismatch);
        CHECK(world->object_count() == 0U); // validated before the first spawn
    }
    // 4. generateCollision=false objects become visual-only voxel objects: rendered, no body.
    {
        std::string text = read_string(kExamples / kHouseFiles[0]);
        replace_once(text, "\"structural\":false,\"generateCollision\":true", "\"structural\":false,\"generateCollision\":false");
        const auto project = make_project(base / "nocollision", text);
        auto loose = dve::LooseContentSource::open(project, &error);
        auto world = make_world();
        const auto result = dve::load_scene_into_game_world(*loose, kScene, *world);
        CHECK(result);
        if (result) {
            CHECK(!result.objects[2].collision);
            const auto visualId = result.objects[2].gameObjectId;
            CHECK(world->geometry_kind(visualId) == dve::GameGeometryKind::Voxel);
            CHECK(world->has_collision(visualId) == false);
            CHECK(world->has_collision(result.objects[0].gameObjectId) == true);
            CHECK(world->object_count() == 3U);
            bool rendered = false;
            for (const dve::GameRenderObject& object : world->render_objects()) {
                if (object.id != visualId) continue;
                rendered = object.voxels != nullptr && !object.collision && !object.materials.empty();
            }
            CHECK(rendered);
            // Queries ignore it; it can be moved like a marker.
            const auto position = world->position(visualId);
            CHECK(position.has_value());
            if (position) {
                const dve::Float3 above{position->x + 0.05F, position->y + 5.0F, position->z + 0.05F};
                const auto hit = world->raycast(above, {0.0F, -1.0F, 0.0F}, 4.9F);
                CHECK(!hit || hit->objectId != visualId);
                CHECK(world->set_position(visualId, {position->x, position->y + 1.0F, position->z}));
            }
            world->tick(1.0F / 60.0F);
            CHECK(world->has_object(visualId));
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

} // namespace


// ---------------------------------------------------------------------------------------

bool same_properties(const std::map<std::string, dve::ComponentValue, std::less<>>& a,
                     const std::map<std::string, dve::ComponentValue, std::less<>>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [name, value] : a) {
        const auto it = b.find(name);
        if (it == b.end() || it->second.index() != value.index()) return false;
        const auto& other = it->second;
        if (const auto* f3 = std::get_if<dve::Float3>(&value)) {
            const auto& g = std::get<dve::Float3>(other);
            if (f3->x != g.x || f3->y != g.y || f3->z != g.z) return false;
        } else if (const auto* q = std::get_if<dve::Quaternion>(&value)) {
            const auto& r = std::get<dve::Quaternion>(other);
            if (q->x != r.x || q->y != r.y || q->z != r.z || q->w != r.w) return false;
        } else if (const auto* i = std::get_if<std::int64_t>(&value)) {
            if (*i != std::get<std::int64_t>(other)) return false;
        } else if (const auto* d = std::get_if<double>(&value)) {
            if (*d != std::get<double>(other)) return false;
        } else if (const auto* t = std::get_if<std::string>(&value)) {
            if (*t != std::get<std::string>(other)) return false;
        } else if (std::get<bool>(value) != std::get<bool>(other)) {
            return false;
        }
    }
    return true;
}

void test_extensions_round_trip_and_validation() {
    const std::string original = read_string(kExamples / kHouseFiles[0]);
    std::vector<dve::Component> components(2);
    components[0].id = 4;
    components[0].type = "dve.spawn";
    components[0].properties["category"] = std::string("a\"b\\c\n");
    components[0].properties["enabled"] = true;
    components[1].id = 9;
    components[1].type = "game.stats";
    components[1].enabled = false;
    components[1].properties["hp"] = std::int64_t{-9007199254740993LL};
    components[1].properties["speed"] = 1.0 / 3.0;
    components[1].properties["offset"] = dve::Float3{0.1F, -2.5F, 1e-7F};
    components[1].properties["turn"] = dve::Quaternion{0.5F, 0.5F, 0.5F, 0.5F};
    const dve::RuntimeSceneAttachment attachment{"hand.R", false, true};
    const std::string extensions = dve::dvoxscene_object_extensions_json(
        dve::RuntimeSceneGeometry::Voxel, components, attachment);
    CHECK(dve::dvoxscene_object_extensions_json(dve::RuntimeSceneGeometry::Voxel, {}, std::nullopt).empty());
    std::string text = original;
    replace_once(text, "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,2,0,1]}",
                 "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,2,0,1],\"extensions\":" + extensions + "}");
    dve::RuntimeSceneError error;
    const auto parsed = dve::parse_dvoxscene_manifest(text, {}, &error);
    CHECK(parsed.has_value());
    if (!parsed) { std::cerr << error.message << "\n"; return; }
    const auto& upper = parsed->objects[1];
    CHECK(upper.extensionVersion == dve::kDvoxSceneExtensionVersion);
    CHECK(parsed->objects[0].extensionVersion == 0U && parsed->objects[0].components.empty());
    CHECK(upper.components.size() == 2U);
    if (upper.components.size() == 2U) {
        CHECK(upper.components[0].id == 4U && upper.components[0].type == "dve.spawn" && upper.components[0].enabled);
        CHECK(same_properties(upper.components[0].properties, components[0].properties));
        CHECK(same_properties(upper.components[1].properties, components[1].properties)); // exact doubles, int64, floats
        CHECK(!upper.components[1].enabled);
    }
    CHECK(upper.attachment && upper.attachment->socket == "hand.R" && !upper.attachment->inheritPosition &&
          upper.attachment->inheritRotation);
    // Re-serializing the parsed metadata gives the same text (canonical form).
    CHECK(dve::dvoxscene_object_extensions_json(upper.geometry, upper.components, upper.attachment) == extensions);

    // The path-based RuntimeSceneWorld parser accepts the block (components are metadata there).
    const auto temp = make_temp_dir("extensions");
    for (const char* file : kHouseFiles) std::filesystem::copy_file(kExamples / file, temp / file);
    write_string(temp / kHouseFiles[0], text);
    {
        dve::RuntimeSceneWorld runtime;
        const auto staged = runtime.stage_scene_package(temp / kHouseFiles[0]);
        CHECK(static_cast<bool>(staged));
    }

    struct Case { std::string replacement; dve::RuntimeSceneErrorCode code; };
    const std::string upperMatrix = "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,2,0,1]}";
    const std::string rootMatrix = "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}";
    const auto with = [&](const std::string& ext) {
        return "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,2,0,1],\"extensions\":" + ext + "}";
    };
    const std::vector<Case> cases = {
        {with("{\"version\":2}"), dve::RuntimeSceneErrorCode::UnsupportedVersion},
        {with("{\"version\":0}"), dve::RuntimeSceneErrorCode::UnsupportedVersion},
        {with("{}"), dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("[]"), dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"physics\":{}}"), dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"geometry\":\"splat\"}"), dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"geometry\":\"polygon\"}"), dve::RuntimeSceneErrorCode::InvalidManifest}, // .dvox file
        {with("{\"version\":1,\"components\":[{\"id\":1,\"type\":\"x.y\",\"properties\":{\"a\":{\"int\":5}}}]}"),
         dve::RuntimeSceneErrorCode::InvalidManifest}, // ints are decimal strings
        {with("{\"version\":1,\"components\":[{\"id\":1,\"type\":\"x.y\",\"properties\":{\"a\":{\"float\":1,\"int\":\"1\"}}}]}"),
         dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"components\":[{\"id\":1,\"type\":\"x.y\",\"properties\":{\"a\":{\"float3\":[1,2]}}}]}"),
         dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"components\":[{\"id\":0,\"type\":\"x.y\"}]}"), dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"components\":[{\"id\":1,\"type\":\"x.y\"},{\"id\":1,\"type\":\"x.z\"}]}"),
         dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"components\":[{\"id\":1,\"type\":\"bad type!\"}]}"), dve::RuntimeSceneErrorCode::InvalidManifest},
        {with("{\"version\":1,\"attachment\":{\"socket\":\"a\",\"weld\":true}}"), dve::RuntimeSceneErrorCode::InvalidManifest},
    };
    for (const Case& c : cases) {
        std::string bad = original;
        replace_once(bad, upperMatrix, c.replacement);
        dve::RuntimeSceneError caseError;
        CHECK(!dve::parse_dvoxscene_manifest(bad, {}, &caseError));
        if (caseError.code != c.code) {
            std::cerr << "FAIL extension case " << c.replacement << ": " << dve::to_string(caseError.code) << " ("
                      << caseError.message << ")\n";
            ++failures;
        }
    }
    // An attachment needs a parent.
    {
        std::string bad = original;
        replace_once(bad, rootMatrix, "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],\"extensions\":"
                                      "{\"version\":1,\"attachment\":{}}}");
        dve::RuntimeSceneError caseError;
        CHECK(!dve::parse_dvoxscene_manifest(bad, {}, &caseError));
        CHECK(caseError.code == dve::RuntimeSceneErrorCode::InvalidParent);
    }
    // Polygon geometry needs a .dmesh file, and the path-based RuntimeSceneWorld refuses it.
    {
        std::string polygon = original;
        replace_once(polygon, "\"file\":\"multi_object_house_Furniture_3eb.dvox\"", "\"file\":\"furniture.dmesh\"");
        replace_once(polygon, "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,3,2,0,1]}",
                     "\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,3,2,0,1],\"extensions\":{\"version\":1,\"geometry\":\"polygon\"}}");
        const auto memory = dve::parse_dvoxscene_manifest(polygon, {}, &error);
        CHECK(memory && memory->objects[2].geometry == dve::RuntimeSceneGeometry::Polygon);
        write_string(temp / kHouseFiles[0], polygon);
        write_string(temp / "furniture.dmesh", "not read");
        dve::RuntimeSceneWorld runtime;
        const auto staged = runtime.stage_scene_package(temp / kHouseFiles[0]);
        CHECK(!staged && staged.error.code == dve::RuntimeSceneErrorCode::InvalidManifest);
        CHECK(staged.error.message.find("polygon") != std::string::npos);
    }
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

void test_attach_children_policy() {
    const auto base = make_temp_dir("attach");
    const auto root = make_project(base);
    std::string error;
    auto content = dve::LooseContentSource::open(root, &error);
    CHECK(content != nullptr);
    if (!content) return;
    // Default: parents are metadata only.
    {
        auto world = make_world();
        const auto loaded = dve::load_scene_into_game_world(*content, kScene, *world);
        CHECK(static_cast<bool>(loaded) && loaded.objects.size() == 3U);
        for (const auto& object : loaded.objects) CHECK(!object.attached && !world->parent_of(object.gameObjectId));
    }
    // With the option (what dve_player uses) both children follow their parents.
    dve::GameSceneLoadOptions options;
    options.attachChildrenToParents = true;
    {
        auto world = make_world();
        const auto loaded = dve::load_scene_into_game_world(*content, kScene, *world, options);
        CHECK(static_cast<bool>(loaded) && loaded.objects.size() == 3U);
        if (loaded.objects.size() == 3U) {
            CHECK(!loaded.objects[0].attached && loaded.objects[1].attached && loaded.objects[2].attached);
            CHECK(world->parent_of(loaded.objects[2].gameObjectId) == loaded.objects[1].gameObjectId);
            const auto before = *world->position(loaded.objects[2].gameObjectId);
            for (int i = 0; i < 60; ++i) world->tick(1.0F / 60.0F);
            // UpperBlock rides the static foundation, so the furniture stays put too (and its
            // body does not build up falling speed).
            const auto after = *world->position(loaded.objects[2].gameObjectId);
            CHECK(std::abs(after.y - before.y) < 1.0e-4F);
            CHECK(std::abs(world->linear_velocity(loaded.objects[2].gameObjectId)->y) < 1.0e-3F);
        }
    }
    // An anchored child of a static collision parent is left alone (it could never move).
    {
        std::string text = read_string(kExamples / kHouseFiles[0]);
        replace_once(text, "\"parent\":0,\"anchored\":false", "\"parent\":0,\"anchored\":true");
        write_string(root / kScene, text);
        auto world = make_world();
        const auto loaded = dve::load_scene_into_game_world(*content, kScene, *world, options);
        CHECK(static_cast<bool>(loaded) && loaded.objects.size() == 3U);
        if (loaded.objects.size() == 3U) {
            CHECK(!loaded.objects[1].attached && loaded.objects[2].attached);
            CHECK(!world->render_objects()[1].dynamic);
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

std::unique_ptr<dve::VoxelObject> voxel_cube(std::uint64_t id, int size) {
    auto voxels = std::make_unique<dve::VoxelObject>(id);
    for (int x = 0; x < size; ++x)
        for (int y = 0; y < size; ++y)
            for (int z = 0; z < size; ++z) (void)voxels->set_voxel({x, y, z}, 1);
    return voxels;
}

// Parent cube resting on a slab, child cube overlapping the parent. Returns the parent's
// horizontal drift and the parent/child world positions after `steps` ticks.
struct OverlapRun { bool available{}; float drift{}; dve::Float3 parent{}; dve::Float3 child{}; };
OverlapRun run_overlap(bool attach, int steps) {
    OverlapRun run;
    std::string error;
    auto physics = dve::create_physics3d_world(dve::Physics3DBackend::Jolt, nullptr, &error);
    if (!physics) return run;
    run.available = true;
    dve::GameWorld world(std::move(physics));
    dve::GameObjectDesc ground;
    ground.name = "ground";
    ground.dynamic = false;
    ground.voxels = std::make_unique<dve::VoxelObject>(1);
    for (int x = -10; x < 10; ++x)
        for (int z = -10; z < 10; ++z) (void)ground.voxels->set_voxel({x, 0, z}, 1);
    CHECK(world.create_object(std::move(ground), &error) != dve::kInvalidGameObjectId);
    dve::GameObjectDesc parent;
    parent.name = "parent";
    parent.transform.position = {0.0F, 0.12F, 0.0F};
    parent.voxels = voxel_cube(2, 4);
    const auto parentId = world.create_object(std::move(parent), &error);
    dve::GameObjectDesc child;
    child.name = "child";
    child.transform.position = {0.25F, 0.2F, 0.05F}; // deep inside the parent's +x side
    child.voxels = voxel_cube(3, 2);
    const auto childId = world.create_object(std::move(child), &error);
    CHECK(parentId != dve::kInvalidGameObjectId && childId != dve::kInvalidGameObjectId);
    if (attach) CHECK(world.attach_object(childId, parentId, true, {}, true, true, &error));
    for (int i = 0; i < steps; ++i) world.tick(1.0F / 60.0F);
    run.parent = *world.position(parentId);
    run.child = *world.position(childId);
    run.drift = std::sqrt(run.parent.x * run.parent.x + run.parent.z * run.parent.z);
    return run;
}

void test_jolt_attachment_does_not_push_parent() {
    const OverlapRun attached = run_overlap(true, 120);
    if (!attached.available) {
        std::cout << "Jolt not compiled in; attachment/parent contact filter check skipped\n";
        return;
    }
    const OverlapRun loose = run_overlap(false, 120);
    std::cout << "overlap drift: attached=" << attached.drift << " unattached=" << loose.drift << "\n";
    // Sanity: without the attachment the overlap really does push the parent sideways...
    CHECK(loose.drift > 0.005F); // ~0.02 m with Jolt 5.x
    // ...but an attached child is filtered against its parent: the parent settles in place
    // and the child keeps its authored offset.
    CHECK(attached.drift < 0.001F); // ~2e-6 m
    CHECK(std::abs((attached.child.x - attached.parent.x) - 0.25F) < 1.0e-3F);
    CHECK(std::abs((attached.child.y - attached.parent.y) - 0.08F) < 1.0e-3F);
}

int main() {
    if (!dve::geometry_kind_supported(dve::GeometryKind::Voxel)) {
        std::cout << "voxel geometry disabled in this profile; dmesh parity only\n";
        test_dmesh_memory_matches_path();
        return failures == 0 ? 0 : 1;
    }
    test_dvox_memory_matches_path();
    test_dmesh_memory_matches_path();
    test_spawn_from_bytes_matches_path();
    test_manifest_memory_matches_path();
    test_loose_and_pak_give_identical_worlds();
    test_failures_are_clean();
    test_extensions_round_trip_and_validation();
    test_attach_children_policy();
    test_jolt_attachment_does_not_push_parent();
    if (failures != 0) {
        std::cerr << failures << " game scene loader check(s) failed\n";
        return 1;
    }
    std::cout << "game scene loader tests passed\n";
    return 0;
}
