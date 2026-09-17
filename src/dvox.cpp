#include "dve/dvox.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dve {

namespace {

constexpr std::array<char, 4> kMagic{'D', 'V', 'O', 'X'};
constexpr std::uint16_t kVersionMajor = 1;
constexpr std::uint16_t kVersionMinor = 2;
constexpr std::uint16_t kLegacyVersionMinor = 0;
constexpr std::uint32_t kHeaderSize = 64;
constexpr std::uint32_t kBrickRecordSize = 32;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template <class T>
void hash_scalar(std::uint64_t& hash, T value) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    for (std::size_t i = 0; i < sizeof(T); ++i) hash_byte(hash, bytes[i]);
}

[[nodiscard]] std::uint64_t content_hash(const VoxelObject& object) {
    std::uint64_t hash = kFnvOffset;
    hash_scalar(hash, object.id());
    for (BrickKey key : object.bricks().sorted_keys()) {
        const Brick* brick = object.find_brick(key);
        if (brick == nullptr || brick->empty()) continue;
        hash_scalar(hash, key.x);
        hash_scalar(hash, key.y);
        hash_scalar(hash, key.z);
        for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) hash_byte(hash, brick->material(i));
    }
    return hash;
}

void hash_u16_le(std::uint64_t& hash, std::uint16_t value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xFFU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void hash_u32_le(std::uint64_t& hash, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void hash_u64_le(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void hash_span(std::uint64_t& hash, std::span<const std::byte> bytes) noexcept {
    for (const std::byte value : bytes) hash_byte(hash, std::to_integer<std::uint8_t>(value));
}

[[nodiscard]] std::uint64_t content_hash_v1_1(
    std::uint16_t versionMinor,
    std::uint32_t flags,
    float voxelSizeMeters,
    std::uint64_t objectId,
    std::uint32_t materialCount,
    std::uint32_t brickCount,
    std::span<const std::byte> materialBytes,
    std::span<const std::byte> tableBytes,
    std::span<const std::byte> payloadBytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_u16_le(hash, kVersionMajor);
    hash_u16_le(hash, versionMinor);
    hash_u32_le(hash, flags);
    hash_u32_le(hash, std::bit_cast<std::uint32_t>(voxelSizeMeters));
    hash_u64_le(hash, objectId);
    hash_u32_le(hash, materialCount);
    hash_u32_le(hash, brickCount);
    hash_u64_le(hash, materialBytes.size());
    hash_u64_le(hash, tableBytes.size());
    hash_u64_le(hash, payloadBytes.size());
    hash_span(hash, materialBytes);
    hash_span(hash, tableBytes);
    hash_span(hash, payloadBytes);
    return hash;
}

class ByteWriter {
public:
    void write_u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void write_u16(std::uint16_t value) {
        write_u8(static_cast<std::uint8_t>(value & 0xFFU));
        write_u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    }
    void write_u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) write_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void write_i32(std::int32_t value) { write_u32(std::bit_cast<std::uint32_t>(value)); }
    void write_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) write_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void write_float(float value) { write_u32(std::bit_cast<std::uint32_t>(value)); }
    void write_bytes(const void* data, std::size_t size) {
        const auto* begin = static_cast<const std::byte*>(data);
        bytes_.insert(bytes_.end(), begin, begin + static_cast<std::ptrdiff_t>(size));
    }
    void append(const ByteWriter& other) { bytes_.insert(bytes_.end(), other.bytes_.begin(), other.bytes_.end()); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    std::vector<std::byte> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(const std::vector<std::byte>& bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - cursor_; }
    [[nodiscard]] std::size_t position() const noexcept { return cursor_; }
    void seek(std::size_t position) {
        if (position > bytes_.size()) throw std::runtime_error("DVOX seek exceeds file");
        cursor_ = position;
    }
    std::uint8_t read_u8() {
        require(1);
        return std::to_integer<std::uint8_t>(bytes_[cursor_++]);
    }
    std::uint16_t read_u16() {
        const std::uint16_t a = read_u8();
        const std::uint16_t b = read_u8();
        return static_cast<std::uint16_t>(a | (b << 8U));
    }
    std::uint32_t read_u32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(read_u8()) << shift;
        return value;
    }
    std::int32_t read_i32() { return std::bit_cast<std::int32_t>(read_u32()); }
    std::uint64_t read_u64() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(read_u8()) << shift;
        return value;
    }
    float read_float() {
        const float value = std::bit_cast<float>(read_u32());
        if (!std::isfinite(value)) throw std::runtime_error("DVOX contains non-finite float");
        return value;
    }
    std::string read_string(std::size_t size) {
        require(size);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
        cursor_ += size;
        return result;
    }
    void read_bytes(void* destination, std::size_t size) {
        require(size);
        std::memcpy(destination, bytes_.data() + cursor_, size);
        cursor_ += size;
    }

private:
    const std::vector<std::byte>& bytes_;
    std::size_t cursor_{};

    void require(std::size_t size) const {
        if (size > bytes_.size() - cursor_) throw std::runtime_error("truncated DVOX file");
    }
};

[[nodiscard]] std::vector<std::byte> read_all(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("unable to open DVOX file");
    const std::streamoff size = input.tellg();
    if (size < 0) throw std::runtime_error("unable to determine DVOX size");
    const std::uint64_t unsignedSize = static_cast<std::uint64_t>(size);
    if (unsignedSize > maximumBytes) throw std::runtime_error("DVOX exceeds configured size limit");
    if (unsignedSize > std::numeric_limits<std::size_t>::max()) throw std::runtime_error("DVOX exceeds addressable memory");
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(unsignedSize));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("unable to read DVOX file");
    }
    return bytes;
}

struct EncodedBrick {
    BrickKey key{};
    BrickEncoding encoding{BrickEncoding::Empty};
    std::uint32_t generation{};
    std::uint64_t payloadOffset{};
    std::uint32_t payloadSize{};
    ByteWriter payload;
};

void write_occupancy(ByteWriter& writer, const Bitset512& occupancy) {
    for (std::uint64_t word : occupancy.words) writer.write_u64(word);
}

[[nodiscard]] EncodedBrick encode_brick(BrickKey key, const Brick& brick) {
    EncodedBrick encoded;
    encoded.key = key;
    encoded.encoding = brick.encoding();
    encoded.generation = brick.generation();
    const Bitset512 occupancy = brick.occupancy();
    switch (brick.encoding()) {
    case BrickEncoding::Empty:
        break;
    case BrickEncoding::UniformSolid:
        encoded.payload.write_u8(brick.material(0));
        break;
    case BrickEncoding::MaskUniform: {
        write_occupancy(encoded.payload, occupancy);
        MaterialId material = kAirMaterial;
        occupancy.for_each_set([&](std::uint16_t index) { if (material == kAirMaterial) material = brick.material(index); });
        encoded.payload.write_u8(material);
        break;
    }
    case BrickEncoding::LocalPalette4: {
        write_occupancy(encoded.payload, occupancy);
        std::array<MaterialId, 16> palette{};
        std::map<MaterialId, std::uint8_t> indices;
        std::uint8_t paletteSize = 1;
        indices.emplace(kAirMaterial, 0);
        for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) {
            const MaterialId material = brick.material(i);
            if (!indices.contains(material)) {
                if (paletteSize >= palette.size()) throw std::runtime_error("LocalPalette4 brick has too many materials");
                indices.emplace(material, paletteSize);
                palette[paletteSize] = material;
                ++paletteSize;
            }
        }
        encoded.payload.write_u8(paletteSize);
        encoded.payload.write_bytes(palette.data(), palette.size());
        std::array<std::uint8_t, 256> packed{};
        for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) {
            const std::uint8_t paletteIndex = indices.at(brick.material(i));
            const std::size_t byteIndex = i >> 1U;
            if ((i & 1U) == 0) packed[byteIndex] = paletteIndex;
            else packed[byteIndex] |= static_cast<std::uint8_t>(paletteIndex << 4U);
        }
        encoded.payload.write_bytes(packed.data(), packed.size());
        break;
    }
    case BrickEncoding::Palette8:
        write_occupancy(encoded.payload, occupancy);
        for (std::uint16_t i = 0; i < kBrickVoxelCount; ++i) encoded.payload.write_u8(brick.material(i));
        break;
    }
    encoded.payloadSize = static_cast<std::uint32_t>(encoded.payload.size());
    return encoded;
}

void write_material(ByteWriter& writer, const VoxelMaterialDefinition& material) {
    if (material.name.size() > std::numeric_limits<std::uint16_t>::max()) throw std::runtime_error("material name is too long");
    if (material.layers.size() > kMaximumVoxelMaterialLayers) throw std::runtime_error("material has too many layers");
    writer.write_u16(static_cast<std::uint16_t>(material.name.size()));
    writer.write_bytes(material.name.data(), material.name.size());
    writer.write_float(material.baseColor.x);
    writer.write_float(material.baseColor.y);
    writer.write_float(material.baseColor.z);
    writer.write_float(material.baseColor.w);
    writer.write_float(material.emissive.x);
    writer.write_float(material.emissive.y);
    writer.write_float(material.emissive.z);
    writer.write_float(material.metallic);
    writer.write_float(material.roughness);
    writer.write_float(material.densityKilogramsPerCubicMeter);
    writer.write_float(material.structuralStrength);
    writer.write_float(material.fractureResistance);
    writer.write_float(material.flammability);
    writer.write_float(material.thermalConductivity);
    std::uint32_t flags = 0;
    if (material.transparent) flags |= 1U;
    if (material.structural) flags |= 2U;
    writer.write_u32(flags);
    // v1.2 render extension. The original v1.0/v1.1 prefix remains byte-identical above.
    writer.write_float(material.specular);
    writer.write_u8(static_cast<std::uint8_t>(material.shadingModel));
    writer.write_u8(static_cast<std::uint8_t>(material.blendMode));
    writer.write_u16(0);
    writer.write_float(material.subsurfaceScatterDistanceMeters);
    writer.write_float(material.subsurfaceColor.x);
    writer.write_float(material.subsurfaceColor.y);
    writer.write_float(material.subsurfaceColor.z);
    writer.write_float(material.clearCoat);
    writer.write_float(material.clearCoatRoughness);
    writer.write_float(material.foliageColor.x);
    writer.write_float(material.foliageColor.y);
    writer.write_float(material.foliageColor.z);
    writer.write_float(material.foliageTransmittance);
    writer.write_float(material.foliageWrap);
    writer.write_u8(static_cast<std::uint8_t>(material.layers.size()));
    writer.write_u8(0); writer.write_u8(0); writer.write_u8(0);
    for (const VoxelMaterialLayer& layer : material.layers) {
        writer.write_u8(layer.sourceMaterial);
        writer.write_u8(static_cast<std::uint8_t>(layer.blendMode));
        writer.write_u8(layer.enabled ? 1U : 0U);
        writer.write_u8(0);
        writer.write_float(layer.weight);
    }
}

[[nodiscard]] VoxelMaterialDefinition read_material(ByteReader& reader, std::uint16_t minor) {
    VoxelMaterialDefinition material;
    material.name = reader.read_string(reader.read_u16());
    material.baseColor = {reader.read_float(), reader.read_float(), reader.read_float(), reader.read_float()};
    material.emissive = {reader.read_float(), reader.read_float(), reader.read_float()};
    material.metallic = reader.read_float();
    material.roughness = reader.read_float();
    material.densityKilogramsPerCubicMeter = reader.read_float();
    material.structuralStrength = reader.read_float();
    material.fractureResistance = reader.read_float();
    material.flammability = reader.read_float();
    material.thermalConductivity = reader.read_float();
    const std::uint32_t flags = reader.read_u32();
    material.transparent = (flags & 1U) != 0;
    material.structural = (flags & 2U) != 0;
    material.blendMode = material.transparent ? MaterialBlendMode::Translucent : MaterialBlendMode::Opaque;
    if (minor >= 2U) {
        material.specular = reader.read_float();
        const std::uint8_t shading = reader.read_u8();
        const std::uint8_t blend = reader.read_u8();
        reader.read_u16();
        if (shading > static_cast<std::uint8_t>(MaterialShadingModel::ClearCoat) ||
            blend > static_cast<std::uint8_t>(MaterialBlendMode::Translucent))
            throw std::runtime_error("invalid DVOX material shading model");
        material.shadingModel = static_cast<MaterialShadingModel>(shading);
        material.blendMode = static_cast<MaterialBlendMode>(blend);
        material.subsurfaceScatterDistanceMeters = reader.read_float();
        material.subsurfaceColor = {reader.read_float(), reader.read_float(), reader.read_float()};
        material.clearCoat = reader.read_float();
        material.clearCoatRoughness = reader.read_float();
        material.foliageColor = {reader.read_float(), reader.read_float(), reader.read_float()};
        material.foliageTransmittance = reader.read_float();
        material.foliageWrap = reader.read_float();
        const std::uint8_t layerCount = reader.read_u8();
        reader.read_u8(); reader.read_u8(); reader.read_u8();
        if (layerCount > kMaximumVoxelMaterialLayers) throw std::runtime_error("DVOX material layer count exceeds four");
        material.layers.reserve(layerCount);
        for (std::uint8_t i = 0; i < layerCount; ++i) {
            VoxelMaterialLayer layer;
            layer.sourceMaterial = reader.read_u8();
            const std::uint8_t mode = reader.read_u8();
            layer.enabled = reader.read_u8() != 0;
            reader.read_u8();
            layer.weight = reader.read_float();
            if (mode > static_cast<std::uint8_t>(MaterialLayerBlendMode::Additive) ||
                !std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F)
                throw std::runtime_error("invalid DVOX material layer");
            layer.blendMode = static_cast<MaterialLayerBlendMode>(mode);
            material.layers.push_back(layer);
        }
    }
    return material;
}

[[nodiscard]] Bitset512 read_occupancy(ByteReader& reader) {
    Bitset512 occupancy;
    for (std::uint64_t& word : occupancy.words) word = reader.read_u64();
    return occupancy;
}

} // namespace

bool write_dvox(
    const std::filesystem::path& path,
    const CookedVoxelAsset& asset,
    const DvoxWriteOptions& options,
    std::string* error) {
    try {
        if (!asset.object.validate()) throw std::runtime_error("cannot serialize invalid VoxelObject");
        if (asset.materials.size() > 256) throw std::runtime_error("DVOX supports at most 256 materials");

        ByteWriter materials;
        for (const VoxelMaterialDefinition& material : asset.materials) write_material(materials, material);

        std::vector<EncodedBrick> bricks;
        for (BrickKey key : asset.object.bricks().sorted_keys()) {
            const Brick* brick = asset.object.find_brick(key);
            if (brick == nullptr || brick->empty()) continue;
            bricks.push_back(encode_brick(key, *brick));
        }
        ByteWriter payloads;
        for (EncodedBrick& brick : bricks) {
            brick.payloadOffset = payloads.size();
            payloads.append(brick.payload);
        }

        ByteWriter table;
        for (const EncodedBrick& brick : bricks) {
            table.write_i32(brick.key.x);
            table.write_i32(brick.key.y);
            table.write_i32(brick.key.z);
            table.write_u8(static_cast<std::uint8_t>(brick.encoding));
            table.write_u8(0);
            table.write_u8(0);
            table.write_u8(0);
            table.write_u32(brick.generation);
            table.write_u64(brick.payloadOffset);
            table.write_u32(brick.payloadSize);
        }
        if (table.size() != bricks.size() * kBrickRecordSize) throw std::runtime_error("internal DVOX brick table size mismatch");

        const std::uint32_t flags = options.includeDiagnostics ? 1U : 0U;
        const std::uint32_t materialCount = static_cast<std::uint32_t>(asset.materials.size());
        const std::uint32_t brickCount = static_cast<std::uint32_t>(bricks.size());
        const std::uint64_t fullContentHash = content_hash_v1_1(
            kVersionMinor, flags, asset.voxelSizeMeters, asset.object.id(), materialCount, brickCount,
            materials.bytes(), table.bytes(), payloads.bytes());

        ByteWriter header;
        header.write_bytes(kMagic.data(), kMagic.size());
        header.write_u16(kVersionMajor);
        header.write_u16(kVersionMinor);
        header.write_u32(flags);
        header.write_float(asset.voxelSizeMeters);
        header.write_u64(asset.object.id());
        header.write_u32(materialCount);
        header.write_u32(brickCount);
        header.write_u64(materials.size());
        header.write_u64(table.size());
        header.write_u64(payloads.size());
        header.write_u64(fullContentHash);
        if (header.size() != kHeaderSize) throw std::runtime_error("internal DVOX header size mismatch");

        std::ofstream output(path, std::ios::binary);
        if (!output) throw std::runtime_error("unable to open DVOX output");
        const auto write = [&](const ByteWriter& writer) {
            if (!writer.bytes().empty()) {
                output.write(reinterpret_cast<const char*>(writer.bytes().data()),
                             static_cast<std::streamsize>(writer.bytes().size()));
            }
        };
        write(header);
        write(materials);
        write(table);
        write(payloads);
        if (!output) throw std::runtime_error("failed while writing DVOX output");
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

DvoxReadResult read_dvox(const std::filesystem::path& path, std::uint64_t maximumBytes) {
    try {
        const std::vector<std::byte> bytes = read_all(path, maximumBytes);
        ByteReader reader(bytes);
        std::array<char, 4> magic{};
        reader.read_bytes(magic.data(), magic.size());
        if (magic != kMagic) throw std::runtime_error("invalid DVOX magic");
        const std::uint16_t major = reader.read_u16();
        const std::uint16_t minor = reader.read_u16();
        if (major != kVersionMajor || minor > kVersionMinor) {
            throw std::runtime_error("unsupported DVOX version");
        }
        const std::uint32_t flags = reader.read_u32();
        (void)flags;
        const float voxelSize = reader.read_float();
        if (!(voxelSize > 0.0F)) throw std::runtime_error("invalid DVOX voxel size");
        const std::uint64_t objectId = reader.read_u64();
        const std::uint32_t materialCount = reader.read_u32();
        const std::uint32_t brickCount = reader.read_u32();
        const std::uint64_t materialBytes = reader.read_u64();
        const std::uint64_t tableBytes = reader.read_u64();
        const std::uint64_t payloadBytes = reader.read_u64();
        const std::uint64_t expectedHash = reader.read_u64();
        if (materialCount > 256U) throw std::runtime_error("DVOX material count exceeds 256");
        if (tableBytes != static_cast<std::uint64_t>(brickCount) * kBrickRecordSize) {
            throw std::runtime_error("DVOX brick table has invalid size");
        }
        const auto checked_add = [](std::uint64_t a, std::uint64_t b) {
            if (b > std::numeric_limits<std::uint64_t>::max() - a) {
                throw std::runtime_error("DVOX section sizes overflow");
            }
            return a + b;
        };
        std::uint64_t total = kHeaderSize;
        total = checked_add(total, materialBytes);
        total = checked_add(total, tableBytes);
        total = checked_add(total, payloadBytes);
        if (total != bytes.size()) throw std::runtime_error("DVOX section sizes do not match file length");
        if (minor >= 1U) {
            const std::size_t materialOffset = kHeaderSize;
            const std::size_t tableOffset = materialOffset + static_cast<std::size_t>(materialBytes);
            const std::size_t payloadOffset = tableOffset + static_cast<std::size_t>(tableBytes);
            const std::uint64_t actualHash = content_hash_v1_1(
                minor, flags, voxelSize, objectId, materialCount, brickCount,
                std::span<const std::byte>(bytes).subspan(materialOffset, static_cast<std::size_t>(materialBytes)),
                std::span<const std::byte>(bytes).subspan(tableOffset, static_cast<std::size_t>(tableBytes)),
                std::span<const std::byte>(bytes).subspan(payloadOffset, static_cast<std::size_t>(payloadBytes)));
            if (actualHash != expectedHash) throw std::runtime_error("DVOX full-content hash mismatch");
        }

        DvoxReadResult result(objectId);
        result.asset.voxelSizeMeters = voxelSize;
        const std::size_t materialEnd = reader.position() + static_cast<std::size_t>(materialBytes);
        result.asset.materials.reserve(materialCount);
        for (std::uint32_t i = 0; i < materialCount; ++i) result.asset.materials.push_back(read_material(reader, minor));
        if (reader.position() != materialEnd) throw std::runtime_error("DVOX material section length mismatch");

        struct BrickRecord {
            BrickKey key{};
            BrickEncoding encoding{};
            std::uint32_t generation{};
            std::uint64_t payloadOffset{};
            std::uint32_t payloadSize{};
        };
        std::vector<BrickRecord> records;
        records.reserve(brickCount);
        std::optional<BrickKey> previousKey;
        std::uint64_t expectedPayloadOffset = 0;
        for (std::uint32_t i = 0; i < brickCount; ++i) {
            BrickRecord record;
            record.key = {reader.read_i32(), reader.read_i32(), reader.read_i32()};
            const std::uint8_t encoding = reader.read_u8();
            reader.read_u8();
            reader.read_u8();
            reader.read_u8();
            if (encoding == static_cast<std::uint8_t>(BrickEncoding::Empty) ||
                encoding > static_cast<std::uint8_t>(BrickEncoding::Palette8)) {
                throw std::runtime_error("invalid or empty brick encoding in DVOX table");
            }
            record.encoding = static_cast<BrickEncoding>(encoding);
            record.generation = reader.read_u32();
            record.payloadOffset = reader.read_u64();
            record.payloadSize = reader.read_u32();
            if (previousKey && !(previousKey.value() < record.key)) {
                throw std::runtime_error("DVOX brick keys must be strictly sorted and unique");
            }
            previousKey = record.key;
            if (record.payloadOffset != expectedPayloadOffset) {
                throw std::runtime_error("DVOX brick payload ranges must be contiguous and canonical");
            }
            if (record.payloadSize > payloadBytes - expectedPayloadOffset) {
                throw std::runtime_error("brick payload exceeds DVOX payload section");
            }
            expectedPayloadOffset += record.payloadSize;
            records.push_back(record);
        }
        if (expectedPayloadOffset != payloadBytes) {
            throw std::runtime_error("DVOX payload section contains unreferenced bytes");
        }
        const std::size_t payloadStart = reader.position();
        result.asset.object.reserve_bricks(records.size());
        for (const BrickRecord& record : records) {
            reader.seek(payloadStart + static_cast<std::size_t>(record.payloadOffset));
            const std::size_t payloadEnd = reader.position() + record.payloadSize;
            switch (record.encoding) {
            case BrickEncoding::Empty:
                break;
            case BrickEncoding::UniformSolid:
                result.asset.object.fill_brick(record.key, reader.read_u8());
                break;
            case BrickEncoding::MaskUniform: {
                const Bitset512 occupancy = read_occupancy(reader);
                const MaterialId material = reader.read_u8();
                occupancy.for_each_set([&](std::uint16_t index) {
                    result.asset.object.set_voxel(global_from_local(record.key, local_from_index_unchecked(index)), material);
                });
                break;
            }
            case BrickEncoding::LocalPalette4: {
                const Bitset512 occupancy = read_occupancy(reader);
                const std::uint8_t paletteSize = reader.read_u8();
                if (paletteSize == 0 || paletteSize > 16) throw std::runtime_error("invalid LocalPalette4 palette size");
                std::array<MaterialId, 16> palette{};
                reader.read_bytes(palette.data(), palette.size());
                std::array<std::uint8_t, 256> packed{};
                reader.read_bytes(packed.data(), packed.size());
                occupancy.for_each_set([&](std::uint16_t index) {
                    const std::uint8_t byte = packed[index >> 1U];
                    const std::uint8_t paletteIndex = (index & 1U) == 0 ? static_cast<std::uint8_t>(byte & 0x0FU)
                                                                         : static_cast<std::uint8_t>(byte >> 4U);
                    if (paletteIndex >= paletteSize) throw std::runtime_error("LocalPalette4 index exceeds palette");
                    result.asset.object.set_voxel(global_from_local(record.key, local_from_index_unchecked(index)), palette[paletteIndex]);
                });
                break;
            }
            case BrickEncoding::Palette8: {
                const Bitset512 occupancy = read_occupancy(reader);
                std::array<MaterialId, kBrickVoxelCount> materials{};
                reader.read_bytes(materials.data(), materials.size());
                occupancy.for_each_set([&](std::uint16_t index) {
                    result.asset.object.set_voxel(global_from_local(record.key, local_from_index_unchecked(index)), materials[index]);
                });
                break;
            }
            }
            if (reader.position() != payloadEnd) throw std::runtime_error("brick payload size does not match encoding");
        }
        if (!result.asset.object.validate()) throw std::runtime_error("decoded DVOX object failed validation");
        if (minor == kLegacyVersionMinor && content_hash(result.asset.object) != expectedHash) {
            throw std::runtime_error("DVOX legacy content hash mismatch");
        }
        result.asset.stats.outputVoxels = result.asset.object.occupied_voxel_count();
        result.asset.stats.outputBricks = result.asset.object.brick_count();
        result.success = true;
        return result;
    } catch (const std::exception& exception) {
        DvoxReadResult failed;
        failed.error = exception.what();
        failed.success = false;
        return failed;
    }
}

} // namespace dve
