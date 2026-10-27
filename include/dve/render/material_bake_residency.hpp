#pragma once
// T2: bridge completed material texture bakes into opt-in texture residency.
// Asset ID assignment stays with the host; partial admission remains observable.
#include "dve/render/material_texture_bake.hpp"
#include "dve/render/texture_residency.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dve::render {
struct MaterialBakeTextureBinding {
    MaterialBakeMap map{MaterialBakeMap::BaseColor};
    std::uint64_t assetId{};
    TextureCompressionPreference compression{TextureCompressionPreference::PreferBc7};
};
struct MaterialBakeResidencyReport {
    std::size_t accepted{};
    std::size_t rejected{};
    std::vector<MaterialBakeMap> failedMaps{};
};
// False reports validation or partial queue-admission failure. Accepted textures
// remain queued; use ReplaceIfChanged for idempotent retry, never assume rollback.
[[nodiscard]] bool enqueue_material_bake_textures(
    TextureResidencyManager& manager,
    const MaterialTextureBakeResult& bake,
    std::span<const MaterialBakeTextureBinding> bindings,
    MaterialBakeResidencyReport& report,
    TextureUploadMode mode = TextureUploadMode::ReplaceIfChanged,
    std::string* error = nullptr);
} // namespace dve::render
