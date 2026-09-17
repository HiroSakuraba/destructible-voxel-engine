#include "dve/polygon_asset.hpp"

#include <stdexcept>

namespace dve {

CookedPolygonAsset cook_polygon_model(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions,
    const PolygonCookOptions& polygonOptions,
    std::vector<ImportDiagnostic>* diagnostics) {
    ImportedModelResult imported = import_model(sourcePath, importOptions);
    if (diagnostics != nullptr) {
        diagnostics->insert(diagnostics->end(), imported.diagnostics.begin(), imported.diagnostics.end());
    }
    if (!imported.success) throw std::runtime_error("model import failed");
    return cook_polygon_scene(imported.scene, polygonOptions, diagnostics);
}

} // namespace dve
