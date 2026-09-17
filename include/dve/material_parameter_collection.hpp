#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dve/asset_cooker.hpp"

namespace dve {

inline constexpr std::size_t kMaximumMaterialGlobalScalars = 32;
inline constexpr std::size_t kMaximumMaterialGlobalVectors = 16;
inline constexpr std::uint8_t kInvalidMaterialGlobalSlot = 0xFFU;

// A compact, stable-slot analogue of Unreal's Material Parameter Collections. Names exist on
// the control/editor side only; the renderer receives fixed scalar/vector slots, so updates do
// not require one copy per material instance. Slots never move during the collection's
// lifetime: changing a value is safe, while adding a new name appends a new slot.
class MaterialParameterCollection {
public:
    struct ScalarEntry {
        std::string name;
        float value{};
    };
    struct VectorEntry {
        std::string name;
        Float4 value{};
    };

    MaterialParameterCollection();

    [[nodiscard]] bool set_scalar(std::string_view name, float value, std::string* error = nullptr);
    [[nodiscard]] bool set_vector(std::string_view name, Float4 value, std::string* error = nullptr);
    [[nodiscard]] std::optional<float> scalar(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<Float4> vector(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> scalar_slot(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> vector_slot(std::string_view name) const noexcept;
    [[nodiscard]] float scalar_at(std::size_t slot) const noexcept;
    [[nodiscard]] Float4 vector_at(std::size_t slot) const noexcept;
    [[nodiscard]] std::size_t scalar_count() const noexcept { return scalars_.size(); }
    [[nodiscard]] std::size_t vector_count() const noexcept { return vectors_.size(); }
    [[nodiscard]] const std::vector<ScalarEntry>& scalars() const noexcept { return scalars_; }
    [[nodiscard]] const std::vector<VectorEntry>& vectors() const noexcept { return vectors_; }
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] static std::optional<MaterialParameterCollection> load(
        const std::filesystem::path& path, std::string* error = nullptr);

private:
    std::vector<ScalarEntry> scalars_;
    std::vector<VectorEntry> vectors_;
    std::unordered_map<std::string, std::uint8_t> scalarSlots_;
    std::unordered_map<std::string, std::uint8_t> vectorSlots_;
};

// Canonical slots are inserted by the constructor in this exact order. Shaders may consume
// these semantics directly; additional user names remain available to explicit master-material
// bindings on the CPU side.
inline constexpr std::uint8_t kMpcTimeSecondsSlot = 0;
inline constexpr std::uint8_t kMpcWindStrengthSlot = 1;
inline constexpr std::uint8_t kMpcWetnessSlot = 2;
inline constexpr std::uint8_t kMpcSnowAmountSlot = 3;
inline constexpr std::uint8_t kMpcTimeOfDayTintSlot = 0;
inline constexpr std::uint8_t kMpcWindDirectionSlot = 1;

} // namespace dve
