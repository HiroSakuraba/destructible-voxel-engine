#include "dve/editor_selection_query.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>

#include "dve/brick.hpp"

namespace dve::editor {
namespace {
std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(separator, start);
        const std::string_view part = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!part.empty()) parts.emplace_back(lower(part));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

constexpr std::array<std::string_view, 11> kKeys{"name", "type", "tag", "layer", "material", "prefab", "component",
                                                 "parent", "locked", "visible", "anchored"};

std::optional<bool> yes_no(std::string_view value) {
    if (value == "yes" || value == "true" || value == "on" || value == "1") return true;
    if (value == "no" || value == "false" || value == "off" || value == "0") return false;
    return std::nullopt;
}

bool contains_text(std::string_view haystack, std::string_view needle) { return lower(haystack).find(needle) != std::string::npos; }
}  // namespace

bool looks_like_selection_query(std::string_view text) noexcept {
    std::size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && text[start] == ' ') ++start;
        std::size_t end = text.find(' ', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view word = text.substr(start, end - start);
        if (!word.empty() && (word.front() == '-' || word.find(':') != std::string_view::npos)) return true;
        start = end + 1;
    }
    return false;
}

std::optional<SelectionQuery> parse_selection_query(std::string_view text, std::string* error) {
    SelectionQuery query;
    for (const std::string& word : split(text, ' ')) {
        SelectionQueryTerm term;
        std::string_view body = word;
        if (body.size() > 1 && body.front() == '-') { term.negated = true; body.remove_prefix(1); }
        const std::size_t colon = body.find(':');
        if (colon == std::string_view::npos) {
            term.values = {std::string(body)};
        } else {
            term.key = std::string(body.substr(0, colon));
            term.values = split(body.substr(colon + 1), ',');
            if (std::find(kKeys.begin(), kKeys.end(), term.key) == kKeys.end()) {
                if (error) *error = "Unknown filter key \"" + term.key + "\" (try type, tag, layer, material, prefab, component, parent, locked, visible, anchored)";
                return std::nullopt;
            }
            if (term.values.empty()) {
                if (error) *error = "\"" + term.key + ":\" needs a value";
                return std::nullopt;
            }
            if (term.key == "name") term.key.clear();
            if (term.key == "locked" || term.key == "visible" || term.key == "anchored")
                for (const std::string& value : term.values)
                    if (!yes_no(value)) {
                        if (error) *error = "\"" + term.key + ":\" takes yes or no";
                        return std::nullopt;
                    }
        }
        query.terms.push_back(std::move(term));
    }
    return query;
}

std::string selection_object_kind(const EditorDocument& document, const EditorObject& object) {
    if (object.text3d) return "text";
    if (object.gaborVolume) return "gabor";
    if (object.subdivision) return "subdivision";
    if (object.polygon) return "mesh";
    if (object.voxels && object.voxels->occupied_voxel_count() > 0) return "voxels";
    return document.children_of(object.id).empty() ? "helper" : "group";
}

std::set<std::string> selection_object_materials(const EditorObject& object) {
    std::set<std::string> materials;
    if (object.voxels) {
        std::set<MaterialId> ids;
        for (const auto& [key, brick] : object.voxels->bricks()) {
            (void)key;
            brick.occupancy().for_each_set([&](std::uint16_t index) { ids.insert(brick.material(index)); });
        }
        for (const MaterialId id : ids) {
            materials.insert("#" + std::to_string(id));
            if (const auto it = object.voxelMaterials.find(id); it != object.voxelMaterials.end() && !it->second.name.empty())
                materials.insert(lower(it->second.name));
        }
    }
    if (object.polygon)
        for (const auto& material : object.polygon->materials)
            if (!material.name.empty()) materials.insert(lower(material.name));
    return materials;
}

bool selection_query_matches(const EditorDocument& document, const EditorObject& object, const SelectionQuery& query) {
    std::optional<std::set<std::string>> materials;  // computed on demand: it scans the voxels
    for (const SelectionQueryTerm& term : query.terms) {
        bool any = false;
        for (const std::string& value : term.values) {
            bool hit = false;
            if (term.key.empty()) hit = contains_text(object.name, value);
            else if (term.key == "type") hit = selection_object_kind(document, object) == value ||
                                               (value == "polygon" && object.polygon && !object.subdivision);
            else if (term.key == "tag") hit = std::any_of(object.tags.begin(), object.tags.end(), [&](const std::string& tag) { return lower(tag) == value; });
            else if (term.key == "layer") {
                std::uint32_t layer{};
                const auto [end, code] = std::from_chars(value.data(), value.data() + value.size(), layer);
                hit = code == std::errc{} && end == value.data() + value.size() && layer == object.layer;
            } else if (term.key == "material") {
                if (!materials) materials = selection_object_materials(object);
                hit = materials->contains(value) || materials->contains("#" + value);
            } else if (term.key == "prefab") hit = object.prefabLink && contains_text(object.prefabLink->prefabAsset.generic_string(), value);
            else if (term.key == "component")
                hit = std::any_of(object.components.begin(), object.components.end(), [&](const Component& component) {
                    return contains_text(component.type, value);
                });
            else if (term.key == "parent") {
                for (auto parent = object.parent; parent && !hit;) {
                    const EditorObject* ancestor = document.find_object(*parent);
                    if (!ancestor) break;
                    hit = contains_text(ancestor->name, value);
                    parent = ancestor->parent;
                }
            } else {
                const bool wanted = *yes_no(value);
                const bool actual = term.key == "locked" ? object.flags.locked : term.key == "visible" ? object.flags.visible : object.flags.anchored;
                hit = wanted == actual;
            }
            if (hit) { any = true; break; }
        }
        if (any == term.negated) return false;
    }
    return true;
}

}  // namespace dve::editor
