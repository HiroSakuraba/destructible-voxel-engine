#include "dve/material_parameter_collection.hpp"

#include <algorithm>
#include <bit>
#include <iterator>
#include <cmath>
#include <fstream>
#include <unordered_set>

namespace dve {
namespace {

bool finite(Float4 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}


void write_u16(std::ostream& out, std::uint16_t value) {
    const char bytes[2]{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
    out.write(bytes, 2);
}

void write_u32(std::ostream& out, std::uint32_t value) {
    const char bytes[4]{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
                        static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
    out.write(bytes, 4);
}

void write_float(std::ostream& out, float value) {
    write_u32(out, std::bit_cast<std::uint32_t>(value));
}

bool read_u16(std::istream& in, std::uint16_t& value) {
    unsigned char bytes[2]{};
    if (!in.read(reinterpret_cast<char*>(bytes), 2)) return false;
    value = static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
    return true;
}

bool read_u32(std::istream& in, std::uint32_t& value) {
    unsigned char bytes[4]{};
    if (!in.read(reinterpret_cast<char*>(bytes), 4)) return false;
    value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return true;
}

bool read_float(std::istream& in, float& value) {
    std::uint32_t bits{};
    if (!read_u32(in, bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

bool write_name(std::ostream& out, std::string_view name) {
    if (name.size() > 0xFFFFU) return false;
    write_u16(out, static_cast<std::uint16_t>(name.size()));
    out.write(name.data(), static_cast<std::streamsize>(name.size()));
    return static_cast<bool>(out);
}

bool read_name(std::istream& in, std::string& name) {
    std::uint16_t length{};
    if (!read_u16(in, length) || length > 96U) return false;
    name.resize(length);
    return length == 0U || static_cast<bool>(in.read(name.data(), static_cast<std::streamsize>(length)));
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 96) return false;
    for (const char ch : name) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '.';
        if (!ok) return false;
    }
    return true;
}

} // namespace

MaterialParameterCollection::MaterialParameterCollection() {
    std::string ignored;
    (void)set_scalar("TimeSeconds", 0.0F, &ignored);
    (void)set_scalar("WindStrength", 0.0F, &ignored);
    (void)set_scalar("Wetness", 0.0F, &ignored);
    (void)set_scalar("SnowAmount", 0.0F, &ignored);
    (void)set_vector("TimeOfDayTint", {1.0F, 1.0F, 1.0F, 1.0F}, &ignored);
    (void)set_vector("WindDirection", {1.0F, 0.0F, 0.0F, 0.0F}, &ignored);
}

bool MaterialParameterCollection::set_scalar(std::string_view name, float value, std::string* error) {
    if (!valid_name(name)) {
        if (error) *error = "material global scalar name is empty, too long, or contains unsupported characters";
        return false;
    }
    if (!std::isfinite(value)) {
        if (error) *error = "material global scalar must be finite";
        return false;
    }
    const auto existing = scalarSlots_.find(std::string(name));
    if (existing != scalarSlots_.end()) {
        scalars_[existing->second].value = value;
        return true;
    }
    if (vectorSlots_.contains(std::string(name))) {
        if (error) *error = "material global name is already used by a vector";
        return false;
    }
    if (scalars_.size() >= kMaximumMaterialGlobalScalars) {
        if (error) *error = "material global scalar capacity exceeded";
        return false;
    }
    const auto slot = static_cast<std::uint8_t>(scalars_.size());
    scalarSlots_.emplace(std::string(name), slot);
    scalars_.push_back({std::string(name), value});
    return true;
}

bool MaterialParameterCollection::set_vector(std::string_view name, Float4 value, std::string* error) {
    if (!valid_name(name)) {
        if (error) *error = "material global vector name is empty, too long, or contains unsupported characters";
        return false;
    }
    if (!finite(value)) {
        if (error) *error = "material global vector must be finite";
        return false;
    }
    const auto existing = vectorSlots_.find(std::string(name));
    if (existing != vectorSlots_.end()) {
        vectors_[existing->second].value = value;
        return true;
    }
    if (scalarSlots_.contains(std::string(name))) {
        if (error) *error = "material global name is already used by a scalar";
        return false;
    }
    if (vectors_.size() >= kMaximumMaterialGlobalVectors) {
        if (error) *error = "material global vector capacity exceeded";
        return false;
    }
    const auto slot = static_cast<std::uint8_t>(vectors_.size());
    vectorSlots_.emplace(std::string(name), slot);
    vectors_.push_back({std::string(name), value});
    return true;
}

std::optional<float> MaterialParameterCollection::scalar(std::string_view name) const noexcept {
    const auto it = scalarSlots_.find(std::string(name));
    if (it == scalarSlots_.end()) return std::nullopt;
    return scalars_[it->second].value;
}

std::optional<Float4> MaterialParameterCollection::vector(std::string_view name) const noexcept {
    const auto it = vectorSlots_.find(std::string(name));
    if (it == vectorSlots_.end()) return std::nullopt;
    return vectors_[it->second].value;
}

std::optional<std::uint8_t> MaterialParameterCollection::scalar_slot(std::string_view name) const noexcept {
    const auto it = scalarSlots_.find(std::string(name));
    return it == scalarSlots_.end() ? std::nullopt : std::optional<std::uint8_t>(it->second);
}

std::optional<std::uint8_t> MaterialParameterCollection::vector_slot(std::string_view name) const noexcept {
    const auto it = vectorSlots_.find(std::string(name));
    return it == vectorSlots_.end() ? std::nullopt : std::optional<std::uint8_t>(it->second);
}

float MaterialParameterCollection::scalar_at(std::size_t slot) const noexcept {
    return slot < scalars_.size() ? scalars_[slot].value : 0.0F;
}

Float4 MaterialParameterCollection::vector_at(std::size_t slot) const noexcept {
    return slot < vectors_.size() ? vectors_[slot].value : Float4{};
}

bool MaterialParameterCollection::validate(std::string* error) const {
    if (scalars_.size() > kMaximumMaterialGlobalScalars || vectors_.size() > kMaximumMaterialGlobalVectors) {
        if (error) *error = "material parameter collection exceeds fixed GPU capacity";
        return false;
    }
    std::unordered_set<std::string> names;
    for (std::size_t slot = 0; slot < scalars_.size(); ++slot) {
        const auto& entry = scalars_[slot];
        if (!valid_name(entry.name) || !std::isfinite(entry.value) || !names.insert(entry.name).second ||
            scalarSlots_.find(entry.name) == scalarSlots_.end() || scalarSlots_.at(entry.name) != slot) {
            if (error) *error = "invalid or inconsistent material global scalar table";
            return false;
        }
    }
    for (std::size_t slot = 0; slot < vectors_.size(); ++slot) {
        const auto& entry = vectors_[slot];
        if (!valid_name(entry.name) || !finite(entry.value) || !names.insert(entry.name).second ||
            vectorSlots_.find(entry.name) == vectorSlots_.end() || vectorSlots_.at(entry.name) != slot) {
            if (error) *error = "invalid or inconsistent material global vector table";
            return false;
        }
    }
    return true;
}



bool MaterialParameterCollection::save(const std::filesystem::path& path, std::string* error) const {
    std::string validationError;
    if (!validate(&validationError)) {
        if (error) *error = validationError;
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot open material parameter collection for writing";
        return false;
    }
    constexpr char magic[8]{'D','V','E','M','P','C','1','\0'};
    out.write(magic, 8);
    write_u16(out, 1U);
    write_u16(out, static_cast<std::uint16_t>(scalars_.size()));
    write_u16(out, static_cast<std::uint16_t>(vectors_.size()));
    write_u16(out, 0U);
    for (const ScalarEntry& entry : scalars_) {
        if (!write_name(out, entry.name)) break;
        write_float(out, entry.value);
    }
    for (const VectorEntry& entry : vectors_) {
        if (!write_name(out, entry.name)) break;
        write_float(out, entry.value.x);
        write_float(out, entry.value.y);
        write_float(out, entry.value.z);
        write_float(out, entry.value.w);
    }
    if (!out) {
        if (error) *error = "failed while writing material parameter collection";
        return false;
    }
    return true;
}

std::optional<MaterialParameterCollection> MaterialParameterCollection::load(
    const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open material parameter collection";
        return std::nullopt;
    }
    char magic[8]{};
    std::uint16_t version{}, scalarCount{}, vectorCount{}, reserved{};
    constexpr char expected[8]{'D','V','E','M','P','C','1','\0'};
    if (!in.read(magic, 8) || !std::equal(std::begin(magic), std::end(magic), std::begin(expected)) ||
        !read_u16(in, version) || !read_u16(in, scalarCount) || !read_u16(in, vectorCount) ||
        !read_u16(in, reserved) || version != 1U || reserved != 0U ||
        scalarCount > kMaximumMaterialGlobalScalars || vectorCount > kMaximumMaterialGlobalVectors) {
        if (error) *error = "invalid or unsupported material parameter collection header";
        return std::nullopt;
    }

    MaterialParameterCollection collection;
    const std::size_t canonicalScalarCount = collection.scalar_count();
    const std::size_t canonicalVectorCount = collection.vector_count();
    if (scalarCount < canonicalScalarCount || vectorCount < canonicalVectorCount) {
        if (error) *error = "material parameter collection omits canonical slots";
        return std::nullopt;
    }
    for (std::size_t slot = 0; slot < scalarCount; ++slot) {
        std::string name;
        float value{};
        if (!read_name(in, name) || !read_float(in, value) ||
            (slot < canonicalScalarCount && collection.scalars()[slot].name != name) ||
            !collection.set_scalar(name, value, error)) {
            if (error && error->empty()) *error = "invalid material global scalar record";
            return std::nullopt;
        }
    }
    for (std::size_t slot = 0; slot < vectorCount; ++slot) {
        std::string name;
        Float4 value{};
        if (!read_name(in, name) || !read_float(in, value.x) || !read_float(in, value.y) ||
            !read_float(in, value.z) || !read_float(in, value.w) ||
            (slot < canonicalVectorCount && collection.vectors()[slot].name != name) ||
            !collection.set_vector(name, value, error)) {
            if (error && error->empty()) *error = "invalid material global vector record";
            return std::nullopt;
        }
    }
    if (in.peek() != std::char_traits<char>::eof()) {
        if (error) *error = "material parameter collection has trailing data";
        return std::nullopt;
    }
    std::string validationError;
    if (!collection.validate(&validationError)) {
        if (error) *error = validationError;
        return std::nullopt;
    }
    return collection;
}

} // namespace dve
