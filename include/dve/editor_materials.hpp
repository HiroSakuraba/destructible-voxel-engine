#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/asset_cooker.hpp"

namespace dve { class MaterialLibrary; }

namespace dve::editor {

// Immutable engine material used by newly-created project templates and as the editor's
// default paint material. It deliberately does not reuse a physical preset such as concrete.
inline constexpr MaterialId kDefaultSurfaceMaterial = 9;

struct EditorMaterialEntry {
    MaterialId id{kAirMaterial};
    VoxelMaterialDefinition definition{};
    bool builtIn{};
};

class EditorMaterialLibrary {
public:
    EditorMaterialLibrary();

    [[nodiscard]] const EditorMaterialEntry* find(MaterialId id) const noexcept;
    [[nodiscard]] EditorMaterialEntry* find(MaterialId id) noexcept;
    [[nodiscard]] std::vector<EditorMaterialEntry> entries() const;
    [[nodiscard]] bool upsert(EditorMaterialEntry entry, std::string* error = nullptr);
    [[nodiscard]] bool remove(MaterialId id, std::string* error = nullptr);
    [[nodiscard]] MaterialId next_available_id() const noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;

    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] static std::optional<EditorMaterialLibrary> load(
        const std::filesystem::path& path,
        std::string* error = nullptr);

    [[nodiscard]] static EditorMaterialLibrary make_default();

private:
    explicit EditorMaterialLibrary(bool populateDefaults);
    std::map<MaterialId, EditorMaterialEntry> entries_;
};

[[nodiscard]] Float4 editor_material_display_color(
    const EditorMaterialLibrary& library,
    MaterialId id) noexcept;

[[nodiscard]] std::string_view editor_material_shading_label(MaterialShadingModel model) noexcept;
[[nodiscard]] std::string editor_material_badge(const VoxelMaterialDefinition& definition);
[[nodiscard]] std::vector<std::string> editor_material_warnings(
    const VoxelMaterialDefinition& definition);

// Copies every successfully resolved voxel-facing material from a master/instance library into
// the editor library. Built-in slots are preserved by default; pass replaceBuiltIns=true when
// the authored master library is intended to be authoritative for those ids as well.
[[nodiscard]] bool sync_resolved_materials(
    const dve::MaterialLibrary& source,
    EditorMaterialLibrary& destination,
    bool replaceBuiltIns = false,
    std::string* error = nullptr);

} // namespace dve::editor
