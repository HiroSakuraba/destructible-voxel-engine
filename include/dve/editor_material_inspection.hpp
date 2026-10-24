#pragma once
#include "dve/editor_material_preview.hpp"
#include "dve/render/material_texture_bake.hpp"
#include <functional>
namespace dve::editor {
enum class MaterialInspectionChannel : unsigned { Beauty,BaseColor,Normal,Roughness,Metallic,Emission,Opacity,UvCoverage,AmbientOcclusion,DiffuseLighting };
enum class MaterialInspectionView : unsigned { Source,Baked,Split };
[[nodiscard]] const char* material_inspection_channel_label(MaterialInspectionChannel) noexcept;
[[nodiscard]] MaterialPreviewImage render_material_inspection(const CookedPolygonAsset& source,const CookedPolygonAsset& baked,
    const render::MaterialTextureBakeSettings&,MaterialInspectionChannel,MaterialInspectionView,
    const std::function<bool()>& cancelled={});
}
