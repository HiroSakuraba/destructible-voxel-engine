#pragma once
#include "dve/editor_material_presets.hpp"
#include "dve/polygon_asset.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace dve::render {struct MaterialTextureBakeSettings;}
namespace dve::editor {
enum class MaterialInspectionChannel : unsigned;
enum class MaterialInspectionView : unsigned;
enum class MaterialPreviewShape : unsigned { Sphere, Patch, RoundedBox, Wedge };
struct MaterialPreviewImage { unsigned size{64}; std::vector<std::uint32_t> rgb; std::string error; };
[[nodiscard]] CookedPolygonAsset material_preview_fixture(const VoxelMaterialDefinition&,MaterialPreviewShape);
// One bounded worker; latest request replaces pending work. Cache keys include the complete
// resolved material and fixed CPU renderer/lighting/fixture revisions. Stale results cannot
// replace a new request. Closing cancels by row and never waits for a full thumbnail queue.
class MaterialPreviewWorker {
public:
    MaterialPreviewWorker();
    ~MaterialPreviewWorker();
    MaterialPreviewWorker(const MaterialPreviewWorker&)=delete;
    MaterialPreviewWorker& operator=(const MaterialPreviewWorker&)=delete;
    void request(const VoxelMaterialDefinition&,MaterialPreviewShape);
    void request(const CookedPolygonAsset&,std::uint32_t materialIndex,MaterialPreviewShape);
    void request_inspection(const CookedPolygonAsset&,const CookedPolygonAsset&,const render::MaterialTextureBakeSettings&,MaterialInspectionChannel,MaterialInspectionView);
    void cancel();
    [[nodiscard]] std::shared_ptr<const MaterialPreviewImage> image() const;
    [[nodiscard]] bool busy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
