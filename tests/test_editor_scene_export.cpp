// dve_export_scene library tests (packaging Phase 4):
//  - the example editor scene exports to DVOXSCENE + .dvox that load_scene_into_game_world
//    accepts, with the editor's ids, transforms, voxels, anchoring and material library
//  - hidden / empty objects are skipped, unsupported data is reported as warnings,
//    parents become manifest parent indices, collision flags map to generateCollision
//  - --strict semantics: any warning fails and nothing is written
//  - re-export is byte-identical and removes stale .dvox files; bad output names fail
#include "dve/content_source.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/game_scene_loader.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

std::filesystem::path make_temp_dir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        (std::string("dve_editor_scene_export_") + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

std::string read_string(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::uint64_t voxel_hash(const dve::VoxelObject& object) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) { for (int i = 0; i < 8; ++i) { hash ^= (v >> (i * 8)) & 0xFFU; hash *= 1099511628211ULL; } };
    for (const auto& [key, brick] : object.bricks()) {
        if (brick.empty()) continue;
        mix(static_cast<std::uint32_t>(key.x)); mix(static_cast<std::uint32_t>(key.y)); mix(static_cast<std::uint32_t>(key.z));
        for (const dve::MaterialId m : brick.materials()) mix(m);
    }
    return hash;
}

bool contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) if (line.find(needle) != std::string::npos) return true;
    return false;
}

bool near(float a, float b) { return std::abs(a - b) <= 1.0e-5F; }

struct Loaded {
    std::unique_ptr<dve::GameWorld> world;
    dve::GameSceneLoadResult result;
};

Loaded load(const std::filesystem::path& root, const std::string& manifest) {
    Loaded loaded;
    loaded.world = std::make_unique<dve::GameWorld>(dve::create_physics3d_world(dve::Physics3DBackend::Reference));
    std::string error;
    auto content = dve::LooseContentSource::open(root, &error);
    CHECK(content != nullptr);
    if (!content) return loaded;
    loaded.result = dve::load_scene_into_game_world(*content, manifest, *loaded.world);
    if (!loaded.result) std::cerr << "load failed: " << loaded.result.error.message << "\n";
    return loaded;
}

void test_example_scene() {
    const auto source = std::filesystem::path(DVE_TEST_SOURCE_DIR) /
        "examples/editor_demo_project/scenes/editor_demo.dvescene";
    std::string error;
    const auto document = dve::editor::EditorDocument::load(source, &error);
    CHECK(document.has_value());
    if (!document) { std::cerr << error << "\n"; return; }
    const auto materials = dve::editor::EditorMaterialLibrary::make_default();
    const auto root = make_temp_dir("example");
    const auto manifest = root / "scenes" / "editor_demo.dvoxscene.json";
    std::filesystem::create_directories(manifest.parent_path());
    const auto result = dve::editor::export_editor_scene(*document, materials, manifest);
    CHECK(result.success);
    if (!result.success) { std::cerr << result.error << "\n"; return; }
    CHECK(result.objects.size() == document->objects().size());
    CHECK(result.skippedObjects == 0U);
    CHECK(std::filesystem::is_regular_file(root / "scenes/editor_demo.objects/1001.dvox"));

    const Loaded loaded = load(root, "scenes/editor_demo.dvoxscene.json");
    CHECK(static_cast<bool>(loaded.result));
    CHECK(loaded.result.objects.size() == document->objects().size());
    const auto renderObjects = loaded.world->render_objects();
    CHECK(renderObjects.size() == document->objects().size());
    std::size_t index = 0;
    for (const auto& [id, object] : document->objects()) {
        if (index >= renderObjects.size()) break;
        const dve::GameRenderObject& runtime = renderObjects[index];
        const auto& loadedObject = loaded.result.objects[index];
        CHECK(loadedObject.sceneObjectId == id);
        CHECK(*runtime.name == object.name);
        CHECK(runtime.voxels != nullptr);
        if (runtime.voxels) CHECK(voxel_hash(*runtime.voxels) == voxel_hash(*object.voxels));
        CHECK(runtime.dynamic == !object.flags.anchored);
        CHECK(near(runtime.voxelSizeMeters, object.voxelSizeMeters));
        const dve::RigidTransform expected = document->world_transform(id).value_or(object.transform);
        CHECK(near(runtime.transform.position.x, expected.position.x));
        CHECK(near(runtime.transform.position.y, expected.position.y));
        CHECK(near(runtime.transform.position.z, expected.position.z));
        // Materials come from the editor library (not the grey placeholders of the editor's
        // own revision folder), so colours and densities match what the editor shows.
        for (std::size_t m = 1; m < runtime.materials.size(); ++m) {
            const auto* entry = materials.find(static_cast<dve::MaterialId>(m));
            if (!entry) continue;
            CHECK(runtime.materials[m].name == entry->definition.name);
            CHECK(runtime.materials[m].densityKilogramsPerCubicMeter == entry->definition.densityKilogramsPerCubicMeter);
        }
        ++index;
    }

    // Re-export: identical bytes, stale files removed.
    const std::string first = read_string(manifest);
    std::ofstream(root / "scenes/editor_demo.objects/999.dvox") << "stale";
    const auto again = dve::editor::export_editor_scene(*document, materials, manifest);
    CHECK(again.success);
    CHECK(read_string(manifest) == first);
    CHECK(!std::filesystem::exists(root / "scenes/editor_demo.objects/999.dvox"));

    // Output must be a .dvoxscene.json.
    const auto bad = dve::editor::export_editor_scene(*document, materials, root / "scene.json");
    CHECK(!bad.success);
    std::filesystem::remove_all(root);
}

dve::editor::EditorObject voxel_object(dve::editor::EditorObjectId id, const char* name, dve::Float3 position,
                                       dve::MaterialId material, int count) {
    dve::editor::EditorObject object(id, name);
    object.voxels = std::make_unique<dve::VoxelObject>(id);
    for (int i = 0; i < count; ++i) (void)object.voxels->set_voxel({i, 0, 0}, material);
    object.transform.position = position;
    return object;
}

void test_mapping_and_warnings() {
    using namespace dve::editor;
    EditorDocument document("Mapping");
    auto base = voxel_object(10, "Base", {0, 0, 0}, 1, 4);
    base.flags.anchored = true;
    document.add_object(std::move(base));
    auto hidden = voxel_object(11, "Hidden", {1, 0, 0}, 1, 2);
    hidden.flags.visible = false;
    document.add_object(std::move(hidden));
    auto tagged = voxel_object(12, "Tagged", {2, 0, 0}, 3, 3);
    tagged.tags = {"loot"};
    tagged.flags.collisionEnabled = false;
    document.add_object(std::move(tagged));
    document.add_object(voxel_object(13, "Child", {0, 1, 0}, 42, 1));
    document.add_object(EditorObject(14, "Empty"));
    std::string error;
    CHECK(document.attach_object(13, 10, true, {}, true, true, &error));

    const auto materials = EditorMaterialLibrary::make_default();
    const auto root = make_temp_dir("mapping");
    const auto manifest = root / "mapping.dvoxscene.json";

    // Strict: the hidden/empty/tag/attachment/material warnings fail the export, nothing written.
    EditorSceneExportOptions strict;
    strict.strict = true;
    const auto strictResult = export_editor_scene(document, materials, manifest, strict);
    CHECK(!strictResult.success);
    CHECK(!std::filesystem::exists(manifest));

    const auto result = export_editor_scene(document, materials, manifest);
    CHECK(result.success);
    if (!result.success) { std::cerr << result.error << "\n"; return; }
    CHECK(result.objects.size() == 3U);
    CHECK(result.skippedObjects == 2U);
    CHECK(contains(result.warnings, "'Hidden' skipped"));
    CHECK(contains(result.warnings, "'Empty' skipped"));
    CHECK(contains(result.warnings, "tags/groups/layer dropped"));
    CHECK(contains(result.warnings, "attachment exported as hierarchy metadata"));
    CHECK(contains(result.warnings, "material id(s) 42"));
    const std::string text = read_string(manifest);
    CHECK(text.find("\"id\":13") != std::string::npos);
    CHECK(text.find("\"nodePath\":\"/Base/Child\"") != std::string::npos);
    CHECK(text.find("\"parent\":0") != std::string::npos);
    CHECK(text.find("\"generateCollision\":false") != std::string::npos);

    const Loaded loaded = load(root, "mapping.dvoxscene.json");
    CHECK(static_cast<bool>(loaded.result));
    CHECK(loaded.result.objects.size() == 3U);
    if (loaded.result.objects.size() == 3U) {
        CHECK(loaded.result.objects[0].anchored);
        CHECK(!loaded.result.objects[1].collision);
        CHECK(loaded.result.objects[2].parentGameObjectId == loaded.result.objects[0].gameObjectId);
    }
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_example_scene();
    test_mapping_and_warnings();
    if (failures != 0) {
        std::cerr << "dve_editor_scene_export_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "dve_editor_scene_export_tests: PASS\n";
    return 0;
}
