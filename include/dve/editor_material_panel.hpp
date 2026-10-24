#pragma once
#include "dve/editor_material_preview.hpp"
#include "dve/render/material_texture_bake.hpp"
#include "dve/editor_viewport.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace dve::editor {
struct MaterialPanelLayout {
    UiRect panel,close,search,family,favorites,recent,backend,supported,previous,next,preview,shape,savePreset,apply,editSlot,loadSlot,reset,update,advanced,layered;
    std::vector<UiRect> rows,fields,resets;
};
struct MaterialPanelState {
    bool open{},advanced{},favoritesOnly{},recentOnly{},supportedOnly{},searchFocus{true};
    bool preferencesLoaded{};
    bool layerMode{};
    render::MaterialTextureBakeSettings bake;
    std::filesystem::path lastBakeDirectory;
    std::size_t layerIndex{};
    std::optional<std::string> layerEditing;
    MaterialPresetBackend backend{MaterialPresetBackend::CpuReference};
    MaterialPreviewShape shape{MaterialPreviewShape::Sphere};
    MaterialPresetCatalog catalog{MaterialPresetCatalog::starter()};
    // Directory whose user presets are merged into catalog; a different project rebuilds it.
    std::optional<std::filesystem::path> catalogDirectory;
    std::string query,family,selected{"dve.glass.clear"},message;
    std::set<std::string> favorites;
    std::vector<std::string> recent;
    std::size_t first{},controlFirst{};
    unsigned slot{};
    std::optional<MaterialPresetProperty> editing;
    std::string typed;
    bool replaceText{};
    VoxelMaterialDefinition draft;
    std::unique_ptr<MaterialPreviewWorker> preview;
    std::unique_ptr<MaterialPreviewWorker> thumbnails;
    std::map<std::string,std::shared_ptr<const MaterialPreviewImage>> thumbnailCache;
    std::string thumbnailPending;
};
}
