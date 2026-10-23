#pragma once
#include <cstdint>
#include <string>
namespace dve {
// Authoring provenance only. Resolved shading values remain authoritative at runtime.
struct MaterialPresetOrigin {
    std::string id;
    std::uint32_t revision{};
    std::uint64_t overrides{};
    bool operator==(const MaterialPresetOrigin&) const = default;
};
}
