#include "dve/editor_asset_browser.hpp"
#include "dve/editor_native.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace dve::editor;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) throw std::runtime_error("could not write test fixture");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::uint64_t simple_hash(std::string_view bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}

void test_scan_query_dependencies_and_persistence(const std::filesystem::path& root) {
    write_file(root / "sources" / "crate.obj", "o crate\nv 0 0 0\n");
    write_file(root / "cooked" / "crate.dmesh", "DVE cooked crate v1\n");
    write_file(root / "assets" / "textures" / "rust.png", "fake-png-rust");
    write_file(root / "scenes" / "yard.dvescene",
               "mesh=\"cooked/crate.dmesh\"\ntexture=\"assets/textures/rust.png\"\n"
               "missing=\"assets/textures/missing.png\"\n");
    write_file(root / "scripts" / "main.lua", "local mesh = 'cooked/crate.dmesh'\n");

    EditorAssetDatabase database(root);
    EditorAssetScanReport report;
    std::string error;
    require(database.scan(&report, &error), error.c_str());
    require(report.indexed == 5U, "scan did not index the five project assets");
    require(report.added == 5U, "first scan did not report new assets");
    require(report.thumbnailsGenerated == 5U, "first scan did not generate thumbnails");

    const auto* cooked = database.find_path("cooked/crate.dmesh");
    const auto* scene = database.find_path("scenes/yard.dvescene");
    require(cooked && scene, "expected cooked or scene asset is missing");
    const std::string cookedId = cooked->id;
    require(scene->dependencies.size() == 2U, "scene dependencies were not extracted");
    require(scene->unresolvedDependencies.size() == 1U && scene->health == EditorAssetHealth::MissingDependency,
            "missing scene reference was not diagnosed");
    require(database.reverse_dependencies_of(cookedId).size() == 2U,
            "reverse dependency list did not include scene and script");

    EditorAssetQuery query;
    query.text = "rust";
    const auto rust = database.query(query);
    require(rust.size() == 1U && rust.front()->kind == EditorAssetKind::Texture,
            "search did not return the texture asset");

    require(database.set_tags(cookedId, {"Environment", "Prop", "prop"}, &error), error.c_str());
    EditorAssetDatabase reloaded(root);
    require(reloaded.load(&error), error.c_str());
    const auto* persisted = reloaded.find(cookedId);
    require(persisted && persisted->tags.size() == 2U && persisted->tags.front() == "environment",
            "asset tags did not persist or normalize");
}

void test_import_health_move_and_generation(const std::filesystem::path& root) {
    EditorAssetDatabase database(root);
    EditorAssetScanReport report;
    std::string error;
    require(database.scan(&report, &error), error.c_str());
    const auto* cooked = database.find_path("cooked/crate.dmesh");
    require(cooked, "cooked asset missing before import registration");
    const std::string stableId = cooked->id;
    const std::uint64_t initialGeneration = cooked->generation;

    require(database.register_import("sources/crate.obj", "cooked/crate.dmesh", &error), error.c_str());
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(root / "cooked" / "crate.dmesh", now - std::chrono::seconds(20));
    std::filesystem::last_write_time(root / "sources" / "crate.obj", now);
    require(database.scan(&report, &error), error.c_str());
    cooked = database.find(stableId);
    require(cooked && cooked->health == EditorAssetHealth::StaleImport,
            "newer import source did not mark cooked output stale");

    const auto moved = database.move_asset(stableId, "assets/models/crate_renamed.dmesh", true);
    require(moved.success, moved.message.c_str());
    require(moved.rewrittenReferences.size() == 2U, "move did not rewrite both text references");
    require(read_file(root / "scenes" / "yard.dvescene").find("assets/models/crate_renamed.dmesh") != std::string::npos,
            "scene reference was not rewritten");
    require(read_file(root / "scripts" / "main.lua").find("assets/models/crate_renamed.dmesh") != std::string::npos,
            "script reference was not rewritten");
    cooked = database.find(stableId);
    require(cooked && cooked->relativePath == std::filesystem::path("assets/models/crate_renamed.dmesh"),
            "move changed the stable asset ID or failed to update its path");

    write_file(root / cooked->relativePath, "DVE cooked crate v2 changed\n");
    require(database.scan(&report, &error), error.c_str());
    cooked = database.find(stableId);
    require(cooked && cooked->generation == initialGeneration + 1U,
            "asset generation did not advance after content replacement");

    std::filesystem::create_directories(root / "assets" / "relocated");
    std::filesystem::rename(root / cooked->relativePath, root / "assets" / "relocated" / "crate_external.dmesh");
    require(database.scan(&report, &error), error.c_str());
    cooked = database.find(stableId);
    require(cooked && cooked->relativePath == std::filesystem::path("assets/relocated/crate_external.dmesh"),
            "manual external move was not matched by content fingerprint");
    require(report.moved == 1U, "manual move was not reported");
}

void test_thumbnail_determinism_and_filters(const std::filesystem::path& root) {
    EditorAssetDatabase database(root);
    EditorAssetScanReport report;
    std::string error;
    require(database.scan(&report, &error), error.c_str());
    const auto* texture = database.find_path("assets/textures/rust.png");
    require(texture, "texture missing before thumbnail test");
    const auto thumbnail = root / texture->thumbnailPath;
    const std::string first = read_file(thumbnail);
    require(first.starts_with("P6\n64 64\n255\n"), "thumbnail is not a deterministic 64x64 PPM");
    std::filesystem::remove(thumbnail);
    require(database.generate_thumbnail(texture->id, &error), error.c_str());
    const std::string second = read_file(thumbnail);
    require(simple_hash(first) == simple_hash(second), "thumbnail changed across regeneration");

    EditorAssetQuery textureQuery;
    textureQuery.kind = EditorAssetKind::Texture;
    require(database.query(textureQuery).size() == 1U, "kind filter did not isolate textures");
    EditorAssetQuery generatedQuery;
    generatedQuery.includeGenerated = false;
    const auto rows = database.query(generatedQuery);
    require(std::none_of(rows.begin(), rows.end(), [](const EditorAssetRecord* row) { return row->generated; }),
            "generated exclusion filter returned cooked assets");
}


void test_native_assets_panel(const std::filesystem::path& root) {
    NativeEditorController controller;
    controller.configure_ai_assistant(root);
    require(controller.dispatch_action("window.toggle_assets"), "native assets panel did not open");
    require(controller.bottom_tab() == BottomPanelTab::Assets, "assets tab was not selected");
    require(!controller.asset_database().records().empty(), "native assets panel did not load the project index");

    controller.asset_browser_state().query.text = "rust";
    const auto filtered = controller.asset_browser_rows();
    require(filtered.size() == 1U && filtered.front()->kind == EditorAssetKind::Texture,
            "native asset search state did not filter rows");
    controller.asset_browser_state().query.text.clear();

    const auto* asset = controller.asset_database().find_path("assets/relocated/crate_external.dmesh");
    require(asset, "native fixture asset is missing");
    controller.asset_browser_state().selectedId = asset->id;
    require(controller.dispatch_action("asset.rename"), "native rename action did not begin");
    controller.text_input("crate_native.dmesh");
    controller.key_down("enter", false, false, false);
    require(controller.asset_database().find_path("assets/relocated/crate_native.dmesh"),
            "native rename did not execute the reference-safe move");

    const EditorObjectId prefabSourceId = controller.workspace().document().allocate_object_id();
    EditorObject prefabSource(prefabSourceId, "Native Prefab Source");
    prefabSource.voxels->set_voxel({0, 0, 0}, kDefaultSurfaceMaterial);
    controller.workspace().document().add_object(std::move(prefabSource));
    controller.workspace().select_object(prefabSourceId);
    require(controller.dispatch_action("create.prefab_from_selection"),
            "native prefab capture action failed");
    const auto* prefabAsset = controller.asset_database().find_path(
        "assets/prefabs/Native_Prefab_Source.dveprefab");
    require(prefabAsset && prefabAsset->kind == EditorAssetKind::Prefab,
            "native prefab capture did not create an indexed prefab asset");
    controller.asset_browser_state().selectedId = prefabAsset->id;
    const std::size_t beforeInstance = controller.workspace().document().objects().size();
    require(controller.dispatch_action("asset.instantiate_prefab"),
            "native prefab instantiation action failed");
    require(controller.workspace().document().objects().size() == beforeInstance + 1U,
            "native prefab instantiation created the wrong object count");
    require(controller.workspace().commands().undo(controller.workspace().document()).success,
            "native prefab instantiation was not undoable");
    require(controller.workspace().document().objects().size() == beforeInstance,
            "native prefab undo left the instance in the scene");
    require(controller.workspace().commands().redo(controller.workspace().document()).success,
            "native prefab instantiation was not redoable");

    require(controller.dispatch_action("asset.filter_stale"), "native issue filter action failed");
    require(controller.asset_browser_state().query.staleOnly, "native issue filter did not become active");
    require(controller.dispatch_action("asset.thumbnail"), "native thumbnail regeneration failed");
}


void test_rejects_unsafe_index(const std::filesystem::path& parent) {
    const auto root = parent / "unsafe_index";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".dve");
    write_file(root / ".dve" / "asset_index.dve",
               "DVE_ASSET_INDEX 1 1\n\"asset-bad\" \"../../escape.dmesh\" \"Polygon Mesh\" 1 1 1 1 \"\" \"\" 0\n");
    EditorAssetDatabase database(root);
    std::string error;
    require(!database.load(&error) && error.find("unsafe") != std::string::npos,
            "asset index accepted a path escaping the project root");
    std::filesystem::remove_all(root);
}

void test_animation_asset_classification() {
    require(classify_editor_asset("characters/hero.dveskeleton") == EditorAssetKind::Animation,
            "skeleton asset was not classified as animation");
    require(classify_editor_asset("characters/walk.dveanim") == EditorAssetKind::Animation,
            "animation clip was not classified as animation");
    require(to_string(EditorAssetKind::Animation) == "Animation",
            "animation asset kind did not round trip to its stable name");
}

void test_sprite_asset_classification() {
    require(classify_editor_asset("terrain/island.height.png") == EditorAssetKind::Terrain,
            "heightmap PNG classification mismatch");
    require(classify_editor_asset("terrain/island.r16") == EditorAssetKind::Terrain,
            "RAW16 terrain classification mismatch");
    require(classify_editor_asset("navigation/island.dnav") == EditorAssetKind::Navigation,
            "navigation asset classification mismatch");
    require(classify_editor_asset("characters/original_hero.dvesprite") == EditorAssetKind::Sprite,
            "sprite asset was not classified as a sprite");
    require(to_string(EditorAssetKind::Sprite) == "Sprite" &&
            editor_asset_is_text("characters/original_hero.dvesprite"),
            "sprite asset kind or text contract is incomplete");
}

} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "dve_editor_asset_browser_v176";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        test_scan_query_dependencies_and_persistence(root);
        test_import_health_move_and_generation(root);
        test_thumbnail_determinism_and_filters(root);
        test_native_assets_panel(root);
        test_rejects_unsafe_index(root.parent_path());
        test_animation_asset_classification();
        test_sprite_asset_classification();
        std::filesystem::remove_all(root);
        std::cout << "dve_editor_asset_browser_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_asset_browser_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
