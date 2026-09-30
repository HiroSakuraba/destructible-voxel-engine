// dve_export_scene library tests (packaging Phase 4):
//  - the example editor scene exports to DVOXSCENE + .dvox that load_scene_into_game_world
//    accepts, with the editor's ids, transforms, voxels, anchoring and material library
//  - hidden / empty objects are skipped, unsupported data is reported as warnings,
//    parents become manifest parent indices, collision flags map to generateCollision
//  - --strict semantics: any warning fails and nothing is written
//  - re-export is byte-identical and removes stale .dvox files; bad output names fail
//  - .dmesh objects are copied and load as polygon objects (collision and visual-only)
//  - 3D text is baked to voxels with its face/side colours; Gabor volumes are baked to
//    visual-only voxels (and a field that never reaches the threshold is skipped)
//  - components (and tags/groups/layer as dve.membership) survive into GameWorld; unknown
//    engine types are dropped with a warning; attachments are real in the loaded world
#include "dve/content_source.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/game_scene_loader.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/runtime_scene.hpp"

#include "support/export_scene_fixtures.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <variant>

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
    // Tags now travel as a dve.membership component and the attachment as an extension.
    CHECK(!contains(result.warnings, "tags/groups/layer dropped"));
    CHECK(!contains(result.warnings, "attachment exported as hierarchy metadata"));
    CHECK(result.objects.size() == 3U && result.objects[2].attached && result.objects[1].componentCount == 1U);
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
        CHECK(loaded.result.objects[2].attached);
        CHECK(loaded.world->parent_of(loaded.result.objects[2].gameObjectId) == loaded.result.objects[0].gameObjectId);
        CHECK(loaded.world->has_tag(loaded.result.objects[1].gameObjectId, "loot"));
    }
    std::filesystem::remove_all(root);
}


dve::RigidTransform at(dve::Float3 position) {
    dve::RigidTransform transform;
    transform.position = position;
    return transform;
}

void test_polygon_objects() {
    using namespace dve::editor;
    const auto root = make_temp_dir("polygon");
    std::filesystem::create_directories(root / "project/assets");
    std::ofstream(root / "project/project.dveproject") << "DVE_PROJECT 1\n";
    const dve::CookedPolygonAsset box = dve::test_fixtures::make_box_polygon({0.5F, 0.25F, 0.5F}, {0.8F, 0.2F, 0.2F, 1}, 7);
    std::string error;
    CHECK(dve::write_dmesh(root / "project/assets/box.dmesh", box, &error));

    EditorDocument document("Polygons");
    auto ground = voxel_object(1, "Ground", {0, -1, 0}, 1, 8);
    ground.flags.anchored = true;
    document.add_object(std::move(ground));
    EditorObject crate(2, "Crate");
    crate.sourceAsset = "assets/box.dmesh";
    crate.transform = at({0, 2, 0});
    document.add_object(std::move(crate));
    EditorObject sign(3, "Sign");
    sign.sourceAsset = "assets/box.dmesh";
    sign.flags.collisionEnabled = false;
    sign.transform = at({2, 1, 0});
    document.add_object(std::move(sign));
    EditorObject missing(4, "Missing");
    missing.sourceAsset = "assets/missing.dmesh";
    document.add_object(std::move(missing));

    const auto materials = EditorMaterialLibrary::make_default();
    EditorSceneExportOptions options;
    options.projectRoot = root / "project";
    const auto manifest = root / "out/level.dvoxscene.json";
    std::filesystem::create_directories(manifest.parent_path());
    const auto result = export_editor_scene(document, materials, manifest, options);
    CHECK(result.success);
    if (!result.success) { std::cerr << result.error << "\n"; return; }
    CHECK(result.objects.size() == 3U);
    CHECK(result.skippedObjects == 1U);
    CHECK(contains(result.warnings, "'Missing' skipped"));
    CHECK(std::filesystem::is_regular_file(root / "out/level.objects/2.dmesh"));
    CHECK(read_string(root / "out/level.objects/2.dmesh") == read_string(root / "project/assets/box.dmesh"));
    const std::string text = read_string(manifest);
    CHECK(text.find("\"file\":\"level.objects/2.dmesh\"") != std::string::npos);
    CHECK(text.find("\"geometry\":\"polygon\"") != std::string::npos);

    const Loaded loaded = load(root / "out", "level.dvoxscene.json");
    CHECK(static_cast<bool>(loaded.result));
    if (loaded.result.objects.size() == 3U) {
        const auto crateId = loaded.result.objects[1].gameObjectId;
        const auto signId = loaded.result.objects[2].gameObjectId;
        CHECK(loaded.result.objects[1].geometry == dve::RuntimeSceneGeometry::Polygon);
        CHECK(loaded.world->geometry_kind(crateId) == dve::GameGeometryKind::Polygon);
        CHECK(loaded.world->geometry_kind(signId) == dve::GameGeometryKind::Polygon);
        CHECK(loaded.world->has_collision(crateId) == true);
        CHECK(loaded.world->has_collision(signId) == false);
        std::size_t polygons = 0;
        for (const auto& object : loaded.world->render_objects()) {
            if (object.polygon) {
                ++polygons;
                CHECK(object.polygon->vertices.size() == box.vertices.size());
            }
        }
        CHECK(polygons == 2U);
        // The crate is a dynamic polygon body: it falls under gravity.
        const float before = loaded.world->position(crateId)->y;
        for (int i = 0; i < 10; ++i) loaded.world->tick(1.0F / 60.0F);
        CHECK(loaded.world->position(crateId)->y < before);
        CHECK(near(loaded.world->position(signId)->y, 1.0F));
    }

    // Re-export: byte-identical, stale .dmesh files removed as well.
    const std::string first = read_string(manifest);
    std::ofstream(root / "out/level.objects/999.dmesh") << "stale";
    const auto again = export_editor_scene(document, materials, manifest, options);
    CHECK(again.success);
    CHECK(read_string(manifest) == first);
    CHECK(!std::filesystem::exists(root / "out/level.objects/999.dmesh"));

    // The project root defaults to the folder holding project.dveproject above the scene.
    EditorDocument saved = clone_editor_document(document);
    std::filesystem::create_directories(root / "project/scenes");
    const auto saveResult = saved.save_transactional(root / "project/scenes/level.dvescene");
    CHECK(saveResult.success);
    const auto reloaded = EditorDocument::load(root / "project/scenes/level.dvescene", &error);
    CHECK(reloaded.has_value());
    if (reloaded) {
        const auto defaulted = export_editor_scene(*reloaded, materials, root / "out2/level.dvoxscene.json");
        CHECK(defaulted.success);
        CHECK(std::filesystem::is_regular_file(root / "out2/level.objects/2.dmesh"));
    }
    std::filesystem::remove_all(root);
}

void test_text3d_bake() {
    using namespace dve::editor;
    const auto root = make_temp_dir("text3d");
    dve::Text3DStyle style;
    style.emSizeMeters = 1.0F;
    style.extrusionDepthMeters = 0.35F;
    style.faceColor = {0.9F, 0.8F, 0.1F, 1.0F};
    style.sideColor = {0.2F, 0.3F, 0.9F, 1.0F};
    auto cooked = dve::test_fixtures::cook_test_text(root / "font", "A", style, 5);
    CHECK(static_cast<bool>(cooked));
    if (!cooked) { std::cerr << cooked.error << "\n"; return; }

    // Direct bake: the glyph is the triangle (0,0) (0.5,1) (1,0) in metres.
    const auto materials = EditorMaterialLibrary::make_default();
    std::string error;
    const auto baked = bake_text3d_voxels(cooked.asset, 0.1F, materials, 5, 1U << 20U, &error);
    CHECK(baked.has_value());
    if (!baked) { std::cerr << error << "\n"; return; }
    CHECK(baked->object.material_at({5, 1, 0}) != dve::kAirMaterial);   // (0.55, 0.15): inside
    CHECK(baked->object.material_at({0, 9, 0}) == dve::kAirMaterial);   // (0.05, 0.95): outside
    CHECK(baked->object.material_at({9, 9, 0}) == dve::kAirMaterial);   // (0.95, 0.95): outside
    // Depth 0.35 m at 0.1 m => layers -2..1 (centres -0.15..0.15): back/front face, inner side.
    CHECK(baked->object.material_at({5, 1, -2}) == 1U);
    CHECK(baked->object.material_at({5, 1, -1}) == 2U);
    CHECK(baked->object.material_at({5, 1, 0}) == 2U);
    CHECK(baked->object.material_at({5, 1, 1}) == 1U);
    CHECK(baked->object.material_at({5, 1, 2}) == dve::kAirMaterial);
    CHECK(baked->materials.size() == 3U);
    if (baked->materials.size() == 3U) {
        CHECK(near(baked->materials[1].baseColor.x, 0.9F) && near(baked->materials[2].baseColor.z, 0.9F));
    }
    const std::uint64_t count = baked->object.occupied_voxel_count();
    CHECK(count > 150U && count < 260U); // ~ area 0.5 m^2 => ~50 columns x 4 layers
    // A voxel much larger than the glyph cannot hold it; tiny budgets are refused.
    CHECK(!bake_text3d_voxels(cooked.asset, 0.1F, materials, 5, 10U, &error).has_value());

    EditorDocument document("Text");
    EditorObject title(5, "Title");
    title.text3d = cooked.asset;
    title.voxelSizeMeters = 0.1F;
    title.flags.anchored = true;
    title.transform = at({1, 2, 3});
    document.add_object(std::move(title));
    const auto manifest = root / "text.dvoxscene.json";
    EditorSceneExportOptions strict;
    strict.strict = true; // baking is a note, not a warning
    const auto result = export_editor_scene(document, materials, manifest, strict);
    CHECK(result.success);
    if (!result.success) { std::cerr << result.error << "\n"; return; }
    CHECK(result.objects.size() == 1U && result.objects[0].kind == EditorSceneExportKind::BakedText3D);
    CHECK(contains(result.notes, "3D text baked"));
    const Loaded loaded = load(root, "text.dvoxscene.json");
    CHECK(static_cast<bool>(loaded.result));
    const auto render = loaded.world->render_objects();
    CHECK(render.size() == 1U);
    if (render.size() == 1U) {
        CHECK(render[0].voxels != nullptr && render[0].voxels->occupied_voxel_count() == count);
        CHECK(near(render[0].transform.position.y, 2.0F));
        CHECK(render[0].materials.size() == 3U && near(render[0].materials[1].baseColor.y, 0.8F));
    }
    std::filesystem::remove_all(root);
}

void test_gabor_bake() {
    using namespace dve::editor;
    const auto root = make_temp_dir("gabor");
    const dve::GaborVolumeAsset blob = dve::test_fixtures::make_gabor_blob(0.3F, 20.0F, {0.2F, 0.6F, 0.9F});
    std::string error;
    const auto baked = bake_gabor_volume_voxels(blob, 0.1F, 0.5F, 9, 1U << 20U, 1ULL << 30U, &error);
    CHECK(baked.has_value());
    if (!baked) { std::cerr << error << "\n"; return; }
    // density = 20 exp(-r^2 / (2 * 0.3^2)) >= ln 2 / 0.1  <=>  r <= ~0.437 m.
    CHECK(baked->object.material_at({0, 0, 0}) == 1U);
    CHECK(baked->object.material_at({3, 0, 0}) == 1U);   // centre 0.35 m
    CHECK(baked->object.material_at({5, 0, 0}) == dve::kAirMaterial); // centre 0.55 m
    const std::uint64_t count = baked->object.occupied_voxel_count();
    CHECK(count > 200U && count < 600U);
    CHECK(baked->materials.size() == 2U && near(baked->materials[1].baseColor.z, 0.9F));
    // A lower threshold grows the shell; a field that never gets opaque bakes to nothing.
    const auto wider = bake_gabor_volume_voxels(blob, 0.1F, 0.2F, 9, 1U << 20U, 1ULL << 30U, &error);
    CHECK(wider && wider->object.occupied_voxel_count() > count);
    const dve::GaborVolumeAsset faint = dve::test_fixtures::make_gabor_blob(0.3F, 1.0F, {1, 1, 1});
    CHECK(!bake_gabor_volume_voxels(faint, 0.1F, 0.5F, 9, 1U << 20U, 1ULL << 30U, &error).has_value());
    CHECK(!bake_gabor_volume_voxels(blob, 0.1F, 1.5F, 9, 1U << 20U, 1ULL << 30U, &error).has_value());

    EditorDocument document("Gabor");
    EditorObject cloud(9, "Cloud");
    cloud.gaborVolume = blob;
    cloud.transform = at({0, 3, 0});
    document.add_object(std::move(cloud));
    EditorObject mist(10, "Mist");
    mist.gaborVolume = faint;
    document.add_object(std::move(mist));
    const auto materials = EditorMaterialLibrary::make_default();
    const auto manifest = root / "gabor.dvoxscene.json";
    const auto result = export_editor_scene(document, materials, manifest);
    CHECK(result.success);
    if (!result.success) { std::cerr << result.error << "\n"; return; }
    CHECK(result.objects.size() == 1U && result.objects[0].kind == EditorSceneExportKind::BakedGaborVolume);
    CHECK(result.objects.size() == 1U && !result.objects[0].collision);
    CHECK(result.skippedObjects == 1U && contains(result.warnings, "'Mist' skipped"));
    const Loaded loaded = load(root, "gabor.dvoxscene.json");
    CHECK(static_cast<bool>(loaded.result));
    if (loaded.result.objects.size() == 1U) {
        const auto id = loaded.result.objects[0].gameObjectId;
        CHECK(loaded.world->has_collision(id) == false);
        CHECK(loaded.world->voxel_count(id) == count);
        for (int i = 0; i < 10; ++i) loaded.world->tick(1.0F / 60.0F);
        CHECK(near(loaded.world->position(id)->y, 3.0F)); // visual-only: does not fall
    }
    std::filesystem::remove_all(root);
}

void test_components_and_attachments() {
    using namespace dve::editor;
    EditorDocument document("Components");
    auto cart = voxel_object(20, "Cart", {0, 5, 0}, 1, 4);
    cart.groups = {"vehicles"};
    cart.layer = 3;
    document.add_object(std::move(cart));
    document.add_object(voxel_object(21, "Flag", {0, 6, 0}, 1, 1));
    document.add_object(voxel_object(22, "Lamp", {1, 5, 0}, 1, 1));
    std::string error;
    CHECK(document.attach_object(21, 20, true, "mast", true, false, &error));
    CHECK(document.attach_object(22, 20, true, {}, true, true, &error));

    dve::Component spawn;
    spawn.type = "dve.spawn";
    spawn.properties["category"] = std::string("enemy \"boss\"\n");
    spawn.properties["enabled"] = false;
    CHECK(document.add_component(20, spawn, &error) != nullptr);
    dve::Component custom;
    custom.type = "game.health";
    custom.enabled = false;
    custom.properties["hp"] = std::int64_t{9007199254740993LL}; // > 2^53: must survive as a string
    custom.properties["regen"] = 0.1;
    custom.properties["offset"] = dve::Float3{0.5F, -1.25F, 3.0F};
    custom.properties["facing"] = dve::Quaternion{0, 0.70710677F, 0, 0.70710677F};
    CHECK(document.add_component(20, custom, &error) != nullptr);
    dve::Component unknown;
    unknown.type = "dve.not_a_real_component";
    CHECK(document.add_component(21, unknown, &error) != nullptr);

    const auto materials = EditorMaterialLibrary::make_default();
    const auto root = make_temp_dir("components");
    const auto manifest = root / "c.dvoxscene.json";
    const auto result = export_editor_scene(document, materials, manifest);
    CHECK(result.success);
    if (!result.success) { std::cerr << result.error << "\n"; return; }
    CHECK(contains(result.warnings, "'dve.not_a_real_component' dropped"));
    CHECK(result.objects.size() == 3U);
    const std::string text = read_string(manifest);
    CHECK(text.find("\"int\":\"9007199254740993\"") != std::string::npos);
    CHECK(text.find("\"socket\":\"mast\"") != std::string::npos);

    const Loaded loaded = load(root, "c.dvoxscene.json");
    CHECK(static_cast<bool>(loaded.result));
    if (loaded.result.objects.size() != 3U) return;
    const auto cartId = loaded.result.objects[0].gameObjectId;
    const auto flagId = loaded.result.objects[1].gameObjectId;
    const auto lampId = loaded.result.objects[2].gameObjectId;
    const auto* components = loaded.world->components(cartId);
    CHECK(components != nullptr);
    if (components) {
        const dve::Component* health = dve::find_component_by_type(std::span<const dve::Component>(*components), "game.health");
        CHECK(health != nullptr);
        if (health) {
            CHECK(!health->enabled);
            CHECK(std::get<std::int64_t>(health->properties.at("hp")) == 9007199254740993LL);
            CHECK(std::get<double>(health->properties.at("regen")) == 0.1);
            CHECK(near(std::get<dve::Float3>(health->properties.at("offset")).y, -1.25F));
            CHECK(near(std::get<dve::Quaternion>(health->properties.at("facing")).w, 0.70710677F));
        }
        const dve::Component* spawnPoint = dve::find_component_by_type(std::span<const dve::Component>(*components), "dve.spawn");
        CHECK(spawnPoint && std::get<std::string>(spawnPoint->properties.at("category")) == "enemy \"boss\"\n");
    }
    CHECK(loaded.world->find_by_group("vehicles").size() == 1U);
    CHECK(loaded.world->find_by_layer(3).size() == 1U);
    CHECK(loaded.world->find_by_component("game.health").size() == 1U);
    CHECK(loaded.world->find_by_component("dve.not_a_real_component").empty());

    // Attachments are real: children follow the falling cart with their authored offsets.
    CHECK(loaded.world->parent_of(flagId) == cartId && loaded.world->parent_of(lampId) == cartId);
    for (int i = 0; i < 30; ++i) loaded.world->tick(1.0F / 60.0F);
    const dve::Float3 cartPos = *loaded.world->position(cartId);
    CHECK(cartPos.y < 4.9F);
    CHECK(near(loaded.world->position(flagId)->y - cartPos.y, 1.0F));
    CHECK(near(loaded.world->position(lampId)->x - cartPos.x, 1.0F));
    CHECK(near(loaded.world->position(lampId)->y, cartPos.y));
    // Physics interplay: an attached body rides its parent instead of accumulating its own
    // gravity between the per-tick snaps.
    const auto cartVelocity = loaded.world->linear_velocity(cartId);
    const auto lampVelocity = loaded.world->linear_velocity(lampId);
    CHECK(cartVelocity && lampVelocity && std::abs(lampVelocity->y - cartVelocity->y) < 1.0e-3F);
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    test_example_scene();
    test_mapping_and_warnings();
    test_polygon_objects();
    test_text3d_bake();
    test_gabor_bake();
    test_components_and_attachments();
    if (failures != 0) {
        std::cerr << "dve_editor_scene_export_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "dve_editor_scene_export_tests: PASS\n";
    return 0;
}
