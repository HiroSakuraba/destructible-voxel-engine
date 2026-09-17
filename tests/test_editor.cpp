#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "dve/editor_accessibility.hpp"
#include "dve/editor_command.hpp"
#include "dve/editor_diagnostics.hpp"
#include "dve/editor_diagnostics_background.hpp"
#include "dve/editor_file_workflow.hpp"
#include "dve/editor_import_preview.hpp"
#include "dve/editor_import_workflow.hpp"
#include "dve/editor_materials.hpp"
#include "dve/master_material.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_platform_bridge.hpp"
#include "dve/editor_viewport.hpp"
#include "dve/editor_project.hpp"
#include "dve/editor_tasks.hpp"
#include "dve/editor_tools.hpp"
#include "dve/editor_workspace.hpp"
#include "dve/game_ui.hpp"

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path make_temp() {
    const auto path = std::filesystem::temp_directory_path() / "dve_editor_tests";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
    return path;
}

void test_commands_and_tools() {
    EditorDocument document("Tool Test");
    EditorObject object(10, "Wall");
    object.voxels->set_voxel({0,0,0}, 1);
    document.add_object(std::move(object));
    document.mark_clean();
    EditorCommandStack stack;

    BrushSettings add;
    add.shape = BrushShape::Cube;
    add.operation = VoxelToolOperation::Add;
    add.radius = 1;
    add.material = 2;
    add.strokeId = 77;
    require(stack.execute(document, make_brush_command(document, 10, {2,0,0}, add)).success, "brush execute failed");
    require(document.find_object(10)->voxels->occupied_voxel_count() > 1, "brush added no voxels");
    const auto afterFirst = document.find_object(10)->voxels->occupied_voxel_count();
    require(stack.execute(document, make_brush_command(document, 10, {3,0,0}, add)).success, "merged brush failed");
    require(stack.size() == 1, "continuous stroke did not merge");
    require(stack.undo(document).success, "brush undo failed");
    require(document.find_object(10)->voxels->occupied_voxel_count() == 1, "brush undo did not restore state");
    require(stack.redo(document).success, "brush redo failed");
    require(document.find_object(10)->voxels->occupied_voxel_count() >= afterFirst, "brush redo failed");

    require(stack.execute(document, make_anchor_brush_command(document, 10, {0,0,0}, 0, true)).success, "anchor brush failed");
    require(document.find_object(10)->anchors.contains({0,0,0}), "anchor not painted");
    require(stack.undo(document).success, "anchor undo failed");
    require(document.find_object(10)->anchors.empty(), "anchor undo failed");

    const MaterialId airBefore = document.find_object(10)->voxels->material_at({100,100,100});
    BrushSettings paint = add;
    paint.operation = VoxelToolOperation::Paint;
    paint.strokeId = 0;
    require(stack.execute(document, make_brush_command(document, 10, {100,100,100}, paint)).success, "paint no-op failed");
    require(document.find_object(10)->voxels->material_at({100,100,100}) == airBefore, "paint created a voxel in air");
}

void test_document_project_and_recovery(const std::filesystem::path& root) {
    EditorProject project{"Editor Tests", root};
    std::string error;
    require(project.initialize(&error), error.c_str());
    require(project.save(&error), error.c_str());
    auto loadedProject = EditorProject::load(root / "project.dveproject", &error);
    require(loadedProject.has_value(), error.c_str());

    EditorDocument document("Warehouse");
    EditorObject foundation(1001, "Foundation");
    foundation.flags.anchored = true;
    foundation.voxels->fill_brick({0,0,0}, 1);
    foundation.anchors.insert({0,0,0});
    document.add_object(std::move(foundation));
    EditorObject crate(1002, "Crate");
    crate.parent = 1001;
    crate.voxels->set_voxel({0,0,0}, 2);
    document.add_object(std::move(crate));
    const auto save = document.save_transactional(project.scenes_dir() / "warehouse.dvescene");
    require(save.success, save.error.c_str());
    require(std::filesystem::exists(save.revisionDirectory / "1001.dvox"), "revision object was not written");
    auto loaded = EditorDocument::load(save.manifestPath, &error);
    require(loaded.has_value(), error.c_str());
    require(loaded->objects().size() == 2, "scene object count mismatch");
    require(loaded->find_object(1001)->voxels->occupied_voxel_count() == 512, "scene voxel content mismatch");

    loaded->find_object(1002)->voxels->set_voxel({1,0,0}, 2);
    loaded->mark_dirty();
    EditorAutosaveManager autosave(project.autosave_dir());
    auto recovery = autosave.save_recovery(*loaded, &error);
    require(recovery.has_value(), error.c_str());
    require(loaded->dirty(), "autosave changed document dirty state");
    require(!autosave.recoveries().empty(), "recovery was not discoverable");
}

void test_workspace_menus_and_preferences() {
    EditorDocument document("Workspace");
    EditorObject object(1, "Object"); object.voxels->set_voxel({0,0,0}, 1); document.add_object(std::move(object));
    EditorWorkspace workspace(std::move(document));
    require(workspace.menus().actions().size() >= 38, "default menus incomplete");
    const auto paletteMatches = workspace.menus().search("rotate", 4);
    require(!paletteMatches.empty() && paletteMatches.front().id == "transform.rotate",
            "command palette search did not rank rotate tool");
    std::string error;
    require(!workspace.menus().add({"other","Edit","Conflict","Ctrl+S"}, &error), "shortcut conflict accepted");
    workspace.set_panel_visible(PanelId::Profiler, false);
    require(!workspace.panel_visible(PanelId::Profiler), "panel visibility failed");
    require(workspace.set_mode(EditorMode::Simulate, &error), error.c_str());
    workspace.document().find_object(1)->voxels->set_voxel({1,0,0}, 1);
    require(workspace.set_mode(EditorMode::Edit, &error), error.c_str());
    require(workspace.document().find_object(1)->voxels->occupied_voxel_count() == 1, "simulate state leaked into edit mode");

    EditorPreferences preferences;
    preferences.uiScale = 1.5F;
    preferences.highContrast = true;
    const auto parsed = EditorPreferences::parse(preferences.serialize(), &error);
    require(parsed && parsed->highContrast && parsed->uiScale == 1.5F, "preferences round trip failed");
}

void test_tasks() {
    EditorTaskManager tasks(2, 4);
    std::string error;
    auto task = tasks.submit("Cook preview", [](EditorTaskContext& context) {
        context.report(0.25F, "Importing");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        context.report(0.75F, "Voxelizing");
    }, &error);
    require(task.has_value(), error.c_str());
    auto cancelTask = tasks.submit("Cancel me", [](EditorTaskContext& context) {
        for (int i = 0; i < 50 && !context.cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, &error);
    require(cancelTask.has_value(), error.c_str());
    require(tasks.cancel(*cancelTask), "task cancellation request failed");
    tasks.wait_idle();
    require(tasks.snapshot(*task)->state == EditorTaskState::Succeeded, "task did not succeed");
    require(tasks.snapshot(*cancelTask)->state == EditorTaskState::Cancelled, "task cancellation failed");
}

void test_import_workflow(const std::filesystem::path& root) {
    ModelImportWorkflow workflow;
    std::string error;
    const auto source = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples/unit_cube.obj";
    require(workflow.choose_source(source, &error), error.c_str());
    require(workflow.page() == ImportWizardPage::ObjectStructure, "wizard did not advance to object structure");
    require(!workflow.nodes().empty(), "wizard found no mesh nodes");
    require(workflow.advance(&error), error.c_str());
    workflow.settings().voxelSizeMeters = 0.5F;
    require(workflow.advance(&error), error.c_str());
    require(workflow.estimate().triangles == 12, "import estimate triangle count wrong");
    require(workflow.advance(&error), error.c_str());
    require(workflow.advance(&error), error.c_str());
    require(workflow.advance(&error), error.c_str());
    require(workflow.cook(root / "cube.dvoxscene.json", &error), error.c_str());
    require(std::filesystem::exists(root / "cube.dvoxscene.json"), "import workflow did not publish package");
}



void test_live_import_preview(const std::filesystem::path& root) {
    EditorTaskManager tasks(2, 8);
    ModelImportPreviewService preview(tasks);
    const auto source = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "examples/unit_cube.obj";
    VoxelizeSettings settings;
    settings.voxelSizeMeters = 0.5F;
    std::string error;
    require(preview.request_preview(source, settings, {}, &error), error.c_str());
    tasks.wait_idle();
    const ImportPreviewSnapshot snapshot = preview.snapshot();
    require(snapshot.ready && snapshot.objectCount == 1, "live import preview did not complete");
    require(snapshot.occupiedVoxels > 0 && snapshot.occupiedBricks > 0, "preview statistics missing");
    const auto manifest = root / "preview.dvoxscene.json";
    require(preview.publish_current(manifest, &error), error.c_str());
    require(std::filesystem::exists(manifest), "preview publication did not create a scene package");

    settings.voxelSizeMeters = 0.25F;
    require(preview.request_preview(source, settings, {}, &error), error.c_str());
    require(preview.cancel(), "preview cancellation request failed");
    tasks.wait_idle();
    const ImportPreviewSnapshot cancelled = preview.snapshot();
    require(!cancelled.ready, "cancelled preview remained publishable");
    require(cancelled.occupiedVoxels == 0 && cancelled.occupiedBricks == 0,
            "cancelled preview retained stale statistics");
    require(cancelled.error.empty(), "cancelled preview retained a stale error");
}

void test_background_object_diagnostics() {
    EditorTaskManager tasks(2, 8);
    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();

    // Two separate components (not six-neighbour adjacent) plus one voxel anchored to the
    // object's anchor set, so connectedComponents/detachedComponents/anchoredComponents are
    // all nontrivially exercised, not just occupiedVoxels.
    EditorObject object(31001, "Diagnostics Subject");
    object.voxelSizeMeters = 0.2F;
    for (int x = 0; x < 3; ++x) object.voxels->set_voxel({x, 0, 0}, 1);
    object.anchors.insert({0, 0, 0});
    object.voxels->set_voxel({10, 0, 0}, 2);

    EditorObjectDiagnosticsService service(tasks);
    std::string error;
    require(service.request(object, materials, &error), error.c_str());
    tasks.wait_idle();
    const ObjectDiagnosticsSnapshot snapshot = service.snapshot();
    require(snapshot.ready && snapshot.diagnostics.has_value(), "background diagnostics did not complete");

    // Oracle check: the background result must exactly match calling analyze_editor_object()
    // directly against an equivalent object, proving the clone-then-analyze path changes
    // nothing about the answer.
    const EditorObjectDiagnostics direct = analyze_editor_object(object, materials);
    require(snapshot.diagnostics->occupiedVoxels == direct.occupiedVoxels, "occupied voxel count mismatch");
    require(snapshot.diagnostics->connectedComponents == direct.connectedComponents, "component count mismatch");
    require(snapshot.diagnostics->connectedComponents == 2, "expected exactly two connected components");
    require(snapshot.diagnostics->anchoredComponents == 1, "expected exactly one anchored component");
    require(snapshot.diagnostics->detachedComponents == 1, "expected exactly one detached component");
    require(std::abs(snapshot.diagnostics->massKilograms - direct.massKilograms) < 1.0e-9,
            "mass mismatch between background and direct diagnostics");

    // The live object must be untouched: the background path only ever sees a clone.
    require(object.voxels->occupied_voxel_count() == 4U, "background diagnostics mutated the live object");

    // Stale-result rejection: a request that is cancelled before it can be read back must
    // never publish a result, even if the worker had already started or finished.
    require(service.request(object, materials, &error), error.c_str());
    require(service.cancel(), "diagnostics cancellation request failed");
    tasks.wait_idle();
    require(!service.snapshot().ready, "cancelled diagnostics request remained ready");
}


void test_new_project_template() {
    EditorDocument document = make_new_project_document();
    require(document.name() == "Untitled Project", "new project template has the wrong name");
    require(!document.dirty(), "new project template should start clean");
    require(document.objects().size() == 1U, "new project template should contain one starter object");
    const EditorObject& oval = document.objects().begin()->second;
    require(oval.name == "Starter Oval", "starter object is not the oval template");
    require(oval.voxels->occupied_voxel_count() > 1000U, "starter oval is unexpectedly sparse");
    require(oval.voxels->material_at({0, 4, 0}) == kDefaultSurfaceMaterial,
            "starter oval does not use Standard Surface");
    require(!oval.voxels->occupied_at({-8, 0, -6}) && !oval.voxels->occupied_at({7, 9, 5}),
            "starter oval retained bounding-box corner voxels");
    require(oval.voxels->occupied_at({-7, 4, 0}) && oval.voxels->occupied_at({0, 4, -5}),
            "starter oval radii were not populated");

    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    const EditorMaterialEntry* standard = materials.find(kDefaultSurfaceMaterial);
    require(standard && standard->definition.name == "Standard Surface",
            "Standard Surface material preset is missing");
    require(standard->definition.shadingModel == MaterialShadingModel::StandardPBR &&
            standard->definition.blendMode == MaterialBlendMode::Opaque,
            "Standard Surface is not opaque PBR");
    require(std::abs(standard->definition.baseColor.x - 0.214F) < 1.0e-6F &&
            std::abs(standard->definition.roughness - kStandardSurfaceRoughness) < 1.0e-6F &&
            standard->definition.metallic == 0.0F &&
            std::abs(standard->definition.specular - kStandardSurfaceSpecular) < 1.0e-6F,
            "Standard Surface defaults drifted");
    require(editor_material_badge(standard->definition).find("REFL LOW") != std::string::npos,
            "Standard Surface is not labeled as low reflectivity");
    const MasterMaterial standardMaster = make_standard_surface_master_material();
    require(std::abs(standardMaster.parameters.find_vector(kBaseColorParam)->defaultValue.x -
                     standard->definition.baseColor.x) < 1.0e-6F &&
            std::abs(standardMaster.parameters.find_scalar(kRoughnessParam)->defaultValue -
                     standard->definition.roughness) < 1.0e-6F &&
            std::abs(standardMaster.parameters.find_scalar(kSpecularParam)->defaultValue -
                     standard->definition.specular) < 1.0e-6F,
            "editor and master Standard Surface defaults drifted");

    NativeEditorController controller{EditorWorkspace(EditorDocument("Temporary"))};
    require(controller.dispatch_action("file.new_project"), "new project action failed");
    require(controller.workspace().document().objects().size() == 1U &&
            controller.workspace().selected_object().has_value(),
            "new project action did not create and select the oval");
    require(controller.active_material() == kDefaultSurfaceMaterial,
            "new project editor paint material is not Standard Surface");
}

void test_materials_viewport_and_native_controller(const std::filesystem::path& root) {
    EditorMaterialLibrary materials = EditorMaterialLibrary::make_default();
    require(materials.find(1) && materials.find(4), "default material presets missing");
    EditorMaterialEntry custom;
    custom.id = 20;
    custom.definition.name = "Foam";
    custom.definition.baseColor = {0.9F, 0.85F, 0.3F, 1.0F};
    custom.definition.densityKilogramsPerCubicMeter = 45.0F;
    custom.definition.specular = 0.23F;
    custom.definition.shadingModel = MaterialShadingModel::Subsurface;
    custom.definition.blendMode = MaterialBlendMode::Translucent;
    custom.definition.subsurfaceScatterDistanceMeters = 0.035F;
    custom.definition.subsurfaceColor = {0.9F, 0.7F, 0.2F};
    custom.definition.clearCoat = 0.65F;
    custom.definition.clearCoatRoughness = 0.12F;
    custom.definition.foliageColor = {0.25F, 0.75F, 0.15F};
    custom.definition.foliageTransmittance = 0.45F;
    custom.definition.foliageWrap = 0.2F;
    custom.definition.layers.push_back({4, 0.35F, MaterialLayerBlendMode::Multiply, true});
    custom.definition.transparent = true;
    custom.definition.structural = false;
    std::string error;
    require(materials.upsert(custom, &error), error.c_str());
    require(editor_material_badge(custom.definition) == "SSS BLEND",
            "material shading/blend badge is incorrect");
    require(editor_material_warnings(custom.definition).empty(),
            "valid material produced semantic warnings");
    VoxelMaterialDefinition suspicious = custom.definition;
    suspicious.shadingModel = MaterialShadingModel::ClearCoat;
    suspicious.clearCoat = 0.0F;
    suspicious.transparent = false;
    require(editor_material_warnings(suspicious).size() == 2U,
            "material semantic diagnostics missed inconsistent settings");
    const auto materialPath = root / "materials.dvematerials";
    require(materials.save(materialPath, &error), error.c_str());
    auto loadedMaterials = EditorMaterialLibrary::load(materialPath, &error);
    require(loadedMaterials && loadedMaterials->find(20), error.c_str());
    require(loadedMaterials->find(20)->definition.densityKilogramsPerCubicMeter == 45.0F,
            "material physical properties did not round trip");
    require(loadedMaterials->find(20)->definition.shadingModel == MaterialShadingModel::Subsurface &&
            loadedMaterials->find(20)->definition.blendMode == MaterialBlendMode::Translucent &&
            std::abs(loadedMaterials->find(20)->definition.specular - 0.23F) < 1.0e-6F &&
            std::abs(loadedMaterials->find(20)->definition.subsurfaceScatterDistanceMeters - 0.035F) < 1.0e-6F &&
            std::abs(loadedMaterials->find(20)->definition.clearCoat - 0.65F) < 1.0e-6F &&
            std::abs(loadedMaterials->find(20)->definition.clearCoatRoughness - 0.12F) < 1.0e-6F &&
            std::abs(loadedMaterials->find(20)->definition.foliageTransmittance - 0.45F) < 1.0e-6F &&
            loadedMaterials->find(20)->definition.layers.size() == 1 &&
            loadedMaterials->find(20)->definition.layers.front().sourceMaterial == 4 &&
            std::abs(loadedMaterials->find(20)->definition.layers.front().weight - 0.35F) < 1.0e-6F,
            "render material properties and layers did not round trip");

    MaterialLibrary authored;
    MasterMaterial master;
    master.name = "EditorBridge";
    master.parameters.scalars.push_back({"Roughness", 0.4F, 0.0F, 1.0F});
    master.parameters.vectors.push_back({"BaseColor", {0.2F, 0.4F, 0.8F, 1.0F}});
    const MasterMaterialId masterId = authored.add_master(std::move(master), &error);
    require(masterId != kInvalidMasterMaterialId, error.c_str());
    MaterialInstance instance;
    instance.name = "EditorBridgeInstance";
    instance.materialId = 42;
    instance.master = masterId;
    require(authored.add_instance(std::move(instance), &error) != kInvalidMaterialInstanceId, error.c_str());
    require(authored.resolve_all(&error), error.c_str());
    require(sync_resolved_materials(authored, *loadedMaterials, false, &error), error.c_str());
    require(loadedMaterials->find(42) && loadedMaterials->find(42)->definition.name == "EditorBridgeInstance",
            "resolved master material did not sync into editor library");

    EditorDocument document("Viewport");
    EditorObject object(77, "Pick Me");
    object.voxelSizeMeters = 0.5F;
    object.voxels->set_voxel({0,0,0}, 2);
    document.add_object(std::move(object));
    EditorCamera camera;
    camera.position = {0.25F, 0.25F, 4.0F};
    camera.target = {0.25F, 0.25F, 0.25F};
    const UiRect viewport{0,0,800,600};
    const auto centerRay = make_viewport_ray(camera, viewport, 400.0F, 300.0F);
    const auto picked = pick_editor_document(document, centerRay, 100.0F);
    require(picked && picked->objectId == 77 && picked->voxel == Int3{0,0,0}, "viewport picking failed");
    const auto draw = build_voxel_draw_list(document, materials, camera, viewport, {}, 77);
    require(draw.size() == 1 && draw.front().selected, "viewport draw-list construction failed");

    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 800);
    require(controller.layout().viewport.width > 0 && controller.layout().toolbarButtons.size() == 9,
            "native editor layout failed");
    controller.set_active_tool(EditorToolId::AddVoxel);
    require(controller.active_tool() == EditorToolId::AddVoxel, "tool selection failed");
    const auto beforeFocus = controller.focus_region();
    controller.focus_next();
    require(controller.focus_region() != beforeFocus, "keyboard focus traversal failed");
    controller.workspace().select_object(1002);
    controller.frame_selection();
    const auto items = controller.draw_items();
    require(!items.empty(), "native viewport produced no draw items");
    require(controller.dispatch_action("help.command_palette"), "command palette did not open");
    controller.key_down("r", false, false, false);
    controller.key_down("o", false, false, false);
    controller.key_down("t", false, false, false);
    require(controller.command_palette_open() && !controller.command_palette_results().empty(),
            "command palette did not accept a query");
    controller.key_down("return", false, false, false);
    require(controller.active_tool() == EditorToolId::Rotate, "command palette did not execute rotate tool");
    const AccessibilityNode accessibility = build_editor_accessibility_tree(controller);
    const std::string accessibilityJson = serialize_accessibility_tree_json(accessibility);
    require(accessibilityJson.find("sceneHierarchy") != std::string::npos &&
            accessibilityJson.find("Destructible Wall") != std::string::npos,
            "accessibility tree omitted editor semantics");
    require(controller.dispatch_action("physics.simulate"), "simulate action failed");
    require(controller.workspace().mode() == EditorMode::Simulate, "simulate mode not entered");
    require(controller.dispatch_action("physics.stop"), "stop action failed");
    require(controller.workspace().mode() == EditorMode::Edit, "edit mode not restored");
}




void test_file_workflow_and_recent_projects(const std::filesystem::path& root) {
    require(classify_editor_file("vehicle.glb").intent == EditorFileIntent::ImportModel,
            "GLB drop was not routed to model import");
    require(classify_editor_file("hero.dvesprite").intent == EditorFileIntent::OpenSpriteAsset,
            "sprite asset was not routed to its editor");
    require(classify_editor_file("warehouse.dvoxscene.json").intent == EditorFileIntent::AddScenePackage,
            "DVOX scene drop was not routed to package loading");
    require(!classify_editor_file("notes.txt").supported,
            "unsupported file was incorrectly accepted");
    const auto routes = route_editor_file_drop({"project.dveproject", "scene.dvescene", "wall.png"});
    require(routes.size() == 3 && routes[0].intent == EditorFileIntent::OpenProject &&
            routes[1].intent == EditorFileIntent::OpenScene &&
            routes[2].intent == EditorFileIntent::ImportTexture,
            "multi-file drop routing failed");

    std::filesystem::create_directories(root);
    const auto first = root / "first.dveproject";
    const auto second = root / "second.dveproject";
    const auto third = root / "third.dveproject";
    std::ofstream(first).put('1');
    std::ofstream(second).put('2');
    std::ofstream(third).put('3');
    RecentProjectRegistry recent(2);
    recent.record(first, "First");
    recent.record(second, "Second");
    recent.record(first, "First Again");
    require(recent.entries().size() == 2 && recent.entries().front().displayName == "First Again",
            "recent project ordering or de-duplication failed");
    recent.record(third, "Third");
    require(recent.entries().size() == 2 && recent.entries().front().displayName == "Third",
            "recent project capacity failed");
    const auto registryPath = root / "recent.dverecents";
    std::string error;
    require(recent.save(registryPath, &error), "recent project save failed");
    auto loaded = RecentProjectRegistry::load(registryPath, 2, &error);
    require(loaded.has_value() && loaded->entries().size() == 2,
            "recent project load failed");
    std::filesystem::remove(third);
    require(loaded->remove_missing() == 1, "missing recent project was not pruned");
}

void test_voxel_rescale_command() {
    EditorDocument document("Rescale");
    EditorObject object(41, "Scalable");
    object.voxels->set_voxel({0,0,0}, 3);
    object.anchors.insert({0,0,0});
    document.add_object(std::move(object));
    EditorCommandStack stack;
    auto command = make_rescale_voxel_object_command(document, 41, {2.0F,2.0F,2.0F});
    require(static_cast<bool>(command), "rescale command was not created");
    require(stack.execute(document, std::move(command)).success, "rescale command failed");
    require(document.find_object(41)->voxels->occupied_voxel_count() == 8, "2x voxel rescale did not create eight cells");
    require(document.find_object(41)->anchors.size() == 8, "rescale did not preserve anchor coverage");
    require(stack.undo(document).success, "rescale undo failed");
    require(document.find_object(41)->voxels->occupied_voxel_count() == 1, "rescale undo did not restore geometry");
    require(document.find_object(41)->anchors.size() == 1, "rescale undo did not restore anchors");
}

void test_multi_selection_transforms_and_diagnostics() {
    EditorDocument document("Multi Selection");
    EditorObject a(1, "A");
    a.voxelSizeMeters = 1.0F;
    a.voxels->set_voxel({0,0,0}, 1);
    a.anchors.insert({0,0,0});
    document.add_object(std::move(a));
    EditorObject b(2, "B");
    b.voxelSizeMeters = 1.0F;
    b.transform.position = {2.0F, 0.0F, 0.0F};
    b.voxels->set_voxel({0,0,0}, 4);
    document.add_object(std::move(b));

    NativeEditorController controller{EditorWorkspace(std::move(document))};
    controller.workspace().select_object(1);
    controller.workspace().add_to_selection(2);
    require(controller.workspace().selection_count() == 2, "multi-selection did not retain both objects");
    require(controller.nudge_selection({1.0F, 0.0F, 0.0F}).success, "multi-object nudge failed");
    require(controller.workspace().document().find_object(1)->transform.position.x == 1.0F, "first object did not move");
    require(controller.workspace().document().find_object(2)->transform.position.x == 3.0F, "second object did not move");
    require(controller.workspace().commands().undo(controller.workspace().document()).success, "multi-object undo failed");
    require(controller.workspace().document().find_object(1)->transform.position.x == 0.0F, "multi-object undo did not restore first object");
    require(controller.set_primary_rotation_euler_degrees({0.0F, 0.0F, 90.0F}).success, "numeric rotation failed");
    const Float3 rotated = rotate(controller.workspace().document().find_object(2)->transform.rotation, {1.0F,0.0F,0.0F});
    require(std::abs(rotated.y - 1.0F) < 0.001F, "numeric rotation did not update orientation");

    const EditorSelectionDiagnostics diagnostics = controller.selection_diagnostics();
    require(diagnostics.objectCount == 2 && diagnostics.occupiedVoxels == 2, "selection diagnostics counts wrong");
    require(diagnostics.massKilograms > 0.0, "selection diagnostics mass missing");
    require(diagnostics.collisionBoxes == 2, "collision proxy diagnostics wrong");

    controller.workspace().toggle_selection(1);
    require(controller.workspace().selection_count() == 1 && controller.workspace().selected_object() == 2,
            "selection toggle did not preserve primary object");
    require(controller.dispatch_action("edit.select_all"), "select all action failed");
    require(controller.workspace().selection_count() == 2, "select all action failed");
    require(controller.dispatch_action("transform.space"), "transform-space action failed");
    require(controller.transform_space() == EditorTransformSpace::Local, "local transform mode not entered");
}

void test_object_lifecycle_actions() {
    EditorDocument document("Lifecycle");
    EditorObject seed(1, "Seed");
    seed.voxelSizeMeters = 1.0F;
    seed.voxels->set_voxel({0, 0, 0}, 1);
    document.add_object(std::move(seed));

    NativeEditorController controller{EditorWorkspace(std::move(document))};

    // create.empty: adds a new, initially-empty, selected object.
    require(controller.dispatch_action("create.empty"), "create.empty failed");
    require(controller.workspace().document().objects().size() == 2, "create.empty did not add an object");
    const auto emptyId = controller.workspace().selected_object();
    require(emptyId.has_value(), "create.empty did not select the new object");
    const EditorObject* emptyObject = controller.workspace().document().find_object(*emptyId);
    require(emptyObject && emptyObject->voxels->occupied_voxel_count() == 0, "create.empty was not actually empty");

    // create.voxel: adds a new object with a default solid block, also selected.
    require(controller.dispatch_action("create.voxel"), "create.voxel failed");
    require(controller.workspace().document().objects().size() == 3, "create.voxel did not add an object");
    const auto voxelId = controller.workspace().selected_object();
    require(voxelId.has_value() && *voxelId != *emptyId, "create.voxel did not select a new distinct object");
    const EditorObject* voxelObject = controller.workspace().document().find_object(*voxelId);
    require(voxelObject && voxelObject->voxels->occupied_voxel_count() == 64,
            "create.voxel did not seed a default 4x4x4 block");

    // edit.delete + undo: removes the selected object, and undo brings back the exact voxels.
    require(controller.dispatch_action("edit.delete"), "edit.delete failed");
    require(controller.workspace().document().objects().size() == 2, "edit.delete did not remove the object");
    require(!controller.workspace().document().find_object(*voxelId), "deleted object still present");
    require(controller.workspace().commands().undo(controller.workspace().document()).success, "delete undo failed");
    const EditorObject* restored = controller.workspace().document().find_object(*voxelId);
    require(restored && restored->voxels->occupied_voxel_count() == 64, "delete undo did not restore voxel content");

    // A locked object must block deletion outright, not delete the rest of the selection.
    controller.workspace().document().find_object(1)->flags.locked = true;
    controller.workspace().clear_selection();
    controller.workspace().add_to_selection(1);
    controller.workspace().add_to_selection(*voxelId);
    require(!controller.dispatch_action("edit.delete"), "delete of a locked selection should fail");
    require(controller.workspace().document().objects().size() == 3, "locked delete must not remove anything");
    controller.workspace().document().find_object(1)->flags.locked = false;

    // copy + paste: original remains, a new object appears with an offset transform.
    controller.workspace().select_object(*voxelId);
    const Float3 originalPosition = controller.workspace().document().find_object(*voxelId)->transform.position;
    require(controller.dispatch_action("edit.copy"), "edit.copy failed");
    require(controller.workspace().document().objects().size() == 3, "copy must not itself add an object");
    require(controller.dispatch_action("edit.paste"), "edit.paste failed");
    require(controller.workspace().document().objects().size() == 4, "paste did not add an object");
    const auto pastedId = controller.workspace().selected_object();
    require(pastedId.has_value() && *pastedId != *voxelId, "paste did not select the new object");
    const EditorObject* pasted = controller.workspace().document().find_object(*pastedId);
    require(pasted && pasted->voxels->occupied_voxel_count() == 64, "pasted object lost its voxel content");
    require(pasted->transform.position.x != originalPosition.x || pasted->transform.position.z != originalPosition.z,
            "pasted object landed exactly on top of its source");
    require(controller.workspace().document().find_object(*voxelId) != nullptr, "copy+paste deleted the source object");

    // cut + paste: original is removed immediately, paste brings back an equivalent object.
    controller.workspace().clear_selection();
    controller.workspace().select_object(*pastedId);
    require(controller.dispatch_action("edit.cut"), "edit.cut failed");
    require(!controller.workspace().document().find_object(*pastedId), "cut did not remove the object");
    require(controller.dispatch_action("edit.paste"), "edit.paste after cut failed");
    require(controller.workspace().document().objects().size() == 4, "cut+paste should restore the object count");

    // file.new_scene: a dirty document requires explicit confirmation before destructive reset.
    require(controller.dispatch_action("file.new_scene"), "file.new_scene failed");
    require(controller.pending_destructive_action() == PendingDestructiveAction::NewScene,
            "dirty new scene did not request confirmation");
    require(!controller.workspace().document().objects().empty(),
            "new scene discarded dirty content before confirmation");
    controller.key_down("return", false, false, false);
    require(!controller.pending_destructive_confirmation(), "new scene confirmation did not close");
    require(controller.workspace().document().objects().empty(), "new scene was not empty");
    require(!controller.workspace().commands().can_undo(), "new scene did not clear undo history");
    require(controller.workspace().selection_count() == 0, "new scene did not clear selection");

    // file.exit: signals the host loop to close, does not itself terminate the process.
    require(!controller.quit_requested(), "quit was requested before file.exit was dispatched");
    require(controller.dispatch_action("file.exit"), "file.exit failed");
    require(controller.quit_requested(), "file.exit did not set the quit flag");

    require(controller.dispatch_action("help.about"), "help.about failed");

    // Window menu: hiding a panel collapses its width to zero and the viewport reclaims
    // exactly that space; showing it again restores the original layout.
    const int viewportBefore = controller.layout().viewport.width;
    const int hierarchyBefore = controller.layout().hierarchy.width;
    require(hierarchyBefore > 0, "scene hierarchy should start visible");
    require(controller.dispatch_action("window.toggle_hierarchy"), "window.toggle_hierarchy failed");
    require(controller.layout().hierarchy.width == 0, "hidden hierarchy should collapse to zero width");
    require(controller.layout().viewport.width == viewportBefore + hierarchyBefore,
            "viewport did not reclaim the hidden hierarchy's width");
    require(controller.layout().hierarchyRows.empty(), "hidden hierarchy should not generate clickable rows");
    require(controller.dispatch_action("window.toggle_hierarchy"), "re-showing hierarchy failed");
    require(controller.layout().hierarchy.width == hierarchyBefore, "hierarchy width did not restore");

    const int inspectorBefore = controller.layout().inspector.width;
    require(inspectorBefore > 0, "inspector should start visible");
    require(controller.dispatch_action("window.toggle_inspector"), "window.toggle_inspector failed");
    require(controller.layout().inspector.width == 0, "hidden inspector should collapse to zero width");
    require(controller.layout().inspectorToggles.empty(), "hidden inspector should not generate clickable toggles");
    require(controller.dispatch_action("window.toggle_inspector"), "re-showing inspector failed");
    require(controller.layout().inspector.width == inspectorBefore, "inspector width did not restore");
}

EditorDocument make_two_object_document() {
    EditorDocument document("Expanded GUI Test");
    EditorObject a(1, "Alpha");
    a.voxelSizeMeters = 1.0F;
    a.voxels->set_voxel({0, 0, 0}, 1);
    document.add_object(std::move(a));
    EditorObject b(2, "Beta");
    b.voxelSizeMeters = 1.0F;
    b.transform.position = {5.0F, 0.0F, 0.0F};
    b.voxels->set_voxel({0, 0, 0}, 2);
    document.add_object(std::move(b));
    return document;
}

void test_duplicate_and_rename() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.workspace().select_object(1);
    require(controller.dispatch_action("edit.duplicate"), "duplicate failed");
    require(controller.workspace().document().objects().size() == 3, "duplicate did not add an object");
    require(controller.workspace().selection_count() == 1, "duplicate should select exactly the new object");
    const auto duplicatedId = controller.workspace().selected_object();
    require(duplicatedId && *duplicatedId != 1, "duplicate did not select a new distinct object");
    const EditorObject* duplicated = controller.workspace().document().find_object(*duplicatedId);
    require(duplicated && duplicated->voxels->occupied_voxel_count() == 1, "duplicate lost voxel content");
    require(duplicated->transform.position.x != 0.0F || duplicated->transform.position.z != 0.0F,
            "duplicate landed exactly on its source");
    require(controller.workspace().commands().undo(controller.workspace().document()).success, "duplicate undo failed");
    require(controller.workspace().document().objects().size() == 2, "duplicate undo did not remove the copy");

    // Rename: dispatch begins an edit session (does not itself change the name), typed
    // characters arrive through text_input(), Enter commits through the command stack.
    controller.workspace().select_object(1);
    require(controller.dispatch_action("edit.rename"), "rename dispatch failed");
    require(controller.text_edit().kind == TextEditKind::ObjectName, "rename did not begin a text edit session");
    require(controller.workspace().document().find_object(1)->name == "Alpha", "rename changed the name before commit");
    for (char c : std::string("Foundation")) controller.text_input(c);
    controller.key_down("return", false, false, false);
    require(controller.text_edit().kind == TextEditKind::Inactive, "rename did not end the edit session on Enter");
    require(controller.workspace().document().find_object(1)->name == "Foundation", "rename did not apply");
    require(controller.workspace().commands().undo(controller.workspace().document()).success, "rename undo failed");
    require(controller.workspace().document().find_object(1)->name == "Alpha", "rename undo did not restore the name");

    // Escape cancels without committing.
    require(controller.dispatch_action("edit.rename"), "second rename dispatch failed");
    controller.text_input('X');
    controller.key_down("escape", false, false, false);
    require(controller.text_edit().kind == TextEditKind::Inactive, "escape did not end the edit session");
    require(controller.workspace().document().find_object(1)->name == "Alpha", "escape should not have applied the rename");
}

void test_group_and_ungroup() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.workspace().select_object(1);
    controller.workspace().add_to_selection(2);
    require(controller.dispatch_action("edit.group"), "group failed");
    require(controller.workspace().document().objects().size() == 3, "group did not add a group object");
    const auto groupId = controller.workspace().selected_object();
    require(groupId.has_value(), "group did not select the new group object");
    require(controller.workspace().document().find_object(1)->parent == groupId, "child 1 not reparented under the group");
    require(controller.workspace().document().find_object(2)->parent == groupId, "child 2 not reparented under the group");
    require(controller.workspace().document().children_of(*groupId).size() == 2, "group does not report two children");

    controller.workspace().select_object(*groupId);
    require(controller.dispatch_action("edit.ungroup"), "ungroup failed");
    require(controller.workspace().document().objects().size() == 2, "ungroup did not remove the empty group node");
    require(!controller.workspace().document().find_object(1)->parent, "child 1 was not released to root");
    require(!controller.workspace().document().find_object(2)->parent, "child 2 was not released to root");

    require(controller.workspace().commands().undo(controller.workspace().document()).success, "ungroup undo failed");
    require(controller.workspace().document().objects().size() == 3, "ungroup undo did not restore the group node");
    require(controller.workspace().commands().undo(controller.workspace().document()).success, "group undo failed");
    require(controller.workspace().document().objects().size() == 2, "group undo did not remove the group node");
    require(!controller.workspace().document().find_object(1)->parent, "group undo did not release child 1");

    // Ungrouping a selection with no children at all must fail cleanly, not silently no-op.
    controller.workspace().select_object(1);
    require(!controller.dispatch_action("edit.ungroup"), "ungroup should fail with no group in the selection");
}

void test_nested_group_integrity_and_locking() {
    EditorDocument document("Nested Groups");
    EditorObject parent(100, "Parent");
    parent.voxels->set_voxel({0, 0, 0}, 1);
    document.add_object(std::move(parent));
    EditorObject child(101, "Child");
    child.parent = 100;
    child.voxels->set_voxel({1, 0, 0}, 2);
    document.add_object(std::move(child));
    EditorObject grandchild(102, "Grandchild");
    grandchild.parent = 101;
    grandchild.voxels->set_voxel({2, 0, 0}, 3);
    document.add_object(std::move(grandchild));
    document.mark_clean();

    NativeEditorController controller{EditorWorkspace(std::move(document))};
    controller.workspace().select_object(100);
    controller.workspace().add_to_selection(101);
    require(controller.dispatch_action("edit.group"), "grouping a parent-plus-child selection failed");
    const auto newGroup = controller.workspace().selected_object();
    require(newGroup.has_value(), "group did not select its new parent");
    require(controller.workspace().document().find_object(100)->parent == newGroup,
            "selected hierarchy root was not placed under the new group");
    require(controller.workspace().document().find_object(101)->parent == 100U,
            "grouping parent and child flattened the existing hierarchy");
    require(controller.workspace().document().find_object(102)->parent == 101U,
            "grouping changed an unselected descendant relationship");
    require(controller.dispatch_action("edit.undo"), "group hierarchy undo failed");

    // Locked targets must reject rename and hierarchy mutation, matching the rest of the
    // editor command system rather than allowing lock bypass through these newer commands.
    controller.workspace().document().find_object(101)->flags.locked = true;
    CommandResult renameResult = controller.workspace().commands().execute(
        controller.workspace().document(),
        std::make_unique<RenameObjectCommand>(101, "Child", "Renamed"));
    require(!renameResult.success, "locked object rename unexpectedly succeeded");
    require(controller.workspace().document().find_object(101)->name == "Child",
            "failed locked rename changed the name");
    CommandResult reparentResult = controller.workspace().commands().execute(
        controller.workspace().document(),
        std::make_unique<ReparentObjectsCommand>(
            std::vector<ObjectReparentChange>{{101, 100U, std::nullopt}}, "Reparent locked object"));
    require(!reparentResult.success, "locked object reparent unexpectedly succeeded");
    require(controller.workspace().document().find_object(101)->parent == 100U,
            "failed locked reparent changed the hierarchy");
    controller.workspace().document().find_object(101)->flags.locked = false;

    // Nested empty groups selected together must promote the surviving child out of the
    // complete dissolve chain before either group is deleted.
    EditorDocument nested("Nested Ungroup");
    EditorObject groupA(200, "Group A");
    groupA.flags.collisionEnabled = false;
    nested.add_object(std::move(groupA));
    EditorObject groupB(201, "Group B");
    groupB.parent = 200;
    groupB.flags.collisionEnabled = false;
    nested.add_object(std::move(groupB));
    EditorObject survivor(202, "Survivor");
    survivor.parent = 201;
    survivor.voxels->set_voxel({0, 0, 0}, 1);
    nested.add_object(std::move(survivor));
    nested.mark_clean();

    NativeEditorController ungroup{EditorWorkspace(std::move(nested))};
    ungroup.workspace().select_object(200);
    ungroup.workspace().add_to_selection(201);
    require(ungroup.dispatch_action("edit.ungroup"), "nested ungroup failed");
    require(ungroup.workspace().document().objects().size() == 1,
            "nested ungroup did not remove both empty groups");
    require(ungroup.workspace().document().find_object(202) != nullptr,
            "nested ungroup deleted the surviving child");
    require(!ungroup.workspace().document().find_object(202)->parent,
            "nested ungroup left the survivor attached to a deleted group");
    std::string validationError;
    require(ungroup.workspace().document().validate(&validationError), validationError.c_str());
    require(ungroup.dispatch_action("edit.undo"), "nested ungroup undo failed");
    require(ungroup.workspace().document().objects().size() == 3,
            "nested ungroup undo did not restore both groups");
    require(ungroup.workspace().document().find_object(201)->parent == 200U,
            "nested ungroup undo did not restore the inner group parent");
    require(ungroup.workspace().document().find_object(202)->parent == 201U,
            "nested ungroup undo did not restore the survivor parent");
}

void test_snap_and_view_presets() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    const float initialSnap = controller.workspace().preferences().translateSnapMeters;
    require(controller.dispatch_action("view.increase_snap"), "increase snap failed");
    require(controller.workspace().preferences().translateSnapMeters > initialSnap, "snap did not increase");
    require(controller.dispatch_action("view.decrease_snap"), "decrease snap failed");
    require(controller.dispatch_action("view.decrease_snap"), "decrease snap failed");
    require(controller.workspace().preferences().translateSnapMeters < initialSnap, "snap did not decrease below the start value");
    require(controller.workspace().preferences().translateSnapMeters > 0.0F, "snap must never reach zero or go negative");

    const float initialAngleSnap = controller.workspace().preferences().rotateSnapDegrees;
    require(controller.dispatch_action("view.increase_angle_snap"), "increase angle snap failed");
    require(controller.workspace().preferences().rotateSnapDegrees > initialAngleSnap, "angle snap did not increase");

    controller.workspace().select_object(1);
    controller.workspace().add_to_selection(2);
    require(controller.dispatch_action("view.top"), "view.top failed");
    require(controller.camera().projection == EditorProjection::Orthographic, "top view should be orthographic");
    require(controller.dispatch_action("view.perspective"), "view.perspective failed");
    require(controller.camera().projection == EditorProjection::Perspective, "perspective view did not restore perspective projection");
}

void test_quit_confirmation() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    // A document just assembled via add_object() legitimately counts as having unsaved
    // content; mark_clean() is what "matches the last save" actually means, so that is the
    // scenario to construct for the "quits immediately" path.
    controller.workspace().document().mark_clean();
    require(!controller.workspace().document().dirty(), "mark_clean() should clear the dirty flag");
    require(controller.dispatch_action("file.exit"), "file.exit failed on a clean document");
    require(controller.quit_requested(), "clean document should quit immediately, no confirmation needed");

    NativeEditorController dirtyController{EditorWorkspace(make_two_object_document())};
    dirtyController.workspace().select_object(1);
    require(dirtyController.dispatch_action("edit.duplicate"), "setup duplicate failed");
    require(dirtyController.workspace().document().dirty(), "document should be dirty after an edit");

    dirtyController.request_quit();
    require(dirtyController.pending_quit_confirmation(), "dirty document should require quit confirmation");
    require(!dirtyController.quit_requested(), "quit must not proceed until confirmed");
    dirtyController.key_down("escape", false, false, false);
    require(!dirtyController.pending_quit_confirmation(), "escape did not cancel the quit confirmation");
    require(!dirtyController.quit_requested(), "cancelled quit must not proceed");

    dirtyController.request_quit();
    dirtyController.key_down("return", false, false, false);
    require(dirtyController.quit_requested(), "confirming quit did not set the quit flag");
}

void test_marquee_select() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.resize(1280, 800);
    controller.set_active_tool(EditorToolId::Select);
    controller.workspace().clear_selection();
    controller.frame_selection();

    const UiRect viewport = controller.layout().viewport;
    const int left = viewport.x + 2;
    const int top = viewport.y + 2;
    const int right = viewport.x + viewport.width - 2;
    const int bottom = viewport.y + viewport.height - 2;
    // A marquee spanning virtually the whole viewport must catch both framed objects.
    controller.pointer_down(PointerButton::Primary, left, top);
    require(controller.marquee().active, "marquee did not start on an empty-space drag");
    controller.pointer_move(right, bottom);
    controller.pointer_up(PointerButton::Primary, right, bottom);
    require(!controller.marquee().active, "marquee did not end on pointer_up");
    require(controller.workspace().selection_count() == 2, "marquee drag over both objects should select both");

    // A plain click (no real drag) on empty space clears the selection instead of marquee-selecting.
    controller.pointer_down(PointerButton::Primary, left, top);
    controller.pointer_up(PointerButton::Primary, left + 1, top + 1);
    require(controller.workspace().selection_count() == 0, "a plain click on empty space should clear the selection");
}

void test_context_menu_and_hierarchy_filter() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.resize(1280, 800);

    // Right-click without dragging on a hierarchy row opens a context menu targeting that row.
    require(!controller.layout().hierarchyRows.empty(), "expected at least one hierarchy row");
    const UiRect row = controller.layout().hierarchyRows.front();
    const int rx = row.x + 5;
    const int ry = row.y + row.height / 2;
    controller.pointer_down(PointerButton::Secondary, rx, ry);
    controller.pointer_up(PointerButton::Secondary, rx, ry);
    require(controller.context_menu().open, "right-click without dragging should open a context menu");
    require(!controller.context_menu().items.empty(), "context menu should offer at least one action");
    require(controller.workspace().is_selected(1), "opening the context menu should select its target");

    // Clicking a context menu item runs that action and closes the menu.
    std::optional<std::size_t> deleteIndex;
    for (std::size_t i = 0; i < controller.context_menu().items.size(); ++i) {
        if (controller.context_menu().items[i].actionId == "edit.duplicate") deleteIndex = i;
    }
    require(deleteIndex.has_value(), "context menu should offer Duplicate for an object target");
    const UiRect itemRect = controller.context_menu().itemRects[*deleteIndex];
    controller.pointer_down(PointerButton::Primary, itemRect.x + 5, itemRect.y + itemRect.height / 2);
    require(!controller.context_menu().open, "clicking a context menu item should close it");
    require(controller.workspace().document().objects().size() == 3, "context menu Duplicate did not run");

    // Hierarchy filter: typing narrows the row list live, matching hierarchy_order().
    require(controller.layout().hierarchyFilterBox.width > 0, "hierarchy filter box should exist when the panel is visible");
    const UiRect filterBox = controller.layout().hierarchyFilterBox;
    controller.pointer_down(PointerButton::Primary, filterBox.x + 5, filterBox.y + filterBox.height / 2);
    require(controller.text_edit().kind == TextEditKind::HierarchyFilter, "clicking the filter box did not begin filtering");
    for (char c : std::string("Beta")) controller.text_input(c);
    require(controller.hierarchy_filter() == "Beta", "filter text did not apply live");
    const auto filtered = controller.hierarchy_order();
    require(filtered.size() == 1 && filtered.front() == 2, "filter did not narrow the hierarchy to the matching object");
    controller.key_down("escape", false, false, false);
    require(controller.hierarchy_filter().empty(), "escape should clear an in-progress filter");
    require(controller.hierarchy_order().size() == 3, "clearing the filter should restore the full hierarchy");
}

void test_menu_navigation_and_small_window_layout() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.workspace().preferences().uiScale = 3.0F;
    controller.resize(640, 480);

    controller.key_down("h", false, false, true);
    require(controller.open_menu() && *controller.open_menu() == "Help",
            "Alt+H did not open the Help menu");
    require(controller.menu_hovered_action().has_value(),
            "opening a menu did not select its first enabled action");
    const std::size_t first = *controller.menu_hovered_action();
    controller.key_down("down", false, false, false);
    require(controller.menu_hovered_action() && *controller.menu_hovered_action() != first,
            "Down did not advance menu selection");
    controller.key_down("left", false, false, false);
    require(controller.open_menu() && *controller.open_menu() == "Window",
            "Left did not switch to the adjacent menu");
    controller.key_down("escape", false, false, false);
    controller.key_down("w", false, false, true);
    require(controller.open_menu() && *controller.open_menu() == "Window",
            "Alt+W did not open the Window menu");

    const NativeMenuPopupLayout initial = controller.menu_popup_layout();
    require(initial.popup.x >= 0 && initial.popup.y >= 0 &&
            initial.popup.x + initial.popup.width <= 640 &&
            initial.popup.y + initial.popup.height <= 480,
            "menu popup escaped a small window");
    const auto windowActions = controller.workspace().menus().menu("Window");
    if (windowActions.size() > initial.rows.size()) {
        controller.pointer_wheel(-1.0F, initial.popup.x + 5, initial.popup.y + 5);
        require(controller.menu_popup_layout().firstVisibleAction > initial.firstVisibleAction,
                "wheel input did not scroll a clipped menu");
    }

    // Hovering a different top-level label while a menu is open switches without another click.
    const int menuItemWidth = 640 / static_cast<int>(kMenuBarNames.size());
    controller.pointer_move(menuItemWidth + 4, 4);
    require(controller.open_menu() && *controller.open_menu() == "Edit",
            "menu-bar hover did not switch the open menu");
    controller.key_down("escape", false, false, false);
    require(!controller.open_menu(), "Escape did not close the menu");

    controller.workspace().preferences().uiScale = 1.0F;
    controller.resize(640, 480);
    const UiRect row = controller.layout().hierarchyRows.front();
    controller.pointer_down(PointerButton::Secondary, row.x + 4, row.y + row.height / 2);
    controller.pointer_up(PointerButton::Secondary, row.x + 4, row.y + row.height / 2);
    require(controller.context_menu().open, "context menu did not open");
    require(!controller.context_menu().itemRects.empty(), "context menu has no item geometry");
    const UiRect last = controller.context_menu().itemRects.back();
    require(last.x >= 0 && last.y >= 0 && last.x + last.width <= 640 && last.y + last.height <= 480,
            "context menu escaped a small window");
    controller.key_down("down", false, false, false);
    require(controller.context_menu().hoveredItem == std::optional<std::size_t>(0U),
            "keyboard did not select the first context-menu item");
    controller.key_down("end", false, false, false);
    require(controller.context_menu().hoveredItem ==
                std::optional<std::size_t>(controller.context_menu().items.size() - 1U),
            "End did not select the last context-menu item");
    controller.key_down("escape", false, false, false);
    require(!controller.context_menu().open, "Escape did not close the context menu");
}

void test_hierarchy_drag_reparent() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.resize(1280, 800);
    require(controller.layout().hierarchyRows.size() >= 2, "expected at least two hierarchy rows");
    const auto order = controller.hierarchy_order();
    require(order.size() == 2, "expected exactly two objects in the hierarchy");

    // Drag object 1's row onto object 2's row: pointer_down arms a pending drag, pointer_move
    // past the threshold promotes it, pointer_up on a different row reparents.
    std::size_t sourceIndex = 0;
    while (sourceIndex < order.size() && order[sourceIndex] != 1) ++sourceIndex;
    std::size_t targetIndex = 0;
    while (targetIndex < order.size() && order[targetIndex] != 2) ++targetIndex;
    const UiRect sourceRow = controller.layout().hierarchyRows[sourceIndex];
    const UiRect targetRow = controller.layout().hierarchyRows[targetIndex];

    controller.pointer_down(PointerButton::Primary, sourceRow.x + 10, sourceRow.y + sourceRow.height / 2);
    require(controller.hierarchy_drag().sourceId == 1, "drag was not armed on the pressed row's object");
    require(!controller.hierarchy_drag().active, "drag should not be active before the pointer actually moves");
    controller.pointer_move(targetRow.x + 10, targetRow.y + targetRow.height / 2);
    require(controller.hierarchy_drag().active, "drag did not promote after moving past the threshold");
    controller.pointer_up(PointerButton::Primary, targetRow.x + 10, targetRow.y + targetRow.height / 2);
    require(controller.workspace().document().find_object(1)->parent == 2U, "drag-drop did not reparent object 1 under object 2");
    require(controller.hierarchy_drag().sourceId == 0, "drag state was not cleared after drop");

    require(controller.workspace().commands().undo(controller.workspace().document()).success, "reparent-by-drag undo failed");
    require(!controller.workspace().document().find_object(1)->parent, "undo did not release object 1 back to root");
}

EditorDocument make_hierarchy_document(bool lockGrandchild = false) {
    EditorDocument document("Hierarchy Lifecycle Test");
    EditorObject parent(10, "Parent");
    parent.voxels->set_voxel({0, 0, 0}, 1);
    document.add_object(std::move(parent));
    EditorObject child(11, "Child");
    child.parent = 10;
    child.voxels->set_voxel({1, 0, 0}, 2);
    document.add_object(std::move(child));
    EditorObject grandchild(12, "Grandchild");
    grandchild.parent = 11;
    grandchild.flags.locked = lockGrandchild;
    grandchild.voxels->set_voxel({2, 0, 0}, 3);
    document.add_object(std::move(grandchild));
    document.mark_clean();
    return document;
}

void test_hierarchy_lifecycle_integrity() {
    NativeEditorController controller{EditorWorkspace(make_hierarchy_document())};
    controller.resize(1280, 800);
    controller.workspace().select_object(10);

    require(controller.dispatch_action("edit.delete"), "deleting a hierarchy root failed");
    require(controller.workspace().document().objects().empty(), "root deletion did not remove its full subtree");
    require(controller.layout().hierarchyRows.empty(), "hierarchy layout did not shrink after subtree deletion");
    require(controller.dispatch_action("edit.undo"), "subtree delete undo failed");
    require(controller.workspace().document().objects().size() == 3, "subtree delete undo lost descendants");
    require(controller.workspace().document().find_object(11)->parent == 10U, "child parent was not restored");
    require(controller.workspace().document().find_object(12)->parent == 11U, "grandchild parent was not restored");
    require(controller.layout().hierarchyRows.size() == 3, "hierarchy layout did not restore after undo");

    // Selecting both a root and one of its descendants must still delete the closure once,
    // not dereference the descendant after the root's cascading removal.
    controller.workspace().select_object(10);
    controller.workspace().add_to_selection(11);
    require(controller.dispatch_action("edit.delete"), "nested multi-selection delete failed");
    require(controller.workspace().document().objects().empty(), "nested delete left hierarchy objects behind");
    require(controller.dispatch_action("edit.undo"), "nested delete undo failed");

    // Copying/duplicating a hierarchy remaps internal parent ids to the new hierarchy.
    controller.workspace().select_object(10);
    require(controller.dispatch_action("edit.copy"), "hierarchy copy failed");
    require(controller.dispatch_action("edit.paste"), "hierarchy paste failed");
    require(controller.workspace().document().objects().size() == 6, "hierarchy paste did not clone the subtree");
    const auto pastedRoot = controller.workspace().selected_object();
    require(pastedRoot && *pastedRoot != 10, "pasted hierarchy root was not selected");
    const auto pastedChildren = controller.workspace().document().children_of(*pastedRoot);
    require(pastedChildren.size() == 1, "pasted child was not parented to the pasted root");
    const auto pastedGrandchildren = controller.workspace().document().children_of(pastedChildren.front());
    require(pastedGrandchildren.size() == 1, "pasted grandchild hierarchy was not preserved");

    // Cutting a root copies and removes the full closure; pasting after the original ids are
    // gone must still produce a valid, self-contained hierarchy.
    controller.workspace().select_object(10);
    require(controller.dispatch_action("edit.cut"), "hierarchy cut failed");
    require(!controller.workspace().document().find_object(10), "cut hierarchy root still exists");
    require(!controller.workspace().document().find_object(11), "cut hierarchy child still exists");
    require(!controller.workspace().document().find_object(12), "cut hierarchy grandchild still exists");
    require(controller.dispatch_action("edit.paste"), "paste after hierarchy cut failed");
    std::string validationError;
    require(controller.workspace().document().validate(&validationError), validationError.c_str());

    NativeEditorController locked{EditorWorkspace(make_hierarchy_document(true))};
    locked.workspace().select_object(10);
    require(!locked.dispatch_action("edit.delete"), "locked descendant should protect its containing subtree");
    require(locked.workspace().document().objects().size() == 3, "locked subtree deletion was not atomic");
}

void test_shortcuts_layout_and_numeric_validation() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.resize(1280, 800);
    const std::size_t initialRows = controller.layout().hierarchyRows.size();

    controller.key_down("v", true, true, false); // Ctrl+Shift+V: Create Voxel Object
    require(controller.workspace().document().objects().size() == 3, "registered create shortcut did not dispatch");
    require(controller.layout().hierarchyRows.size() == initialRows + 1,
            "hierarchy rows were stale after shortcut-created object");
    controller.key_down("z", true, false, false);
    require(controller.workspace().document().objects().size() == 2, "shortcut undo did not remove created object");
    require(controller.workspace().selection_count() == 0,
            "structural undo left a stale selection pointing at the removed object");
    require(controller.layout().hierarchyRows.size() == initialRows, "hierarchy rows were stale after undo");
    controller.key_down("y", true, false, false);
    require(controller.workspace().document().objects().size() == 3, "shortcut redo did not restore created object");

    controller.workspace().select_object(1);
    controller.key_down("c", true, false, false);
    controller.key_down("v", true, false, false);
    require(controller.workspace().document().objects().size() == 4, "Ctrl+C / Ctrl+V did not dispatch copy and paste");

    // Numeric fields must reject partial tokens and surplus values instead of accepting
    // strtof's parsed prefix (for example, "1x") or silently ignoring a fourth component.
    controller.workspace().select_object(1);
    const UiRect positionField = controller.layout().inspectorFields.front();
    controller.pointer_down(PointerButton::Primary, positionField.x + 4, positionField.y + 4);
    require(controller.text_edit().kind == TextEditKind::Position, "position field did not begin text editing");
    for (const char c : std::string("1x 2 3 4")) controller.text_input(c);
    controller.key_down("return", false, false, false);
    const Float3 position = controller.workspace().document().find_object(1)->transform.position;
    require(length(position) < 1.0e-6F, "invalid numeric input changed the transform");
    require(controller.status().error, "invalid numeric input did not report an error");

    // Menu Exit must use the same dirty-document confirmation path as WM close and Ctrl+Q.
    controller.workspace().document().mark_dirty();
    require(controller.dispatch_action("file.exit"), "menu Exit dispatch failed");
    require(controller.pending_quit_confirmation(), "menu Exit bypassed dirty-document confirmation");
    require(!controller.quit_requested(), "menu Exit quit before confirmation");
    controller.key_down("escape", false, false, false);
    require(!controller.pending_quit_confirmation(), "Escape did not cancel menu Exit confirmation");
}



void test_native_shortcut_profiles_and_contexts() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    controller.resize(1280, 800);
    controller.set_active_tool(EditorToolId::AddVoxel);
    controller.key_down("w", false, false, false);
    require(controller.active_tool() == EditorToolId::Translate,
            "DVE viewport profile did not route W to Move");

    std::string error;
    require(controller.workspace().shortcuts().set_active_profile("Blank Custom", &error), error.c_str());
    controller.set_active_tool(EditorToolId::AddVoxel);
    controller.key_down("w", false, false, false);
    require(controller.active_tool() == EditorToolId::AddVoxel,
            "blank shortcut profile still executed a legacy hard-coded W command");

    require(controller.workspace().shortcuts().set_active_profile("DVE Default", &error), error.c_str());
    const Float3 before = controller.camera().position;
    const UiRect viewport = controller.layout().viewport;
    const int x = viewport.x + viewport.width / 2;
    const int y = viewport.y + viewport.height / 2;
    controller.pointer_down(PointerButton::Secondary, x, y, 0U);
    controller.key_down("w", false, false, false);
    const Float3 afterPress = controller.camera().position;
    controller.update(0.1F);
    const Float3 afterHold = controller.camera().position;
    require(length(subtract(afterHold, afterPress)) > 1.0e-5F,
            "held fly shortcut did not continue moving during update");
    controller.key_up("w", false, false, false);
    controller.update(0.1F);
    require(length(subtract(controller.camera().position, afterHold)) < 1.0e-6F,
            "released fly shortcut continued moving");
    controller.pointer_up(PointerButton::Secondary, x + 10, y, 0U);
    require(length(subtract(controller.camera().position, before)) > 1.0e-5F,
            "FlyNavigation context did not override viewport W while RMB was held");

    require(controller.dispatch_action("help.shortcuts"), "shortcut editor did not open");
    require(controller.shortcut_panel().open, "shortcut editor panel state was not set");
    controller.text_input("camera");
    require(!controller.shortcut_rows().empty(), "shortcut editor search returned no camera commands");
    const std::string shortcutAccessibility = serialize_accessibility_tree_json(
        build_editor_accessibility_tree(controller));
    require(shortcutAccessibility.find("shortcutEditor") != std::string::npos,
            "shortcut editor was omitted from the accessibility tree");
    controller.key_down("escape", false, false, false);
    require(!controller.shortcut_panel().open, "shortcut editor did not close on Escape");
}

void test_platform_event_bridge() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    EditorPlatformBridge bridge(controller);

    platform::PlatformEvent resize;
    resize.type = platform::EventType::WindowResized;
    resize.width = 1440;
    resize.height = 900;
    bridge.handle_event(resize);
    require(controller.layout().statusBar.width == 1440, "platform resize did not reach editor controller");

    platform::PlatformEvent shortcut;
    shortcut.type = platform::EventType::KeyDown;
    shortcut.key = "c";
    shortcut.modifiers = platform::Modifier::Control;
    controller.workspace().select_object(1);
    bridge.handle_event(shortcut);
    shortcut.key = "v";
    bridge.handle_event(shortcut);
    require(controller.workspace().document().objects().size() == 3,
            "platform shortcut event did not route through the menu registry");

    controller.workspace().select_object(1);
    require(controller.dispatch_action("edit.rename"), "platform UTF-8 rename did not start");
    platform::PlatformEvent utf8;
    utf8.type = platform::EventType::TextInput;
    utf8.text = std::string("M") + "\xC3\xBC" + "nch" + "\xC3\xA9";
    bridge.handle_event(utf8);
    platform::PlatformEvent editKey;
    editKey.type = platform::EventType::KeyDown;
    editKey.key = "backspace";
    bridge.handle_event(editKey);
    editKey.key = "return";
    bridge.handle_event(editKey);
    require(controller.workspace().document().find_object(1)->name ==
                std::string("M") + "\xC3\xBC" + "nch",
            "UTF-8 text was split or backspace removed only part of a code point");

    controller.workspace().document().mark_dirty();
    platform::PlatformEvent quit;
    quit.type = platform::EventType::QuitRequested;
    bridge.handle_event(quit);
    require(controller.pending_quit_confirmation(),
            "platform quit event bypassed dirty-document confirmation");
}


void test_native_ai_assistant_panel() {
    NativeEditorController controller{EditorWorkspace(make_two_object_document())};
    require(controller.workspace().ai_assistant().attached(), "AI assistant client was not attached");
    require(controller.dispatch_action("window.toggle_ai_assistant"), "AI assistant menu action failed");
    require(controller.bottom_tab() == BottomPanelTab::Assistant, "AI assistant tab did not open");
    require(controller.workspace().panel_visible(PanelId::Assistant), "AI assistant panel visibility was not set");
    require(controller.layout().aiPromptBox.width > 0, "AI assistant prompt field was not laid out");
    require(controller.layout().aiSendButton.width > 0, "AI assistant send button was not laid out");
    auto* bridge = controller.ai_bridge();
    require(bridge != nullptr, "AI bridge was not created");
    require(bridge->registry().find("dve.editor.list_objects") != nullptr,
            "editor AI tools were not registered");
    const auto context = bridge->registry().call("dve.editor.context", ai::JsonValue::Object{},
        ai::AiApprovalPolicy::ReadOnlyOnly, {}, "test");
    require(context.status == ai::AiCallStatus::Completed, "editor context resource failed");
    require(context.content.find("object_count") != nullptr, "editor context omitted object count");
    const auto settings = bridge->registry().call("dve.editor.list_settings",
        ai::JsonValue::Object{{"changed_only", false}},
        ai::AiApprovalPolicy::ReadOnlyOnly, {}, "test");
    require(settings.status == ai::AiCallStatus::Completed, "editor settings resource failed");
    require(settings.content.find("settings") != nullptr, "editor settings resource omitted rows");
    const auto list = bridge->registry().call("dve.editor.list_objects", ai::JsonValue::Object{},
        ai::AiApprovalPolicy::ReadOnlyOnly, {}, "test");
    require(list.status == ai::AiCallStatus::Completed, "editor object listing failed");
    const EditorObjectId objectId = controller.workspace().document().objects().begin()->first;
    ai::JsonValue transformArgs = ai::JsonValue::Object{
        {"id", static_cast<double>(objectId)}, {"position", ai::JsonValue::Array{9, 8, 7}}};
    const auto pending = bridge->registry().call("dve.editor.set_transform", transformArgs,
        ai::AiApprovalPolicy::AskForChanges, {}, "test");
    require(pending.status == ai::AiCallStatus::ApprovalRequired, "editor transform bypassed approval");
    require(bridge->registry().approvals().approve(pending.approvalId), "editor transform approval failed");
    const auto moved = bridge->registry().call("dve.editor.set_transform", transformArgs,
        ai::AiApprovalPolicy::AskForChanges, pending.approvalId, "test");
    require(moved.status == ai::AiCallStatus::Completed, "approved editor transform failed");
    require(controller.workspace().document().find_object(objectId)->transform.position.x == 9.0F,
            "approved editor transform was not applied");
    require(controller.workspace().commands().undo(controller.workspace().document()).success,
            "AI editor transform was not undoable");
    require(controller.dispatch_action("window.toggle_ai_assistant"), "AI assistant close action failed");
    require(controller.bottom_tab() != BottomPanelTab::Assistant, "AI assistant tab did not close");
}

void test_game_ui() {
    using namespace dve::ui;
    GameSettings settings;
    settings.reducedMotion = true;
    settings.colorVisionMode = "deuteranopia";
    std::string error;
    auto parsed = GameSettings::parse(settings.serialize(), &error);
    require(parsed && parsed->reducedMotion, "game settings round trip failed");
    InputBindingMap bindings;
    require(bindings.bind("jump", "keyboard", "Space", &error), error.c_str());
    require(!bindings.bind("interact", "keyboard", "Space", &error), "input conflict accepted");
    require(bindings.bind("jump", "gamepad", "A", &error), error.c_str());
    GameMenuModel menu;
    menu.open(GameMenuScreen::Pause);
    require(menu.actions().size() >= 5, "pause menu incomplete");
    GameHudModel hud;
    hud.set_tools({{"hammer","Hammer",true},{"bomb","Bomb",false},{"cutter","Cutter",true}});
    require(hud.selected_tool()->id == "hammer", "initial tool wrong");
    require(hud.select_next_tool() && hud.selected_tool()->id == "cutter", "tool wheel did not skip disabled tool");
}

} // namespace

int main() {
    try {
        const auto root = make_temp();
        test_commands_and_tools();
        test_document_project_and_recovery(root / "project");
        test_workspace_menus_and_preferences();
        test_tasks();
        test_import_workflow(root / "cooked");
        test_live_import_preview(root / "preview");
        test_background_object_diagnostics();
        test_new_project_template();
        test_file_workflow_and_recent_projects(root / "recent");
        test_materials_viewport_and_native_controller(root / "native");
        test_voxel_rescale_command();
        test_multi_selection_transforms_and_diagnostics();
        test_object_lifecycle_actions();
        test_duplicate_and_rename();
        test_group_and_ungroup();
        test_nested_group_integrity_and_locking();
        test_snap_and_view_presets();
        test_quit_confirmation();
        test_marquee_select();
        test_context_menu_and_hierarchy_filter();
        test_menu_navigation_and_small_window_layout();
        test_hierarchy_drag_reparent();
        test_hierarchy_lifecycle_integrity();
        test_shortcuts_layout_and_numeric_validation();
        test_native_shortcut_profiles_and_contexts();
        test_platform_event_bridge();
        test_native_ai_assistant_panel();
        test_game_ui();
        std::cout << "dve_editor_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
