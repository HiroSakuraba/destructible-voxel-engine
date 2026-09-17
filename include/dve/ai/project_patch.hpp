#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "dve/ai/json.hpp"

namespace dve::ai {

struct ProjectPatchLimits {
    std::size_t maximumPatchBytes{4U << 20U};
    std::size_t maximumFileBytes{4U << 20U};
    std::size_t maximumFiles{64};
};

struct ProjectPatchResult {
    bool ok{};
    JsonValue content{JsonValue::Object{}};
    std::string error;
};

[[nodiscard]] ProjectPatchResult preview_project_patch(
    const std::filesystem::path& projectRoot,
    std::string_view patch,
    const std::map<std::string, std::string, std::less<>>& baseHashes,
    ProjectPatchLimits limits = {});

[[nodiscard]] ProjectPatchResult apply_project_patch(
    const std::filesystem::path& projectRoot,
    std::string_view patch,
    const std::map<std::string, std::string, std::less<>>& baseHashes,
    ProjectPatchLimits limits = {});

[[nodiscard]] ProjectPatchResult rollback_project_patch(
    const std::filesystem::path& projectRoot,
    std::string_view transactionId);

} // namespace dve::ai
