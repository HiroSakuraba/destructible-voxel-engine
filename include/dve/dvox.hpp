#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>

#include "dve/asset_cooker.hpp"

namespace dve {

struct DvoxWriteOptions {
    bool includeDiagnostics{};
};

struct DvoxReadResult {
    CookedVoxelAsset asset;
    bool success{};
    std::string error;

    explicit DvoxReadResult(std::uint64_t objectId = 1) : asset(objectId) {}
};

[[nodiscard]] bool write_dvox(
    const std::filesystem::path& path,
    const CookedVoxelAsset& asset,
    const DvoxWriteOptions& options = {},
    std::string* error = nullptr);

[[nodiscard]] DvoxReadResult read_dvox(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max());

// In-memory variant for content that does not live on the filesystem (for example an entry
// read from a mounted .dvepak through ContentSource). Validation is identical to the path
// overload, which reads the file and then delegates here; `bytes` must be the complete file.
[[nodiscard]] DvoxReadResult read_dvox(
    std::span<const std::byte> bytes,
    std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max());

} // namespace dve
