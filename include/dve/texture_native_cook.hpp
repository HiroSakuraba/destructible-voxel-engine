#pragma once
#include "dve/compressed_texture.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace dve {
// T3: deterministic DDS DX10 export of already encoded BC4/BC5/BC6H/BC7 mip
// chains for cooked desktop assets. No supercompression/transcode is implied.
[[nodiscard]] bool encode_native_bc_dds(
    const CompressedTextureAsset& texture, std::vector<std::byte>& dds,
    std::string* error = nullptr);
} // namespace dve
