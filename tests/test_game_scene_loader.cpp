// Runtime scene loading into GameWorld from a ContentSource, plus in-memory loader parity:
//  - read_dvox/read_dmesh(bytes) == read_dvox/read_dmesh(path) (results and errors)
//  - parse_dvoxscene_manifest(text) == RuntimeSceneWorld's path parser (metadata and errors)
//  - the same scene from a loose folder and from a .dvepak gives identical GameWorld state
//  - pak integrity failures, missing assets and validation errors fail cleanly (no objects)
//  - GameWorld::render_objects() exposes voxels/materials/transforms read-only

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
    if (failures != 0) {
        std::cerr << failures << " game scene loader check(s) failed\n";
        return 1;
    }
    std::cout << "game scene loader tests passed\n";
    return 0;
}
