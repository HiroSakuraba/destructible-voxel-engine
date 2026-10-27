#include "dve/render/material_bake_residency.hpp"
#include <algorithm>
#include <optional>
#include <set>
#include <utility>

namespace dve::render {
namespace {
std::optional<TextureSemantic> semantic(MaterialBakeMap map) noexcept {
    switch (map) {
    case MaterialBakeMap::BaseColor: return TextureSemantic::BaseColor;
    case MaterialBakeMap::Normal: return TextureSemantic::Normal;
    case MaterialBakeMap::MetallicRoughness: return TextureSemantic::MetallicRoughness;
    case MaterialBakeMap::Emission: return TextureSemantic::Emissive;
    case MaterialBakeMap::Opacity: return TextureSemantic::Opacity;
    case MaterialBakeMap::AmbientOcclusion: return TextureSemantic::AmbientOcclusion;
    case MaterialBakeMap::DiffuseLighting: return TextureSemantic::BaseColor;
    }
    return std::nullopt;
}
}
bool enqueue_material_bake_textures(
    TextureResidencyManager& manager, const MaterialTextureBakeResult& bake,
    std::span<const MaterialBakeTextureBinding> bindings, MaterialBakeResidencyReport& report,
    TextureUploadMode mode, std::string* error) {
    report = {};
    if (!bake.success) {
        if (error) *error = "cannot enqueue a failed or incomplete material bake";
        return false;
    }
    std::set<std::uint64_t> ids;
    std::set<MaterialBakeMap> maps;
    // Validate the entire binding set before performing any mutation. Queue
    // capacity failures during subsequent enqueues are surfaced separately.
    for (const auto& item:bindings) {
        if (item.assetId==0U || !ids.insert(item.assetId).second ||
            !maps.insert(item.map).second || !semantic(item.map) ||
            !bake.images.contains(item.map)) {
            if(error) *error="invalid, duplicate or missing baked texture binding";
            return false;
        }
    }
    bool all=true;
    for (const auto& item:bindings) {
        TextureUploadRequest request;
        request.assetId=item.assetId;
        request.image=bake.images.at(item.map);
        request.semantic=*semantic(item.map);
        request.compression=item.compression;
        request.generateMips=true;
        request.mode=mode;
        std::string local;
        if (manager.enqueue(std::move(request),&local)) {
            ++report.accepted;
        } else {
            ++report.rejected;
            report.failedMaps.push_back(item.map);
            if(error && error->empty()) *error=local;
            all=false;
        }
    }
    return all;
}
} // namespace dve::render
