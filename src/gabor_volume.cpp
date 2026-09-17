#include "dve/gabor_volume.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <sstream>
#include <type_traits>

namespace dve {
namespace {
constexpr std::array<char, 8> kMagicV1{'D','G','A','B','O','R','1','\0'};
constexpr std::array<char, 8> kMagicV2{'D','G','A','B','O','R','2','\0'};
constexpr std::uint32_t kVersionV1 = 1U;
constexpr std::uint32_t kVersionV2 = 2U;
constexpr std::size_t kMaximumPrimitives = 1'000'000U;
constexpr std::size_t kMaximumNameBytes = 4096U;
constexpr float kEpsilon = 1.0e-6F;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
[[nodiscard]] bool finite(Quaternion value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}
[[nodiscard]] Float3 min3(Float3 a, Float3 b) noexcept {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
[[nodiscard]] Float3 max3(Float3 a, Float3 b) noexcept {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
[[nodiscard]] Float3 add3(Float3 a, Float3 b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
[[nodiscard]] Float3 subtract3(Float3 a, Float3 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
[[nodiscard]] Float3 multiply3(Float3 a, float scalar) noexcept { return {a.x*scalar, a.y*scalar, a.z*scalar}; }
[[nodiscard]] float dot3(Float3 a, Float3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }

[[nodiscard]] Quaternion normalized(Quaternion q) noexcept {
    const float magnitude = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (magnitude <= kEpsilon) return {};
    const float inverse = 1.0F / magnitude;
    return {q.x*inverse, q.y*inverse, q.z*inverse, q.w*inverse};
}
[[nodiscard]] Float3 rotate_inverse(Quaternion q, Float3 value) noexcept {
    q = normalized(q);
    const Float3 vector{q.x, q.y, q.z};
    const Float3 cross{vector.y*value.z-vector.z*value.y,
                       vector.z*value.x-vector.x*value.z,
                       vector.x*value.y-vector.y*value.x};
    const Float3 cross2{vector.y*cross.z-vector.z*cross.y,
                        vector.z*cross.x-vector.x*cross.z,
                        vector.x*cross.y-vector.y*cross.x};
    return add3(subtract3(value, multiply3(cross, 2.0F*q.w)), multiply3(cross2, 2.0F));
}

[[nodiscard]] std::uint64_t hash_bytes(std::uint64_t hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}
void hash_u16(std::uint64_t& hash, std::uint16_t value) noexcept {
    const std::array<std::uint8_t, 2> bytes{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U)};
    hash = hash_bytes(hash, bytes.data(), bytes.size());
}
void hash_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 24U)};
    hash = hash_bytes(hash, bytes.data(), bytes.size());
}
void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        const std::uint8_t byte = static_cast<std::uint8_t>(value >> shift);
        hash = hash_bytes(hash, &byte, 1U);
    }
}
void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_u32(hash, std::bit_cast<std::uint32_t>(value));
}
void hash_float3(std::uint64_t& hash, Float3 value) noexcept {
    hash_float(hash, value.x); hash_float(hash, value.y); hash_float(hash, value.z);
}
void hash_quaternion(std::uint64_t& hash, Quaternion value) noexcept {
    hash_float(hash, value.x); hash_float(hash, value.y); hash_float(hash, value.z); hash_float(hash, value.w);
}

[[nodiscard]] std::uint64_t canonical_content_hash(const GaborVolumeAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash = hash_bytes(hash, asset.name.data(), asset.name.size());
    hash_float(hash, asset.material.densityMultiplier);
    hash_float3(hash, asset.material.albedoTint);
    hash_float3(hash, asset.material.emissionColor);
    hash_float(hash, asset.material.emissionIntensity);
    hash_float(hash, asset.material.anisotropy);
    hash_float(hash, asset.material.shadowStrength);
    hash_float(hash, asset.material.maximumRayDistance);
    hash_float(hash, asset.material.lodBias);
    hash_float(hash, asset.material.temporalAccumulation);
    hash_u64(hash, static_cast<std::uint64_t>(asset.primitives.size()));
    for (const auto& primitive : asset.primitives) {
        hash_float3(hash, primitive.center);
        hash_float3(hash, primitive.scale);
        hash_quaternion(hash, primitive.rotation);
        hash_float3(hash, primitive.albedo);
        hash_float(hash, primitive.opacity);
        hash_float(hash, primitive.frequency);
        hash_float(hash, primitive.extent);
        hash_u16(hash, primitive.lodLevel);
        hash_u16(hash, primitive.orientationBin);
    }
    return hash;
}
[[nodiscard]] std::uint64_t legacy_content_hash(const GaborVolumeAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash = hash_bytes(hash, asset.name.data(), asset.name.size());
    hash = hash_bytes(hash, &asset.material, sizeof(asset.material));
    for (const auto& primitive : asset.primitives) hash = hash_bytes(hash, &primitive, sizeof(primitive));
    return hash;
}

[[nodiscard]] bool write_raw(std::ostream& output, const void* data, std::size_t size) {
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
}
[[nodiscard]] bool read_raw(std::istream& input, void* data, std::size_t size) {
    input.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(input);
}
[[nodiscard]] bool write_u16(std::ostream& output, std::uint16_t value) {
    const std::array<std::uint8_t, 2> bytes{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U)};
    return write_raw(output, bytes.data(), bytes.size());
}
[[nodiscard]] bool write_u32(std::ostream& output, std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 24U)};
    return write_raw(output, bytes.data(), bytes.size());
}
[[nodiscard]] bool write_u64(std::ostream& output, std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (unsigned shift = 0; shift < 64U; shift += 8U) bytes[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
    return write_raw(output, bytes.data(), bytes.size());
}
[[nodiscard]] bool read_u16(std::istream& input, std::uint16_t& value) {
    std::array<std::uint8_t, 2> bytes{};
    if (!read_raw(input, bytes.data(), bytes.size())) return false;
    value = static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
    return true;
}
[[nodiscard]] bool read_u32(std::istream& input, std::uint32_t& value) {
    std::array<std::uint8_t, 4> bytes{};
    if (!read_raw(input, bytes.data(), bytes.size())) return false;
    value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return true;
}
[[nodiscard]] bool read_u64(std::istream& input, std::uint64_t& value) {
    std::array<std::uint8_t, 8> bytes{};
    if (!read_raw(input, bytes.data(), bytes.size())) return false;
    value = 0;
    for (unsigned index = 0; index < 8U; ++index) value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    return true;
}
[[nodiscard]] bool write_float(std::ostream& output, float value) {
    return write_u32(output, std::bit_cast<std::uint32_t>(value));
}
[[nodiscard]] bool read_float(std::istream& input, float& value) {
    std::uint32_t bits{};
    if (!read_u32(input, bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}
[[nodiscard]] bool write_float3(std::ostream& output, Float3 value) {
    return write_float(output, value.x) && write_float(output, value.y) && write_float(output, value.z);
}
[[nodiscard]] bool read_float3(std::istream& input, Float3& value) {
    return read_float(input, value.x) && read_float(input, value.y) && read_float(input, value.z);
}
[[nodiscard]] bool write_quaternion(std::ostream& output, Quaternion value) {
    return write_float(output, value.x) && write_float(output, value.y) &&
           write_float(output, value.z) && write_float(output, value.w);
}
[[nodiscard]] bool read_quaternion(std::istream& input, Quaternion& value) {
    return read_float(input, value.x) && read_float(input, value.y) &&
           read_float(input, value.z) && read_float(input, value.w);
}
[[nodiscard]] bool write_material(std::ostream& output, const GaborVolumeMaterial& material) {
    return write_float(output, material.densityMultiplier) &&
           write_float3(output, material.albedoTint) &&
           write_float3(output, material.emissionColor) &&
           write_float(output, material.emissionIntensity) &&
           write_float(output, material.anisotropy) &&
           write_float(output, material.shadowStrength) &&
           write_float(output, material.maximumRayDistance) &&
           write_float(output, material.lodBias) &&
           write_float(output, material.temporalAccumulation);
}
[[nodiscard]] bool read_material(std::istream& input, GaborVolumeMaterial& material) {
    return read_float(input, material.densityMultiplier) &&
           read_float3(input, material.albedoTint) &&
           read_float3(input, material.emissionColor) &&
           read_float(input, material.emissionIntensity) &&
           read_float(input, material.anisotropy) &&
           read_float(input, material.shadowStrength) &&
           read_float(input, material.maximumRayDistance) &&
           read_float(input, material.lodBias) &&
           read_float(input, material.temporalAccumulation);
}
[[nodiscard]] bool write_primitive(std::ostream& output, const GaborVolumePrimitive& primitive) {
    return write_float3(output, primitive.center) && write_float3(output, primitive.scale) &&
           write_quaternion(output, primitive.rotation) && write_float3(output, primitive.albedo) &&
           write_float(output, primitive.opacity) && write_float(output, primitive.frequency) &&
           write_float(output, primitive.extent) && write_u16(output, primitive.lodLevel) &&
           write_u16(output, primitive.orientationBin);
}
[[nodiscard]] bool read_primitive(std::istream& input, GaborVolumePrimitive& primitive) {
    return read_float3(input, primitive.center) && read_float3(input, primitive.scale) &&
           read_quaternion(input, primitive.rotation) && read_float3(input, primitive.albedo) &&
           read_float(input, primitive.opacity) && read_float(input, primitive.frequency) &&
           read_float(input, primitive.extent) && read_u16(input, primitive.lodLevel) &&
           read_u16(input, primitive.orientationBin);
}

struct PlyProperty { std::string type; std::string name; };
struct PlyHeader {
    bool ascii{};
    bool binaryLittle{};
    std::size_t vertexCount{};
    std::vector<PlyProperty> properties;
};

[[nodiscard]] std::optional<PlyHeader> parse_ply_header(std::ifstream& input, std::string& error) {
    std::string line;
    if (!std::getline(input, line) || line != "ply") { error = "Not a PLY file"; return std::nullopt; }
    PlyHeader header;
    bool inVertex = false;
    bool ended = false;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string tag;
        stream >> tag;
        if (tag == "format") {
            std::string format;
            stream >> format;
            header.ascii = format == "ascii";
            header.binaryLittle = format == "binary_little_endian";
        } else if (tag == "element") {
            std::string name;
            std::size_t count{};
            stream >> name >> count;
            inVertex = name == "vertex";
            if (inVertex) header.vertexCount = count;
        } else if (tag == "property" && inVertex) {
            std::string type;
            std::string name;
            stream >> type;
            if (type == "list") { error = "List properties are not supported in the vertex element"; return std::nullopt; }
            stream >> name;
            header.properties.push_back({std::move(type), std::move(name)});
        } else if (tag == "end_header") {
            ended = true;
            break;
        }
    }
    if (!ended || (!header.ascii && !header.binaryLittle)) { error = "Unsupported or incomplete PLY header"; return std::nullopt; }
    if (header.vertexCount == 0 || header.properties.empty()) { error = "PLY has no vertex primitives"; return std::nullopt; }
    return header;
}
[[nodiscard]] std::size_t scalar_size(std::string_view type) {
    if (type=="char" || type=="int8" || type=="uchar" || type=="uint8") return 1U;
    if (type=="short" || type=="int16" || type=="ushort" || type=="uint16") return 2U;
    if (type=="int" || type=="int32" || type=="uint" || type=="uint32" || type=="float" || type=="float32") return 4U;
    if (type=="double" || type=="float64" || type=="int64" || type=="uint64") return 8U;
    return 0U;
}
template<class T> [[nodiscard]] T read_little_scalar(const char* data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
        std::reverse(bytes, bytes + sizeof(T));
    }
    return value;
}
[[nodiscard]] double binary_scalar(const char* data, std::string_view type) {
    if (type=="char" || type=="int8") return read_little_scalar<std::int8_t>(data);
    if (type=="uchar" || type=="uint8") return read_little_scalar<std::uint8_t>(data);
    if (type=="short" || type=="int16") return read_little_scalar<std::int16_t>(data);
    if (type=="ushort" || type=="uint16") return read_little_scalar<std::uint16_t>(data);
    if (type=="int" || type=="int32") return read_little_scalar<std::int32_t>(data);
    if (type=="uint" || type=="uint32") return read_little_scalar<std::uint32_t>(data);
    if (type=="float" || type=="float32") return read_little_scalar<float>(data);
    if (type=="double" || type=="float64") return read_little_scalar<double>(data);
    if (type=="int64") return static_cast<double>(read_little_scalar<std::int64_t>(data));
    if (type=="uint64") return static_cast<double>(read_little_scalar<std::uint64_t>(data));
    return 0.0;
}
[[nodiscard]] double field(const std::map<std::string,double,std::less<>>& row,
                           std::initializer_list<std::string_view> names, double fallback) {
    for (std::string_view name : names) {
        const auto iterator = row.find(std::string(name));
        if (iterator != row.end()) return iterator->second;
    }
    return fallback;
}
[[nodiscard]] std::uint16_t clamp_u16(double value) {
    return static_cast<std::uint16_t>(std::clamp(value, 0.0, 65535.0));
}

[[nodiscard]] Float3 world_to_local(const GaborVolumePrimitive& primitive, Float3 position) noexcept {
    Float3 local = rotate_inverse(primitive.rotation, subtract3(position, primitive.center));
    local.x /= std::max(kEpsilon, primitive.scale.x);
    local.y /= std::max(kEpsilon, primitive.scale.y);
    local.z /= std::max(kEpsilon, primitive.scale.z);
    return local;
}
[[nodiscard]] float primitive_density(const GaborVolumePrimitive& primitive, Float3 position) noexcept {
    const Float3 local = world_to_local(primitive, position);
    const float radiusSquared = dot3(local, local);
    if (radiusSquared > primitive.extent * primitive.extent) return 0.0F;
    const float envelope = std::exp(-0.5F * radiusSquared);
    const float carrier = primitive.frequency == 0.0F
        ? 1.0F : std::max(0.0F, std::cos(primitive.frequency * local.x));
    return std::max(0.0F, primitive.opacity) * envelope * carrier;
}

[[nodiscard]] bool read_v1(std::ifstream& input, GaborImportResult& result) {
    std::uint32_t version{};
    std::uint32_t nameSize{};
    std::uint64_t count{};
    std::uint64_t storedHash{};
    GaborVolumeAsset asset;
    if (!read_raw(input, &version, sizeof(version)) || version != kVersionV1 ||
        !read_raw(input, &nameSize, sizeof(nameSize)) || !read_raw(input, &count, sizeof(count)) ||
        count > kMaximumPrimitives || nameSize > kMaximumNameBytes ||
        !read_raw(input, &asset.boundsMinimum, sizeof(asset.boundsMinimum)) ||
        !read_raw(input, &asset.boundsMaximum, sizeof(asset.boundsMaximum)) ||
        !read_raw(input, &asset.material, sizeof(asset.material)) ||
        !read_raw(input, &storedHash, sizeof(storedHash))) {
        result.error = "Invalid legacy .dgabor header";
        return false;
    }
    asset.name.resize(nameSize);
    asset.primitives.resize(static_cast<std::size_t>(count));
    if (!read_raw(input, asset.name.data(), nameSize) ||
        !read_raw(input, asset.primitives.data(), asset.primitives.size()*sizeof(GaborVolumePrimitive))) {
        result.error = "Truncated legacy .dgabor payload";
        return false;
    }
    if (legacy_content_hash(asset) != storedHash) {
        result.error = "Legacy .dgabor content hash mismatch";
        return false;
    }
    asset.recompute_bounds_and_hash();
    if (!asset.validate(&result.error)) return false;
    result.warnings.push_back("Loaded legacy DGABOR1 data; resave to migrate to canonical DGABOR2 encoding");
    result.asset = std::move(asset);
    return true;
}
} // namespace

bool GaborVolumeAsset::validate(std::string* error) const {
    const auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (name.size() > kMaximumNameBytes) return fail("Gabor volume name exceeds the supported limit");
    if (primitives.empty()) return fail("Gabor volume contains no primitives");
    if (primitives.size() > kMaximumPrimitives) return fail("Gabor volume exceeds the supported primitive limit");
    if (!finite(boundsMinimum) || !finite(boundsMaximum) ||
        boundsMinimum.x > boundsMaximum.x || boundsMinimum.y > boundsMaximum.y ||
        boundsMinimum.z > boundsMaximum.z) return fail("Invalid Gabor bounds");
    if (!std::isfinite(material.densityMultiplier) || material.densityMultiplier < 0.0F ||
        !finite(material.albedoTint) || !finite(material.emissionColor) ||
        !std::isfinite(material.emissionIntensity) || material.emissionIntensity < 0.0F ||
        !std::isfinite(material.anisotropy) || material.anisotropy < -0.99F || material.anisotropy > 0.99F ||
        !std::isfinite(material.shadowStrength) || material.shadowStrength < 0.0F || material.shadowStrength > 1.0F ||
        !std::isfinite(material.maximumRayDistance) || material.maximumRayDistance <= 0.0F ||
        !std::isfinite(material.lodBias) || !std::isfinite(material.temporalAccumulation) ||
        material.temporalAccumulation < 0.0F || material.temporalAccumulation > 1.0F) {
        return fail("Invalid Gabor volume material");
    }
    for (std::size_t index = 0; index < primitives.size(); ++index) {
        const auto& primitive = primitives[index];
        if (!finite(primitive.center) || !finite(primitive.scale) || !finite(primitive.rotation) ||
            !finite(primitive.albedo) || primitive.scale.x <= 0.0F || primitive.scale.y <= 0.0F ||
            primitive.scale.z <= 0.0F || primitive.extent <= 0.0F || primitive.opacity < 0.0F ||
            !std::isfinite(primitive.opacity) || !std::isfinite(primitive.frequency)) {
            return fail("Invalid Gabor primitive at index " + std::to_string(index));
        }
    }
    return true;
}

void GaborVolumeAsset::recompute_bounds_and_hash() {
    Float3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    Float3 maximum{-minimum.x, -minimum.y, -minimum.z};
    for (const auto& primitive : primitives) {
        const float extent = primitive.extent * std::max({primitive.scale.x, primitive.scale.y, primitive.scale.z});
        const Float3 radius{extent, extent, extent};
        minimum = min3(minimum, subtract3(primitive.center, radius));
        maximum = max3(maximum, add3(primitive.center, radius));
    }
    boundsMinimum = minimum;
    boundsMaximum = maximum;
    contentHash = canonical_content_hash(*this);
}

GaborImportResult import_gabor_ply(const std::filesystem::path& path,
                                   const GaborPlyImportOptions& options) {
    GaborImportResult result;
    std::ifstream input(path, std::ios::binary);
    if (!input) { result.error = "Could not open PLY: " + path.string(); return result; }
    std::string headerError;
    const auto header = parse_ply_header(input, headerError);
    if (!header) { result.error = headerError; return result; }
    if (header->vertexCount > options.maximumPrimitives ||
        header->vertexCount > kMaximumPrimitives) {
        result.error = "PLY primitive count exceeds the configured limit";
        return result;
    }
    std::vector<std::size_t> offsets;
    std::size_t stride = 0;
    for (const auto& property : header->properties) {
        const std::size_t bytes = scalar_size(property.type);
        if (bytes == 0) { result.error = "Unsupported PLY scalar type: " + property.type; return result; }
        offsets.push_back(stride);
        stride += bytes;
    }
    if (stride == 0 || stride > 4096U) { result.error = "PLY vertex stride is invalid or too large"; return result; }

    GaborVolumeAsset asset;
    asset.name = path.stem().string();
    asset.material.densityMultiplier = options.densityNormalization;
    asset.primitives.reserve(header->vertexCount);
    std::vector<char> rowBytes(stride);
    for (std::size_t rowIndex = 0; rowIndex < header->vertexCount; ++rowIndex) {
        std::map<std::string,double,std::less<>> row;
        if (header->ascii) {
            std::string line;
            if (!std::getline(input, line)) { result.error = "PLY ended before all primitives were read"; return result; }
            std::istringstream values(line);
            for (const auto& property : header->properties) {
                double value{};
                if (!(values >> value)) { result.error = "Malformed ASCII PLY row"; return result; }
                row[property.name] = value;
            }
        } else {
            input.read(rowBytes.data(), static_cast<std::streamsize>(rowBytes.size()));
            if (!input) { result.error = "PLY ended before all binary primitives were read"; return result; }
            for (std::size_t index = 0; index < header->properties.size(); ++index) {
                row[header->properties[index].name] =
                    binary_scalar(rowBytes.data()+offsets[index], header->properties[index].type);
            }
        }
        GaborVolumePrimitive primitive;
        primitive.center = {
            static_cast<float>(field(row,{"x"},0.0)*options.positionScale),
            static_cast<float>(field(row,{"y"},0.0)*options.positionScale),
            static_cast<float>(field(row,{"z"},0.0)*options.positionScale)};
        const auto scaleValue = [&](std::string_view name, double fallback) {
            const double value = field(row,{name},fallback);
            return static_cast<float>((options.decodeLogScales ? std::exp(value) : value) * options.positionScale);
        };
        primitive.scale = {scaleValue("scale_0",0.0), scaleValue("scale_1",0.0), scaleValue("scale_2",0.0)};
        primitive.rotation = {
            static_cast<float>(field(row,{"rot_1","qx"},0.0)),
            static_cast<float>(field(row,{"rot_2","qy"},0.0)),
            static_cast<float>(field(row,{"rot_3","qz"},0.0)),
            static_cast<float>(field(row,{"rot_0","qw"},1.0))};
        if (options.normalizeQuaternion) primitive.rotation = normalized(primitive.rotation);
        primitive.opacity = static_cast<float>(std::max(0.0, field(row,{"opacities","opacity","sigma_t","sigmat"},1.0))*options.opacityScale);
        const double omegaX = field(row,{"omega","omega_0"},0.0);
        const double omegaY = field(row,{"omega_1"},0.0);
        const double omegaZ = field(row,{"omega_2"},0.0);
        primitive.frequency = static_cast<float>(std::sqrt(omegaX*omegaX + omegaY*omegaY + omegaZ*omegaZ));
        primitive.extent = static_cast<float>(std::max(0.01, field(row,{"extent"},3.0)));
        const auto color = [&](std::initializer_list<std::string_view> names, double fallback) {
            double value = field(row, names, fallback);
            if (value > 1.0) value /= 255.0;
            return static_cast<float>(std::clamp(value,0.0,1.0));
        };
        primitive.albedo = {color({"albedo_0","red","r"},1.0),
                             color({"albedo_1","green","g"},1.0),
                             color({"albedo_2","blue","b"},1.0)};
        primitive.lodLevel = clamp_u16(field(row,{"freq_level","lod_level"},options.forcedLodLevel));
        primitive.orientationBin = clamp_u16(field(row,{"orientation_index","orientation_bin"},0.0));
        asset.primitives.push_back(primitive);
    }
    asset.recompute_bounds_and_hash();
    if (!asset.validate(&result.error)) return result;
    result.asset = std::move(asset);
    return result;
}

GaborImportResult import_gabor_pyramid(const std::filesystem::path& directory,
                                       const GaborPlyImportOptions& options) {
    GaborImportResult result;
    GaborVolumeAsset merged;
    merged.name = directory.filename().string();
    merged.material.densityMultiplier = options.densityNormalization;
    bool found = false;
    for (std::size_t level = 0; level < 256U; ++level) {
        const auto path = directory / ("root.primitives_pyr" + std::to_string(level) + ".ply");
        if (!std::filesystem::exists(path)) {
            if (found) break;
            continue;
        }
        found = true;
        GaborPlyImportOptions levelOptions = options;
        levelOptions.forcedLodLevel = static_cast<std::uint16_t>(level);
        auto imported = import_gabor_ply(path, levelOptions);
        if (!imported) { result.error = imported.error; return result; }
        result.warnings.insert(result.warnings.end(), imported.warnings.begin(), imported.warnings.end());
        merged.primitives.insert(merged.primitives.end(), imported.asset->primitives.begin(), imported.asset->primitives.end());
        if (merged.primitives.size() > options.maximumPrimitives || merged.primitives.size() > kMaximumPrimitives) {
            result.error = "Combined pyramid exceeds the configured primitive limit";
            return result;
        }
    }
    if (!found) { result.error = "No root.primitives_pyr*.ply files were found"; return result; }
    merged.recompute_bounds_and_hash();
    if (!merged.validate(&result.error)) return result;
    result.asset = std::move(merged);
    return result;
}

bool write_dgabor(const std::filesystem::path& path, const GaborVolumeAsset& source,
                  std::string* error) {
    GaborVolumeAsset asset = source;
    asset.recompute_bounds_and_hash();
    if (!asset.validate(error)) return false;
    std::error_code filesystemError;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) { if (error) *error = filesystemError.message(); return false; }
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { if (error) *error = "Could not create .dgabor file"; return false; }
    const std::uint32_t nameSize = static_cast<std::uint32_t>(asset.name.size());
    const std::uint64_t count = asset.primitives.size();
    bool ok = write_raw(output, kMagicV2.data(), kMagicV2.size()) &&
              write_u32(output, kVersionV2) && write_u32(output, nameSize) &&
              write_u64(output, count) && write_float3(output, asset.boundsMinimum) &&
              write_float3(output, asset.boundsMaximum) && write_material(output, asset.material) &&
              write_u64(output, asset.contentHash) && write_raw(output, asset.name.data(), asset.name.size());
    for (const auto& primitive : asset.primitives) ok = ok && write_primitive(output, primitive);
    output.flush();
    ok = ok && static_cast<bool>(output);
    output.close();
    if (!ok) {
        std::filesystem::remove(temporary, filesystemError);
        if (error) *error = "Could not write canonical .dgabor payload";
        return false;
    }
    std::filesystem::remove(path, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(temporary, filesystemError);
        if (error) *error = "Could not commit .dgabor file";
        return false;
    }
    return true;
}

GaborImportResult read_dgabor(const std::filesystem::path& path) {
    GaborImportResult result;
    std::ifstream input(path, std::ios::binary);
    if (!input) { result.error = "Could not open .dgabor file"; return result; }
    std::array<char,8> magic{};
    if (!read_raw(input, magic.data(), magic.size())) { result.error = "Truncated .dgabor header"; return result; }
    if (magic == kMagicV1) {
        (void)read_v1(input, result);
        return result;
    }
    if (magic != kMagicV2) { result.error = "Unsupported .dgabor magic"; return result; }
    std::uint32_t version{};
    std::uint32_t nameSize{};
    std::uint64_t count{};
    std::uint64_t storedHash{};
    GaborVolumeAsset asset;
    if (!read_u32(input, version) || version != kVersionV2 ||
        !read_u32(input, nameSize) || nameSize > kMaximumNameBytes ||
        !read_u64(input, count) || count > kMaximumPrimitives ||
        !read_float3(input, asset.boundsMinimum) || !read_float3(input, asset.boundsMaximum) ||
        !read_material(input, asset.material) || !read_u64(input, storedHash)) {
        result.error = "Invalid canonical .dgabor header";
        return result;
    }
    asset.name.resize(nameSize);
    if (!read_raw(input, asset.name.data(), nameSize)) { result.error = "Truncated .dgabor name"; return result; }
    asset.primitives.resize(static_cast<std::size_t>(count));
    for (auto& primitive : asset.primitives) {
        if (!read_primitive(input, primitive)) { result.error = "Truncated .dgabor primitive payload"; return result; }
    }
    char trailing{};
    if (input.read(&trailing, 1)) { result.error = ".dgabor contains trailing bytes"; return result; }
    asset.recompute_bounds_and_hash();
    if (asset.contentHash != storedHash) { result.error = ".dgabor content hash mismatch"; return result; }
    if (!asset.validate(&result.error)) return result;
    result.asset = std::move(asset);
    return result;
}

float gabor_lod_weight(const GaborVolumePrimitive& primitive, float projectedPixels,
                       float lodBias) noexcept {
    if (primitive.lodLevel == 0U || primitive.frequency == 0.0F) return 1.0F;
    const float scale = std::max({primitive.scale.x, primitive.scale.y, primitive.scale.z});
    const float cycles = primitive.frequency / (2.0F*std::numbers::pi_v<float>);
    const float resolvable = std::max(0.0F, projectedPixels*scale*std::exp2(lodBias));
    const float threshold = cycles*2.0F;
    return std::clamp((resolvable-threshold*0.75F)/std::max(1.0F,threshold*0.5F),0.0F,1.0F);
}

GaborVolumeFramePlan plan_gabor_volume_frame(const GaborVolumeAsset& asset,
                                              const GaborVolumeRenderSettings& settings,
                                              float projectedDiameterPixels) noexcept {
    GaborVolumeFramePlan plan;
    plan.enabled = settings.enabled && !asset.primitives.empty();
    plan.mode = settings.mode;
    plan.quality = settings.quality;
    if (!plan.enabled) return plan;
    switch (settings.quality) {
        case GaborVolumeQuality::Low: plan.raySteps = 48U; break;
        case GaborVolumeQuality::Medium: plan.raySteps = 96U; break;
        case GaborVolumeQuality::High: plan.raySteps = 160U; break;
        case GaborVolumeQuality::Cinematic: plan.raySteps = 256U; break;
    }
    for (const auto& primitive : asset.primitives) {
        const float weight = settings.continuousLod
            ? gabor_lod_weight(primitive, projectedDiameterPixels,
                               settings.lodBias + asset.material.lodBias)
            : 1.0F;
        if (weight > 0.001F) ++plan.submittedPrimitives;
        else ++plan.culledPrimitives;
    }
    plan.temporalHistory = settings.temporalAccumulation;
    plan.shadowWork = settings.castVolumeShadows || settings.receiveSceneShadows;
    return plan;
}

float evaluate_gabor_density(const GaborVolumeAsset& asset, Float3 worldPosition,
                             float projectedPixels, bool continuousLod,
                             float lodBias) noexcept {
    float density = 0.0F;
    for (const auto& primitive : asset.primitives) {
        const float weight = continuousLod
            ? gabor_lod_weight(primitive, projectedPixels, lodBias + asset.material.lodBias)
            : 1.0F;
        density += weight * primitive_density(primitive, worldPosition);
    }
    return std::max(0.0F, density*asset.material.densityMultiplier);
}

bool render_gabor_preview_ppm(const std::filesystem::path& path, const GaborVolumeAsset& asset,
                              const GaborPreviewOptions& options, std::string* error) {
    if (options.width == 0 || options.height == 0 || options.width > 4096U || options.height > 4096U ||
        options.raySteps == 0 || options.raySteps > 2048U) {
        if (error) *error = "Invalid preview dimensions or ray-step count";
        return false;
    }
    std::string validation;
    if (!asset.validate(&validation)) { if (error) *error = validation; return false; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { if (error) *error = "Could not create preview"; return false; }
    output << "P6\n" << options.width << ' ' << options.height << "\n255\n";
    const Float3 size = subtract3(asset.boundsMaximum, asset.boundsMinimum);
    const float depth = std::max(kEpsilon, size.z);
    const float step = depth/static_cast<float>(options.raySteps);
    for (std::uint32_t y = 0; y < options.height; ++y) {
        for (std::uint32_t x = 0; x < options.width; ++x) {
            const float u = (static_cast<float>(x)+0.5F)/static_cast<float>(options.width);
            const float v = (static_cast<float>(y)+0.5F)/static_cast<float>(options.height);
            Float3 position{asset.boundsMinimum.x+u*size.x,
                            asset.boundsMaximum.y-v*size.y,
                            asset.boundsMinimum.z};
            float transmittance = 1.0F;
            Float3 color{};
            for (std::uint32_t stepIndex = 0; stepIndex < options.raySteps && transmittance > 0.005F; ++stepIndex) {
                const float density = evaluate_gabor_density(asset, position,
                    static_cast<float>(options.width), true, 0.0F);
                const float alpha = 1.0F-std::exp(-density*step);
                const Float3 source = add3(asset.material.albedoTint,
                    multiply3(asset.material.emissionColor, asset.material.emissionIntensity));
                color = add3(color, multiply3(source, transmittance*alpha));
                transmittance *= 1.0F-alpha;
                position.z += step;
            }
            color = add3(color, multiply3(options.background, transmittance));
            const auto byte = [](float channel) {
                channel = std::pow(std::clamp(channel,0.0F,1.0F),1.0F/2.2F);
                return static_cast<char>(std::lround(channel*255.0F));
            };
            const std::array<char,3> rgb{byte(color.x), byte(color.y), byte(color.z)};
            output.write(rgb.data(), 3);
        }
    }
    return static_cast<bool>(output);
}

std::string gabor_volume_mode_name(GaborVolumeMode mode) {
    switch (mode) {
        case GaborVolumeMode::AbsorptionPreview: return "Absorption Preview";
        case GaborVolumeMode::EmissionAbsorption: return "Emission-Absorption";
        case GaborVolumeMode::ScatteringExperimental: return "Scattering (Experimental)";
    }
    return "Unknown";
}
std::string gabor_volume_quality_name(GaborVolumeQuality quality) {
    switch (quality) {
        case GaborVolumeQuality::Low: return "Low";
        case GaborVolumeQuality::Medium: return "Medium";
        case GaborVolumeQuality::High: return "High";
        case GaborVolumeQuality::Cinematic: return "Cinematic";
    }
    return "Unknown";
}

} // namespace dve
