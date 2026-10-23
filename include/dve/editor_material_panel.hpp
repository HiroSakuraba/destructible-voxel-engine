#pragma once
#include "dve/editor_material_preview.hpp"
#include "dve/editor_viewport.hpp"
namespace dve::editor {
struct MaterialPanelLayout {
    UiRect panel,close,search,family,favorites,recent,backend,supported,previous,next,preview,shape,savePreset,apply,editSlot,loadSlot,reset,update,advanced;
    std::vector<UiRect> rows,fields,resets;
};
struct MaterialPanelState {
    bool open{},advanced{},favoritesOnly{},recentOnly{},supportedOnly{},searchFocus{true};
    bool preferencesLoaded{};
    MaterialPresetBackend backend{MaterialPresetBackend::CpuReference};
    MaterialPreviewShape shape{MaterialPreviewShape::Sphere};
    MaterialPresetCatalog catalog{MaterialPresetCatalog::starter()};
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
