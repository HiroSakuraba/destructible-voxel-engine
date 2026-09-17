#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dve/geometry_build.hpp"
#include "dve/transform.hpp"

namespace dve {

struct HybridSceneAssetEntry {
    std::uint64_t objectId{};
    std::string name;
    GeometryKind kind{GeometryKind::Voxel};
    std::filesystem::path assetPath;
    std::uint64_t contentHash{};
    RigidTransform transform{};
    bool dynamic{};
    bool structural{true};
    std::vector<std::filesystem::path> dependencies;
};

struct HybridSceneManifest {
    std::string name;
    GeometryBuildMode requiredMode{GeometryBuildMode::Hybrid};
    std::vector<HybridSceneAssetEntry> assets;
    std::uint64_t contentHash{};
};

struct HybridSceneManifestReadResult {
    HybridSceneManifest manifest;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] std::uint64_t hybrid_scene_manifest_hash(const HybridSceneManifest& manifest) noexcept;
[[nodiscard]] bool validate_hybrid_scene_manifest(const HybridSceneManifest& manifest,
                                                  std::string* error = nullptr) noexcept;
[[nodiscard]] bool manifest_compatible_with_build(const HybridSceneManifest& manifest,
                                                  GeometryBuildMode mode,
                                                  std::string* error = nullptr) noexcept;
[[nodiscard]] bool write_dvescene(const std::filesystem::path& path,
                                  const HybridSceneManifest& manifest,
                                  std::string* error = nullptr);
[[nodiscard]] HybridSceneManifestReadResult read_dvescene(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 64ULL * 1024ULL * 1024ULL);

} // namespace dve
