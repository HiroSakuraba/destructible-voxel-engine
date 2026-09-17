#include "dve/editor_asset_browser.hpp"
#include "dve/editor_command.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_prefab.hpp"
#include "dve/game_world.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace dve;
using namespace dve::editor;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(Float3 a, Float3 b, float epsilon = 1.0e-5F) {
    return std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon &&
           std::abs(a.z - b.z) <= epsilon;
}

EditorObject make_voxel_object(EditorObjectId id, std::string name, Float3 position) {
    EditorObject object(id, std::move(name));
    object.transform.position = position;
    object.voxelSizeMeters = 0.5F;
    object.voxels->set_voxel({0, 0, 0}, kDefaultSurfaceMaterial);
    return object;
}

EditorDocument make_document() {
    EditorDocument document("Composition Fixture");
    EditorObject parent = make_voxel_object(10U, "Vehicle", {10.0F, 0.0F, 0.0F});
    Component health;
    health.id = 1U;
    health.type = "game.health";
    health.properties.emplace("maximum", std::int64_t{100});
    health.properties.emplace("regeneration", 2.5);
    parent.components.push_back(std::move(health));
    document.add_object(std::move(parent));

    EditorObject child = make_voxel_object(11U, "Turret", {12.0F, 1.0F, 0.0F});
    Component script;
    script.id = 1U;
    script.type = "dve.script";
    script.properties.emplace("asset", std::string{"scripts/turret.lua"});
    child.components.push_back(std::move(script));
    document.add_object(std::move(child));

    std::string error;
    require(document.attach_object(11U, 10U, true, "turret_mount", true, true, &error), error.c_str());
    document.mark_clean();
    return document;
}

void test_editor_attachment_component_and_scene_roundtrip(const std::filesystem::path& root) {
    EditorDocument document = make_document();
    const EditorObject* child = document.find_object(11U);
    require(child && child->attachment, "child attachment was not created");
    require(near(child->attachment->localTransform.position, {2.0F, 1.0F, 0.0F}),
            "attachment local transform was not derived from the preserved world transform");

    RigidTransform parentWorld = *document.world_transform(10U);
    parentWorld.position = {20.0F, 0.0F, 0.0F};
    require(document.set_world_transform(10U, parentWorld), "parent world transform update failed");
    require(near(document.world_transform(11U)->position, {22.0F, 1.0F, 0.0F}),
            "attached child did not follow its parent");

    RigidTransform childWorld = *document.world_transform(11U);
    childWorld.position = {25.0F, 3.0F, 0.0F};
    require(document.set_world_transform(11U, childWorld), "child world transform update failed");
    require(near(document.find_object(11U)->attachment->localTransform.position, {5.0F, 3.0F, 0.0F}),
            "child world edit did not update its attachment-local transform");

    Component arbitrary;
    arbitrary.type = "studio.targeting";
    arbitrary.properties.emplace("range", 250.0);
    arbitrary.properties.emplace("enabled", true);
    std::string error;
    Component* added = document.add_component(11U, std::move(arbitrary), &error);
    require(added && added->id == 2U, error.c_str());
    require(document.set_component_property(11U, added->id, "mode", std::string{"predictive"}, &error),
            error.c_str());

    const auto scenePath = root / "composition.dvescene";
    const auto saved = document.save_transactional(scenePath);
    require(saved.success, saved.error.c_str());
    auto loaded = EditorDocument::load(scenePath, &error);
    require(loaded.has_value(), error.c_str());
    require(loaded->find_object(11U)->attachment.has_value(), "attachment did not survive scene v4 round-trip");
    require(near(loaded->world_transform(11U)->position, {25.0F, 3.0F, 0.0F}),
            "attached world transform changed after scene round-trip");
    const Component* loadedComponent = loaded->find_component(11U, 2U);
    require(loadedComponent && std::get<std::string>(loadedComponent->properties.at("mode")) == "predictive",
            "open component data did not survive scene round-trip");
}

void test_prefab_capture_instantiation_and_overrides(const std::filesystem::path& root) {
    EditorDocument source = make_document();
    const EditorObjectId rootId = 10U;
    const auto prefabPath = root / "prefabs" / "vehicle.dveprefab";
    const auto captured = capture_editor_prefab(source, std::span<const EditorObjectId>(&rootId, 1U),
                                                prefabPath, "Vehicle Prefab");
    require(captured.success, captured.error.c_str());
    require(captured.objectCount == 2U, "prefab capture did not include the child subtree");

    std::string error;
    auto prefab = load_editor_prefab(prefabPath, &error);
    require(prefab.has_value(), error.c_str());
    require(prefab->rootTemplateObjectIds.size() == 1U, "prefab root list changed on load");

    EditorDocument destination("Prefab Destination");
    const RigidTransform instanceTransform = make_rigid_transform({100.0F, 5.0F, 0.0F}, {});
    auto instance = instantiate_editor_prefab(destination, *prefab, instanceTransform);
    require(instance.success, instance.error.c_str());
    require(instance.objectIds.size() == 2U && instance.rootObjectIds.size() == 1U,
            "prefab instance object count is wrong");
    const EditorObjectId instanceRoot = instance.rootObjectIds.front();
    const auto children = destination.children_of(instanceRoot);
    require(children.size() == 1U, "prefab child hierarchy was not instantiated");
    require(near(destination.world_transform(instanceRoot)->position, {100.0F, 5.0F, 0.0F}),
            "prefab instance root transform is wrong");
    require(near(destination.world_transform(children.front())->position, {102.0F, 6.0F, 0.0F}),
            "prefab child did not retain its attachment-local transform");

    require(apply_editor_prefab_override(destination, children.front(), "name",
                                         std::string{"Commander Turret"}, &error), error.c_str());
    require(destination.find_object(children.front())->name == "Commander Turret",
            "prefab name override was not applied");
    require(destination.find_object(children.front())->prefabLink->overrides.contains("name"),
            "prefab override provenance was not recorded");

    const auto scenePath = root / "prefab_instance.dvescene";
    const auto saved = destination.save_transactional(scenePath);
    require(saved.success, saved.error.c_str());
    auto loaded = EditorDocument::load(scenePath, &error);
    require(loaded.has_value(), error.c_str());
    const auto loadedObjects = loaded->prefab_instance_objects(instance.instanceId);
    require(loadedObjects.size() == 2U, "prefab instance links did not survive scene round-trip");

    EditorDocument commandDocument("Undoable Prefab Destination");
    EditorCommandStack commands;
    auto instantiateCommand = std::make_unique<InstantiateEditorPrefabCommand>(
        std::move(*prefab), make_rigid_transform({40.0F, 0.0F, 0.0F}, {}));
    require(commands.execute(commandDocument, std::move(instantiateCommand)).success,
            "undoable prefab instantiation failed");
    require(commandDocument.objects().size() == 2U, "undoable prefab command created the wrong object count");
    require(commands.undo(commandDocument).success, "prefab-instantiation undo failed");
    require(commandDocument.objects().empty(), "prefab-instantiation undo left objects behind");
    require(commands.redo(commandDocument).success, "prefab-instantiation redo failed");
    require(commandDocument.objects().size() == 2U, "prefab-instantiation redo did not restore objects");
}


void test_editor_commands_and_prefab_asset_kind() {
    require(classify_editor_asset("vehicle.dveprefab") == EditorAssetKind::Prefab,
            "asset browser did not classify prefab assets");
    require(editor_asset_is_text("vehicle.dveprefab"),
            "prefab descriptors must participate in text-reference repair");

    EditorDocument document = make_document();
    EditorCommandStack commands;

    Component inventory;
    inventory.type = "game.inventory";
    inventory.properties.emplace("capacity", std::int64_t{8});
    auto add = std::make_unique<AddComponentCommand>(11U, inventory);
    require(commands.execute(document, std::move(add)).success, "add-component command failed");
    const Component* added = find_component_by_type(
        std::span<const Component>(document.find_object(11U)->components), "game.inventory");
    require(added != nullptr, "add-component command did not publish the component");
    const ComponentId inventoryId = added->id;

    auto edit = std::make_unique<SetComponentPropertyCommand>(
        11U, inventoryId, "capacity", ComponentValue{std::int64_t{8}},
        ComponentValue{std::int64_t{16}});
    require(commands.execute(document, std::move(edit)).success,
            "component-property command failed");
    require(std::get<std::int64_t>(document.find_component(11U, inventoryId)->properties.at("capacity")) == 16,
            "component-property command did not apply");
    require(commands.undo(document).success, "component-property undo failed");
    require(std::get<std::int64_t>(document.find_component(11U, inventoryId)->properties.at("capacity")) == 8,
            "component-property undo did not restore the value");
    require(commands.redo(document).success, "component-property redo failed");

    const auto attached = capture_object_attachment_state(document, 11U);
    require(attached && attached->attachment, "attachment command fixture is not attached");
    ObjectAttachmentState detached;
    detached.worldTransform = *document.world_transform(11U);
    auto detach = std::make_unique<SetObjectAttachmentCommand>(11U, *attached, detached, "Detach object");
    require(commands.execute(document, std::move(detach)).success, "attachment command did not detach");
    require(!document.find_object(11U)->attachment, "attachment command left attachment metadata");
    require(commands.undo(document).success, "attachment-command undo failed");
    require(document.find_object(11U)->attachment.has_value(), "attachment-command undo did not reattach");

    const RigidTransform before = *document.world_transform(11U);
    RigidTransform after = before;
    after.position = {17.0F, 4.0F, 0.0F};
    require(commands.execute(document, std::make_unique<TransformObjectCommand>(11U, before, after)).success,
            "attached transform command failed");
    require(near(document.find_object(11U)->attachment->localTransform.position, {7.0F, 4.0F, 0.0F}),
            "attached transform command did not update local attachment coordinates");
}

void test_game_world_components_and_attachments() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc parentDesc;
    parentDesc.name = "Carrier";
    parentDesc.transform.position = {0.0F, 0.0F, 0.0F};
    Component team;
    team.id = 1U;
    team.type = "game.team";
    team.properties.emplace("name", std::string{"blue"});
    parentDesc.components.push_back(team);
    const GameObjectId parent = world.create_object(std::move(parentDesc));
    require(parent != kInvalidGameObjectId, "could not create runtime parent marker");

    GameObjectDesc childDesc;
    childDesc.name = "Sensor";
    childDesc.transform.position = {2.0F, 0.0F, 0.0F};
    const GameObjectId child = world.create_object(std::move(childDesc));
    require(child != kInvalidGameObjectId, "could not create runtime child marker");
    std::string error;
    require(world.attach_object(child, parent, true, "sensor_mount", true, true, &error), error.c_str());
    require(world.set_position(parent, {10.0F, 0.0F, 0.0F}), "could not move runtime parent");
    require(near(world.position(child).value(), {12.0F, 0.0F, 0.0F}),
            "runtime child did not follow parent attachment");
    require(world.find_by_component("game.team").size() == 1U,
            "runtime component query did not find the parent");

    Component sensor;
    sensor.type = "game.sensor";
    sensor.properties.emplace("radius", 15.0);
    Component* added = world.add_component(child, std::move(sensor), &error);
    require(added != nullptr, error.c_str());
    require(world.set_component_property(child, added->id, "active", true, &error), error.c_str());
    require(std::get<bool>(world.component(child, added->id)->properties.at("active")),
            "runtime component property update failed");

    GameObjectDesc dynamicDesc;
    dynamicDesc.name = "Attached Crate";
    dynamicDesc.transform.position = {13.0F, 0.0F, 0.0F};
    dynamicDesc.dynamic = true;
    dynamicDesc.voxels = std::make_unique<VoxelObject>(77U);
    dynamicDesc.voxels->set_voxel({0, 0, 0}, kDefaultSurfaceMaterial);
    const GameObjectId dynamicChild = world.create_object(std::move(dynamicDesc), &error);
    require(dynamicChild != kInvalidGameObjectId, error.c_str());
    require(world.attach_object(dynamicChild, parent, true, "cargo", true, true, &error), error.c_str());
    require(!world.apply_impulse(dynamicChild, {1.0F, 0.0F, 0.0F}),
            "attached dynamic bodies must reject independent impulses");
    require(world.set_position(parent, {11.0F, 0.0F, 0.0F}), "could not move attached-body parent");
    world.tick(1.0F / 60.0F);
    require(near(world.position(dynamicChild).value(), {14.0F, 0.0F, 0.0F}),
            "attached dynamic body did not hard-synchronize after physics");

    require(world.detach_object(child, true, &error), error.c_str());
    require(world.set_position(parent, {20.0F, 0.0F, 0.0F}), "could not move detached parent");
    require(near(world.position(child).value(), {13.0F, 0.0F, 0.0F}),
            "detached child did not preserve its world transform");
}

void test_membership_native_component_editing_and_prefab_updates(const std::filesystem::path& root) {
    EditorDocument document = make_document();
    std::string error;
    require(document.set_membership(11U, {"enemy", "turret", "enemy"}, 7U,
                                    {"defenses", "vehicles"}, &error), error.c_str());
    require(document.find_by_tag("enemy") == std::vector<EditorObjectId>{11U},
            "editor tag query failed");
    require(document.find_by_group("defenses") == std::vector<EditorObjectId>{11U},
            "editor group query failed");
    require(document.find_by_layer(7U) == std::vector<EditorObjectId>{11U},
            "editor layer query failed");
    const Component* membership = find_component_by_type(
        std::span<const Component>(document.find_object(11U)->components), "dve.membership");
    require(membership != nullptr, "membership component was not materialized");

    const auto membershipScene = root / "membership_v5.dvescene";
    const auto membershipSaved = document.save_transactional(membershipScene);
    require(membershipSaved.success, membershipSaved.error.c_str());
    auto membershipLoaded = EditorDocument::load(membershipScene, &error);
    require(membershipLoaded.has_value(), error.c_str());
    require(membershipLoaded->find_object(11U)->layer == 7U,
            "scene v5 did not preserve object layer");
    require(membershipLoaded->find_object(11U)->tags == std::vector<std::string>({"enemy", "turret"}),
            "scene v5 did not preserve normalized tags");

    NativeEditorController controller(EditorWorkspace{clone_editor_document(document)});
    controller.workspace().select_object(11U);
    require(controller.add_component_to_primary("dve.lifecycle").success,
            "native component add failed");
    const Component* lifecycle = find_component_by_type(
        std::span<const Component>(controller.workspace().document().find_object(11U)->components),
        "dve.lifecycle");
    require(lifecycle != nullptr, "native component add did not publish lifecycle component");
    const ComponentId lifecycleId = lifecycle->id;
    require(controller.set_component_property_text_on_primary(lifecycleId, "spawn", "false").success,
            "native typed component property edit failed");
    require(!std::get<bool>(controller.workspace().document().find_component(11U, lifecycleId)->properties.at("spawn")),
            "native typed component edit produced the wrong value");
    require(controller.set_component_enabled_on_primary(lifecycleId, false).success,
            "native component enable toggle failed");
    require(!controller.workspace().document().find_component(11U, lifecycleId)->enabled,
            "component enable state did not change");
    require(controller.reorder_component_on_primary(lifecycleId, 0U).success,
            "native component reorder failed");
    const auto sections = controller.primary_component_sections();
    require(!sections.empty() && sections.front().id == lifecycleId,
            "native component inspector order did not match the document");
    require(controller.workspace().commands().undo(controller.workspace().document()).success,
            "native component reorder undo failed");
    require(controller.remove_component_from_primary(lifecycleId).success,
            "native component removal failed");

    EditorDocument source = make_document();
    const EditorObjectId sourceRoot = 10U;
    const auto basePath = root / "prefabs" / "updatable_vehicle.dveprefab";
    auto captured = capture_editor_prefab(source, std::span<const EditorObjectId>(&sourceRoot, 1U),
                                          basePath, "Updatable Vehicle");
    require(captured.success, captured.error.c_str());
    auto basePrefab = load_editor_prefab(basePath, &error);
    require(basePrefab.has_value(), error.c_str());

    const EditorPrefabTemplateOverride variantOverride{1U, "name", std::string{"Armored Vehicle"}};
    const auto variantPath = root / "prefabs" / "armored_vehicle.dveprefab";
    const auto variantResult = create_editor_prefab_variant(
        basePath, variantPath, "Armored Vehicle Variant",
        std::span<const EditorPrefabTemplateOverride>(&variantOverride, 1U));
    require(variantResult.success, variantResult.error.c_str());
    auto variant = load_editor_prefab(variantPath, &error);
    require(variant.has_value(), error.c_str());
    require(variant->inheritanceDepth == 1U && !variant->parentPrefabAsset.empty(),
            "prefab variant did not retain parent provenance");
    require(variant->templateDocument.find_object(1U)->name == "Armored Vehicle",
            "prefab variant override was not applied");

    EditorDocument instances("Prefab Update Destination");
    auto instance = instantiate_editor_prefab(instances, *basePrefab,
                                               make_rigid_transform({50.0F, 0.0F, 0.0F}, {}));
    require(instance.success, instance.error.c_str());
    const EditorObjectId instanceRoot = instance.rootObjectIds.front();
    const EditorObjectId instanceChild = instances.children_of(instanceRoot).front();
    require(apply_editor_prefab_override(instances, instanceChild, "name",
                                         std::string{"Custom Turret"}, &error), error.c_str());

    source.find_object(10U)->name = "Vehicle Mk II";
    captured = capture_editor_prefab(source, std::span<const EditorObjectId>(&sourceRoot, 1U),
                                     basePath, "Updatable Vehicle");
    require(captured.success, captured.error.c_str());
    auto updatedPrefab = load_editor_prefab(basePath, &error);
    require(updatedPrefab.has_value(), error.c_str());

    // Stable template IDs do not imply hierarchy order. Add a child whose ID sorts before its
    // newly added parent to verify source propagation resolves dependencies topologically.
    const EditorObjectId updatedRootTemplate = updatedPrefab->rootTemplateObjectIds.front();
    EditorObject lateParent = make_voxel_object(20U, "Sensor Mast", {0.0F, 2.0F, 0.0F});
    updatedPrefab->templateDocument.add_object(std::move(lateParent));
    require(updatedPrefab->templateDocument.attach_object(20U, updatedRootTemplate, true,
                                                           "mast_mount", true, true, &error),
            error.c_str());
    EditorObject earlyChild = make_voxel_object(5U, "Sensor Head", {0.0F, 3.0F, 0.0F});
    updatedPrefab->templateDocument.add_object(std::move(earlyChild));
    require(updatedPrefab->templateDocument.attach_object(5U, 20U, true,
                                                           "sensor_mount", true, true, &error),
            error.c_str());
    updatedPrefab->contentHash = editor_prefab_content_hash(*updatedPrefab);

    const auto preview = preview_editor_prefab_instance_update(instances, instance.instanceId, *updatedPrefab);
    require(preview.stale && preview.success, "prefab source update preview did not detect stale source");
    const auto applied = apply_editor_prefab_instance_update(instances, instance.instanceId, *updatedPrefab);
    require(applied.success, applied.error.c_str());
    require(instances.find_object(instanceRoot)->name == "Vehicle Mk II",
            "prefab source propagation did not update an unmodified property");
    require(instances.find_object(instanceChild)->name == "Custom Turret",
            "prefab source propagation overwrote an instance override");
    require(instances.find_object(instanceRoot)->prefabLink->sourceContentHash == updatedPrefab->contentHash,
            "prefab source propagation did not advance source hash");
    EditorObjectId addedParent = 0U;
    EditorObjectId addedChild = 0U;
    for (const EditorObjectId id : instances.prefab_instance_objects(instance.instanceId)) {
        const EditorObject* object = instances.find_object(id);
        if (!object || !object->prefabLink) continue;
        if (object->prefabLink->templateObjectId == 20U) addedParent = id;
        if (object->prefabLink->templateObjectId == 5U) addedChild = id;
    }
    require(addedParent != 0U && addedChild != 0U,
            "prefab source propagation did not add the dependency-ordered objects");
    require(instances.find_object(addedChild)->parent == addedParent,
            "prefab source propagation created the child before remapping its parent");
}


} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "dve_scene_composition_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    try {
        test_editor_attachment_component_and_scene_roundtrip(root);
        test_prefab_capture_instantiation_and_overrides(root);
        test_editor_commands_and_prefab_asset_kind();
        test_game_world_components_and_attachments();
        test_membership_native_component_editing_and_prefab_updates(root);
        if (std::getenv("DVE_KEEP_TEST_ARTIFACTS") == nullptr) {
            std::filesystem::remove_all(root, ec);
        } else {
            std::cout << "fixture_root=" << root.generic_string() << '\n';
        }
        std::cout << "dve_scene_composition_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_scene_composition_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
