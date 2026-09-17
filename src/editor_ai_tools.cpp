#include "dve/editor_ai_assistant.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "dve/editor_command.hpp"
#include "dve/editor_workspace.hpp"

namespace dve::editor {

namespace {
ai::JsonValue editor_object_schema(ai::JsonValue::Object properties,
                                   std::vector<std::string> required = {}) {
    ai::JsonValue::Array requiredValues;
    for (auto& value : required) requiredValues.emplace_back(std::move(value));
    return ai::JsonValue::Object{{"type", "object"},
                                 {"properties", std::move(properties)},
                                 {"required", std::move(requiredValues)},
                                 {"additionalProperties", false}};
}

ai::JsonValue number_property(std::string description) {
    return ai::JsonValue::Object{{"type", "number"}, {"description", std::move(description)}};
}

ai::JsonValue string_property(std::string description = {}) {
    ai::JsonValue::Object result{{"type", "string"}};
    if (!description.empty()) result.emplace("description", std::move(description));
    return result;
}

ai::JsonValue vector_property(std::size_t count, std::string description) {
    return ai::JsonValue::Object{{"type", "array"},
                                 {"description", std::move(description)},
                                 {"items", ai::JsonValue::Object{{"type", "number"}}},
                                 {"minItems", static_cast<double>(count)},
                                 {"maxItems", static_cast<double>(count)}};
}

std::optional<EditorObjectId> editor_id(const ai::JsonValue* value) {
    if (!value || !value->is_number()) return std::nullopt;
    const double number = value->as_number();
    if (!std::isfinite(number) || number < 1.0 || std::floor(number) != number ||
        number > static_cast<double>(std::numeric_limits<EditorObjectId>::max())) return std::nullopt;
    return static_cast<EditorObjectId>(number);
}

std::optional<Float3> editor_vec3(const ai::JsonValue* value) {
    if (!value || !value->is_array() || value->as_array().size() != 3U) return std::nullopt;
    Float3 result{};
    float* parts[] = {&result.x, &result.y, &result.z};
    for (std::size_t index = 0; index < 3U; ++index) {
        const double number = value->as_array()[index].as_number(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(number) || number < -1.0e8 || number > 1.0e8) return std::nullopt;
        *parts[index] = static_cast<float>(number);
    }
    return result;
}

std::optional<Quaternion> editor_quaternion(const ai::JsonValue* value) {
    if (!value || !value->is_array() || value->as_array().size() != 4U) return std::nullopt;
    Quaternion result{};
    float* parts[] = {&result.x, &result.y, &result.z, &result.w};
    for (std::size_t index = 0; index < 4U; ++index) {
        const double number = value->as_array()[index].as_number(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(number)) return std::nullopt;
        *parts[index] = static_cast<float>(number);
    }
    const float magnitude = std::sqrt(result.x * result.x + result.y * result.y +
                                      result.z * result.z + result.w * result.w);
    if (!(magnitude > 1.0e-6F)) return std::nullopt;
    return Quaternion{result.x / magnitude, result.y / magnitude, result.z / magnitude, result.w / magnitude};
}

ai::JsonValue editor_vec3_json(Float3 value) {
    return ai::JsonValue::Array{value.x, value.y, value.z};
}

ai::JsonValue editor_quaternion_json(Quaternion value) {
    return ai::JsonValue::Array{value.x, value.y, value.z, value.w};
}


ai::JsonValue setting_value_json(const SettingValue& value) {
    if (const auto* boolean = std::get_if<bool>(&value)) return *boolean;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return static_cast<double>(*integer);
    if (const auto* number = std::get_if<double>(&value)) return *number;
    return std::get<std::string>(value);
}

std::string_view editor_mode_name(EditorMode mode) {
    switch (mode) {
        case EditorMode::Edit: return "edit";
        case EditorMode::Simulate: return "simulate";
        case EditorMode::Play: return "play";
    }
    return "unknown";
}

ai::AiToolCallResult command_result(const CommandResult& result, ai::JsonValue content,
                                    std::string successMessage) {
    return {result.success ? ai::AiCallStatus::Completed : ai::AiCallStatus::Failed,
            std::move(content), result.success ? std::move(successMessage) : result.message, {}};
}
} // namespace

void register_editor_ai_tools(ai::DveAiBridge& bridge, EditorWorkspace& workspace) {
    auto& registry = bridge.registry();
    registry.add({"dve.editor.context", "Inspect editor context",
                  "Return the open document, mode, selection, undo state, settings state, shortcut profile, and problem counts.",
                  editor_object_schema({}), ai::AiToolRisk::ReadOnly, true},
        [&workspace](const ai::JsonValue&) {
            ai::JsonValue::Array selection;
            for (const auto id : workspace.selected_objects()) selection.emplace_back(static_cast<double>(id));
            return ai::AiToolCallResult{ai::AiCallStatus::Completed,
                ai::JsonValue::Object{{"document", workspace.document().name()},
                    {"dirty", workspace.document().dirty()},
                    {"mode", std::string(editor_mode_name(workspace.mode()))},
                    {"object_count", static_cast<double>(workspace.document().objects().size())},
                    {"selection", std::move(selection)},
                    {"can_undo", workspace.commands().can_undo()},
                    {"undo_label", std::string(workspace.commands().undo_label())},
                    {"can_redo", workspace.commands().can_redo()},
                    {"redo_label", std::string(workspace.commands().redo_label())},
                    {"changed_setting_count", static_cast<double>(workspace.settings().changed().size())},
                    {"orphaned_setting_count", static_cast<double>(workspace.settings().orphaned_settings().size())},
                    {"shortcut_profile", std::string(workspace.shortcuts().active_profile())},
                    {"shortcut_conflict_count", static_cast<double>(workspace.shortcuts().conflicts(workspace.shortcuts().active_profile()).size())},
                    {"problem_count", static_cast<double>(workspace.problems().problems().size())},
                    {"problem_error_count", static_cast<double>(workspace.problems().error_count())}},
                "editor context inspected", {}};
        });

    registry.add({"dve.editor.list_settings", "List editor settings",
                  "List setting definitions and effective values, optionally restricted to one category or changed values.",
                  editor_object_schema({{"category", string_property("Optional exact category")},
                                        {"changed_only", ai::JsonValue::Object{{"type", "boolean"}}},
                                        {"include_advanced", ai::JsonValue::Object{{"type", "boolean"}}}}),
                  ai::AiToolRisk::ReadOnly, true},
        [&workspace](const ai::JsonValue& arguments) {
            const std::string category = arguments.find("category") ? std::string(arguments.find("category")->as_string()) : std::string{};
            const bool changedOnly = arguments.find("changed_only") && arguments.find("changed_only")->as_bool();
            const bool includeAdvanced = !arguments.find("include_advanced") || arguments.find("include_advanced")->as_bool(true);
            ai::JsonValue::Array rows;
            for (const auto& definition : workspace.settings().definitions()) {
                if (!category.empty() && definition.category != category) continue;
                if (!includeAdvanced && definition.advanced) continue;
                if (changedOnly && !workspace.settings().differs_from_default(definition.id)) continue;
                SettingScope source{}; bool inherited{};
                const SettingValue value = workspace.settings().value(definition.id, &source, &inherited);
                rows.emplace_back(ai::JsonValue::Object{{"id", definition.id}, {"category", definition.category},
                    {"section", definition.section}, {"label", definition.label},
                    {"description", definition.description}, {"value", setting_value_json(value)},
                    {"default", setting_value_json(definition.defaultValue)},
                    {"source_scope", setting_scope_name(source)}, {"inherited", inherited},
                    {"advanced", definition.advanced},
                    {"restart_required", definition.applyPolicy == SettingApplyPolicy::RestartRequired}});
            }
            return ai::AiToolCallResult{ai::AiCallStatus::Completed,
                ai::JsonValue::Object{{"settings", std::move(rows)},
                                      {"orphaned", static_cast<double>(workspace.settings().orphaned_settings().size())}},
                "editor settings listed", {}};
        });

    registry.add({"dve.editor.list_objects", "List editor objects",
                  "List objects in the currently open editor document, including transform, flags, source, and voxel count.",
                  editor_object_schema({}), ai::AiToolRisk::ReadOnly, true},
        [&workspace](const ai::JsonValue&) {
            ai::JsonValue::Array rows;
            for (const auto& [id, object] : workspace.document().objects()) {
                rows.emplace_back(ai::JsonValue::Object{
                    {"id", static_cast<double>(id)}, {"name", object.name},
                    {"position", editor_vec3_json(object.transform.position)},
                    {"visible", object.flags.visible}, {"locked", object.flags.locked},
                    {"source_asset", object.sourceAsset.generic_string()},
                    {"voxel_count", static_cast<double>(object.voxels ? object.voxels->occupied_voxel_count() : 0U)}});
            }
            return ai::AiToolCallResult{ai::AiCallStatus::Completed,
                                        ai::JsonValue::Object{{"document", workspace.document().name()},
                                                              {"dirty", workspace.document().dirty()},
                                                              {"objects", std::move(rows)}},
                                        "editor objects listed", {}};
        });

    registry.add({"dve.editor.inspect_object", "Inspect editor object",
                  "Inspect one object from the currently open editor document.",
                  editor_object_schema({{"id", number_property("Editor object ID")}}, {"id"}),
                  ai::AiToolRisk::ReadOnly, true},
        [&workspace](const ai::JsonValue& arguments) {
            const auto id = editor_id(arguments.find("id"));
            const EditorObject* object = id ? workspace.document().find_object(*id) : nullptr;
            if (!object) return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "unknown editor object", {}};
            return ai::AiToolCallResult{ai::AiCallStatus::Completed,
                ai::JsonValue::Object{{"id", static_cast<double>(*id)}, {"name", object->name},
                    {"position", editor_vec3_json(object->transform.position)},
                    {"rotation", editor_quaternion_json(object->transform.rotation)},
                    {"visible", object->flags.visible}, {"locked", object->flags.locked},
                    {"anchored", object->flags.anchored}, {"structural", object->flags.structural},
                    {"collision", object->flags.collisionEnabled},
                    {"source_asset", object->sourceAsset.generic_string()},
                    {"voxel_count", static_cast<double>(object->voxels ? object->voxels->occupied_voxel_count() : 0U)}},
                "editor object inspected", {}};
        });

    registry.add({"dve.editor.select_object", "Select editor object",
                  "Select one editor object so the user can see and inspect it.",
                  editor_object_schema({{"id", number_property("Editor object ID")}}, {"id"}),
                  ai::AiToolRisk::Mutating, true},
        [&workspace](const ai::JsonValue& arguments) {
            const auto id = editor_id(arguments.find("id"));
            if (!id || !workspace.document().find_object(*id))
                return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "unknown editor object", {}};
            workspace.select_object(*id);
            return ai::AiToolCallResult{ai::AiCallStatus::Completed,
                                        ai::JsonValue::Object{{"id", static_cast<double>(*id)}},
                                        "editor object selected", {}};
        });

    registry.add({"dve.editor.set_transform", "Transform editor object",
                  "Set position and/or normalized quaternion rotation through the editor undo stack.",
                  editor_object_schema({{"id", number_property("Editor object ID")},
                                        {"position", vector_property(3, "World position")},
                                        {"rotation", vector_property(4, "Quaternion [x,y,z,w]")}}, {"id"}),
                  ai::AiToolRisk::Mutating, false},
        [&workspace](const ai::JsonValue& arguments) {
            const auto id = editor_id(arguments.find("id"));
            EditorObject* object = id ? workspace.document().find_object(*id) : nullptr;
            if (!object) return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "unknown editor object", {}};
            if (object->flags.locked) return ai::AiToolCallResult{ai::AiCallStatus::Denied, {}, "editor object is locked", {}};
            RigidTransform after = object->transform;
            bool changed = false;
            if (const auto* position = arguments.find("position")) {
                const auto parsed = editor_vec3(position);
                if (!parsed) return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "position must be three finite numbers", {}};
                after.position = *parsed; changed = true;
            }
            if (const auto* rotation = arguments.find("rotation")) {
                const auto parsed = editor_quaternion(rotation);
                if (!parsed) return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "rotation must be a nonzero finite quaternion", {}};
                after.rotation = *parsed; changed = true;
            }
            if (!changed) return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "position or rotation is required", {}};
            const CommandResult result = workspace.commands().execute(
                workspace.document(), std::make_unique<TransformObjectCommand>(*id, object->transform, after));
            return command_result(result,
                ai::JsonValue::Object{{"id", static_cast<double>(*id)},
                                      {"position", editor_vec3_json(after.position)},
                                      {"rotation", editor_quaternion_json(after.rotation)}},
                "editor transform changed");
        });

    registry.add({"dve.editor.create_empty", "Create editor object",
                  "Create an empty editor object through the undo stack.",
                  editor_object_schema({{"name", string_property("Object name")},
                                        {"position", vector_property(3, "World position")}}),
                  ai::AiToolRisk::Mutating, false},
        [&workspace](const ai::JsonValue& arguments) {
            const EditorObjectId id = workspace.document().allocate_object_id();
            std::string name = arguments.find("name") ? std::string(arguments.find("name")->as_string()) : "AI Object";
            if (name.empty() || name.size() > 128U)
                return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "name must contain 1-128 characters", {}};
            EditorObject object{id, std::move(name)};
            if (const auto* position = arguments.find("position")) {
                const auto parsed = editor_vec3(position);
                if (!parsed) return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "invalid position", {}};
                object.transform.position = *parsed;
            }
            const CommandResult result = workspace.commands().execute(
                workspace.document(), std::make_unique<AddObjectCommand>(std::move(object), "AI create object"));
            if (result.success) workspace.select_object(id);
            return command_result(result, ai::JsonValue::Object{{"id", static_cast<double>(id)}},
                                  "editor object created");
        });

    registry.add({"dve.editor.rename_object", "Rename editor object",
                  "Rename one editor object through the undo stack.",
                  editor_object_schema({{"id", number_property("Editor object ID")},
                                        {"name", string_property("New name")}}, {"id", "name"}),
                  ai::AiToolRisk::Mutating, false},
        [&workspace](const ai::JsonValue& arguments) {
            const auto id = editor_id(arguments.find("id"));
            EditorObject* object = id ? workspace.document().find_object(*id) : nullptr;
            const std::string name = arguments.find("name") ? std::string(arguments.find("name")->as_string()) : std::string{};
            if (!object || name.empty() || name.size() > 128U)
                return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "unknown object or invalid name", {}};
            const CommandResult result = workspace.commands().execute(
                workspace.document(), std::make_unique<RenameObjectCommand>(*id, object->name, name));
            return command_result(result, ai::JsonValue::Object{{"id", static_cast<double>(*id)}, {"name", name}},
                                  "editor object renamed");
        });

    registry.add({"dve.editor.delete_object", "Delete editor object",
                  "Delete one editor object and its descendants through the undo stack.",
                  editor_object_schema({{"id", number_property("Editor object ID")}}, {"id"}),
                  ai::AiToolRisk::Destructive, false},
        [&workspace](const ai::JsonValue& arguments) {
            const auto id = editor_id(arguments.find("id"));
            if (!id || !workspace.document().find_object(*id))
                return ai::AiToolCallResult{ai::AiCallStatus::InvalidArguments, {}, "unknown editor object", {}};
            const CommandResult result = workspace.commands().execute(
                workspace.document(), std::make_unique<RemoveObjectsCommand>(std::vector<EditorObjectId>{*id}, "AI delete object"));
            if (result.success) workspace.prune_selection();
            return command_result(result, ai::JsonValue::Object{{"id", static_cast<double>(*id)}},
                                  "editor object deleted");
        });

    registry.add({"dve.editor.undo", "Undo editor action", "Undo the latest editor command.",
                  editor_object_schema({}), ai::AiToolRisk::Mutating, false},
        [&workspace](const ai::JsonValue&) {
            const CommandResult result = workspace.commands().undo(workspace.document());
            return command_result(result, ai::JsonValue::Object{}, "editor action undone");
        });
    registry.add({"dve.editor.redo", "Redo editor action", "Redo the latest undone editor command.",
                  editor_object_schema({}), ai::AiToolRisk::Mutating, false},
        [&workspace](const ai::JsonValue&) {
            const CommandResult result = workspace.commands().redo(workspace.document());
            return command_result(result, ai::JsonValue::Object{}, "editor action redone");
        });
}


} // namespace dve::editor
