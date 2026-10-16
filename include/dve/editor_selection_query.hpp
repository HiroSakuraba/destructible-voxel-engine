#pragma once

// Select by property (artist worklist ART-019): a small query language for the hierarchy filter
// and Edit > Select Matching, plus the property facts Select Similar compares.
//
//   wall                 name contains "wall" (every bare word must match)
//   type:mesh            voxels | mesh | subdivision | text | gabor | helper | group
//   tag:rock,cliff       any of the listed values (commas mean "or")
//   layer:2  material:3  material:stone  prefab:tree  component:dve.spawn  parent:rig
//   locked:yes  visible:no  anchored:yes
//   -tag:rock            a leading minus negates a term
//
// Keys and values ignore case. Unknown keys are reported as an error instead of matching nothing.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dve/editor_document.hpp"

namespace dve::editor {

struct SelectionQueryTerm {
    std::string key;                  // empty for a bare name word
    std::vector<std::string> values;  // lower case; any may match
    bool negated{};
};

struct SelectionQuery {
    std::vector<SelectionQueryTerm> terms;
    [[nodiscard]] bool empty() const noexcept { return terms.empty(); }
};

// True when the text uses query syntax (a key:value term or a negation), so a plain hierarchy
// filter keeps its old substring meaning.
[[nodiscard]] bool looks_like_selection_query(std::string_view text) noexcept;
[[nodiscard]] std::optional<SelectionQuery> parse_selection_query(std::string_view text, std::string* error = nullptr);
[[nodiscard]] bool selection_query_matches(const EditorDocument& document, const EditorObject& object, const SelectionQuery& query);

// The kind name used by type: and Select Similar > Same Type.
[[nodiscard]] std::string selection_object_kind(const EditorDocument& document, const EditorObject& object);
// Material identities an object uses: "#<id>" for voxel palette ids and lower-case names for
// named voxel overrides and polygon materials.
[[nodiscard]] std::set<std::string> selection_object_materials(const EditorObject& object);

}  // namespace dve::editor
