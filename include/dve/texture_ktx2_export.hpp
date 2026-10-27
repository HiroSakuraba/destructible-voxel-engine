#pragma once
#include "dve/compressed_texture.hpp"
#include <cstddef>
#include <string>
#include <vector>
namespace dve {
// Optional T3 exporter implemented only in dve_native_ktx2_export, built
// when a compatible Khronos libktx installation is supplied explicitly.
// DVE uses libktx so the KHR_DF descriptor is standards-compliant.
[[nodiscard]] bool encode_native_ktx2(
    const CompressedTextureAsset& asset, std::vector<std::byte>& output,
    std::string* error = nullptr);
}
