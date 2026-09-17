#include "dve/editor_prefab.hpp"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::editor;

namespace {

bool parse_object_id(const char* text, EditorObjectId* output) {
    const std::string_view value(text ? text : "");
    const auto result = std::from_chars(value.data(), value.data() + value.size(), *output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && *output != 0U;
}

bool parse_instance_id(const char* text, std::uint64_t* output) {
    const std::string_view value(text ? text : "");
    const auto result = std::from_chars(value.data(), value.data() + value.size(), *output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && *output != 0U;
}

bool parse_float(const char* text, float* output) {
    try {
        std::size_t consumed{};
        *output = std::stof(text ? text : "", &consumed);
        return consumed == std::string(text ? text : "").size() && std::isfinite(*output);
    } catch (...) {
        return false;
    }
}

std::optional<ComponentValue> parse_component_value(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const std::string_view type = text.substr(0U, colon);
    const std::string_view payload = text.substr(colon + 1U);
    if (type == "string") return std::string(payload);
    if (type == "bool") {
        if (payload == "true" || payload == "1") return ComponentValue{true};
        if (payload == "false" || payload == "0") return ComponentValue{false};
        return std::nullopt;
    }
    if (type == "int") {
        std::int64_t value{};
        const auto parsed = std::from_chars(payload.data(), payload.data() + payload.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == payload.data() + payload.size())
            return ComponentValue{value};
        return std::nullopt;
    }
    if (type == "number") {
        try {
            std::size_t consumed{};
            const double value = std::stod(std::string(payload), &consumed);
            if (consumed == payload.size() && std::isfinite(value)) return ComponentValue{value};
        } catch (...) {}
        return std::nullopt;
    }
    const auto parse_tuple = [&](std::size_t count) -> std::optional<std::vector<float>> {
        std::vector<float> values;
        values.reserve(count);
        std::size_t begin{};
        while (begin <= payload.size()) {
            const std::size_t comma = payload.find(',', begin);
            const std::string token(payload.substr(begin, comma == std::string_view::npos
                                                            ? payload.size() - begin
                                                            : comma - begin));
            float value{};
            if (!parse_float(token.c_str(), &value)) return std::nullopt;
            values.push_back(value);
            if (comma == std::string_view::npos) break;
            begin = comma + 1U;
        }
        if (values.size() != count) return std::nullopt;
        return values;
    };
    if (type == "float3") {
        const auto values = parse_tuple(3U);
        if (values) return ComponentValue{Float3{(*values)[0], (*values)[1], (*values)[2]}};
    } else if (type == "quat") {
        const auto values = parse_tuple(4U);
        if (values) return ComponentValue{Quaternion{(*values)[0], (*values)[1], (*values)[2], (*values)[3]}};
    }
    return std::nullopt;
}

void usage() {
    std::cerr
        << "usage:\n"
        << "  dve_prefab_tool inspect <prefab.dveprefab>\n"
        << "  dve_prefab_tool capture <scene.dvescene> <prefab.dveprefab> <root-id> [root-id ...]\n"
        << "  dve_prefab_tool instantiate <scene.dvescene> <prefab.dveprefab> <output.dvescene> [x y z]\n"
        << "  dve_prefab_tool variant <parent.dveprefab> <variant.dveprefab> <name>"
           " [<template-id> <property-path> <typed-value> ...]\n"
        << "  dve_prefab_tool update <scene.dvescene> <prefab.dveprefab> <instance-id>"
           " <output.dvescene> [--allow-conflicts]\n"
        << "typed values: bool:true int:42 number:1.5 string:text float3:x,y,z quat:x,y,z,w\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    std::string error;
    if (command == "inspect") {
        if (argc != 3) { usage(); return 2; }
        auto prefab = load_editor_prefab(argv[2], &error);
        if (!prefab) {
            std::cerr << "prefab load failed: " << error << '\n';
            return 1;
        }
        std::cout << "name=" << prefab->name << '\n'
                  << "objects=" << prefab->templateDocument.objects().size() << '\n'
                  << "roots=" << prefab->rootTemplateObjectIds.size() << '\n'
                  << "parent=" << prefab->parentPrefabAsset.generic_string() << '\n'
                  << "inheritance_depth=" << prefab->inheritanceDepth << '\n'
                  << "content_hash=" << prefab->contentHash << '\n';
        for (const EditorObjectId root : prefab->rootTemplateObjectIds)
            std::cout << "root=" << root << '\n';
        return 0;
    }
    if (command == "capture") {
        if (argc < 5) { usage(); return 2; }
        auto scene = EditorDocument::load(argv[2], &error);
        if (!scene) {
            std::cerr << "scene load failed: " << error << '\n';
            return 1;
        }
        std::vector<EditorObjectId> roots;
        for (int i = 4; i < argc; ++i) {
            EditorObjectId id{};
            if (!parse_object_id(argv[i], &id)) {
                std::cerr << "invalid root object id: " << argv[i] << '\n';
                return 2;
            }
            roots.push_back(id);
        }
        const auto result = capture_editor_prefab(*scene, roots, argv[3]);
        if (!result.success) {
            std::cerr << "prefab capture failed: " << result.error << '\n';
            return 1;
        }
        std::cout << "prefab=" << result.prefabPath.generic_string() << '\n'
                  << "template=" << result.templateScenePath.generic_string() << '\n'
                  << "objects=" << result.objectCount << '\n'
                  << "content_hash=" << result.contentHash << '\n';
        return 0;
    }
    if (command == "variant") {
        if (argc < 5 || ((argc - 5) % 3) != 0) { usage(); return 2; }
        std::vector<EditorPrefabTemplateOverride> overrides;
        for (int i = 5; i < argc; i += 3) {
            EditorObjectId templateId{};
            if (!parse_object_id(argv[i], &templateId)) {
                std::cerr << "invalid template object id: " << argv[i] << '\n';
                return 2;
            }
            auto value = parse_component_value(argv[i + 2]);
            if (!value) {
                std::cerr << "invalid typed override value: " << argv[i + 2] << '\n';
                return 2;
            }
            overrides.push_back({templateId, argv[i + 1], std::move(*value)});
        }
        const auto result = create_editor_prefab_variant(
            argv[2], argv[3], argv[4], overrides);
        if (!result.success) {
            std::cerr << "prefab variant creation failed: " << result.error << '\n';
            return 1;
        }
        std::cout << "prefab=" << result.prefabPath.generic_string() << '\n'
                  << "overrides=" << result.overrideCount << '\n'
                  << "content_hash=" << result.contentHash << '\n';
        return 0;
    }
    if (command == "update") {
        if (argc != 6 && argc != 7) { usage(); return 2; }
        const bool allowConflicts = argc == 7 && std::string_view(argv[6]) == "--allow-conflicts";
        if (argc == 7 && !allowConflicts) { usage(); return 2; }
        auto scene = EditorDocument::load(argv[2], &error);
        if (!scene) {
            std::cerr << "scene load failed: " << error << '\n';
            return 1;
        }
        auto prefab = load_editor_prefab(argv[3], &error);
        if (!prefab) {
            std::cerr << "prefab load failed: " << error << '\n';
            return 1;
        }
        std::uint64_t instanceId{};
        if (!parse_instance_id(argv[4], &instanceId)) {
            std::cerr << "invalid prefab instance id: " << argv[4] << '\n';
            return 2;
        }
        const auto preview = preview_editor_prefab_instance_update(*scene, instanceId, *prefab);
        if (!preview.error.empty()) {
            std::cerr << "prefab update preview failed: " << preview.error << '\n';
            return 1;
        }
        const auto result = apply_editor_prefab_instance_update(
            *scene, instanceId, *prefab, allowConflicts);
        if (!result.success) {
            std::cerr << "prefab source update failed: " << result.error << '\n';
            for (const std::string& conflict : result.conflicts)
                std::cerr << "conflict: " << conflict << '\n';
            return 1;
        }
        const auto saved = scene->save_transactional(argv[5]);
        if (!saved.success) {
            std::cerr << "scene save failed: " << saved.error << '\n';
            return 1;
        }
        std::cout << "instance_id=" << instanceId << '\n'
                  << "stale=" << (preview.stale ? 1 : 0) << '\n'
                  << "updated=" << result.updatedObjects << '\n'
                  << "added=" << result.addedObjects << '\n'
                  << "removed=" << result.removedObjects << '\n'
                  << "scene=" << saved.manifestPath.generic_string() << '\n';
        return 0;
    }
    if (command == "instantiate") {
        if (argc != 5 && argc != 8) { usage(); return 2; }
        auto scene = EditorDocument::load(argv[2], &error);
        if (!scene) {
            std::cerr << "scene load failed: " << error << '\n';
            return 1;
        }
        auto prefab = load_editor_prefab(argv[3], &error);
        if (!prefab) {
            std::cerr << "prefab load failed: " << error << '\n';
            return 1;
        }
        RigidTransform transform{};
        if (argc == 8 && (!parse_float(argv[5], &transform.position.x) ||
                          !parse_float(argv[6], &transform.position.y) ||
                          !parse_float(argv[7], &transform.position.z))) {
            std::cerr << "invalid instance position\n";
            return 2;
        }
        const auto result = instantiate_editor_prefab(*scene, *prefab, transform);
        if (!result.success) {
            std::cerr << "prefab instantiation failed: " << result.error << '\n';
            return 1;
        }
        const auto saved = scene->save_transactional(argv[4]);
        if (!saved.success) {
            std::cerr << "scene save failed: " << saved.error << '\n';
            return 1;
        }
        std::cout << "instance_id=" << result.instanceId << '\n'
                  << "objects=" << result.objectIds.size() << '\n'
                  << "roots=" << result.rootObjectIds.size() << '\n'
                  << "scene=" << saved.manifestPath.generic_string() << '\n';
        return 0;
    }
    usage();
    return 2;
}
