#include "dve/cpu_hair.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-8F;
constexpr std::size_t kMaximumHairNameBytes = 4096U;
constexpr std::size_t kMaximumHairGuides = 1ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumHairPoints = 8ULL * 1024ULL * 1024ULL;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] Float3 clamp_length(Float3 value, float maximumLength) noexcept {
    const float squared = length_squared(value);
    const float maximumSquared = maximumLength * maximumLength;
    if (!(squared > maximumSquared) || !(squared > kEpsilon)) return value;
    return multiply(value, maximumLength / std::sqrt(squared));
}

[[nodiscard]] float safe_length(Float3 value) noexcept {
    const float squared = length_squared(value);
    return squared > 0.0F && std::isfinite(squared) ? std::sqrt(squared) : 0.0F;
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= prime;
    }
}

template<class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(T));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    const std::uint64_t size = static_cast<std::uint64_t>(value.size());
    hash_value(hash, size);
    hash_bytes(hash, value.data(), value.size());
}

[[nodiscard]] bool write_atomic(const std::filesystem::path& path,
                                std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        set_error(error, "could not create hair asset directory: " + ec.message());
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            set_error(error, "could not open temporary hair asset");
            return false;
        }
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            set_error(error, "could not write complete hair asset");
            return false;
        }
    }
    if (!std::filesystem::exists(path, ec)) {
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
        if (!ec) return true;
        const std::string publishError = ec.message();
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        set_error(error, "could not publish hair asset: " + publishError);
        return false;
    }
    std::filesystem::path backup = path;
    backup += ".bak";
    std::filesystem::remove(backup, ec);
    ec.clear();
    std::filesystem::rename(path, backup, ec);
    if (ec) {
        const std::string stageError = ec.message();
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        set_error(error, "could not stage previous hair asset: " + stageError);
        return false;
    }
    std::filesystem::rename(temporary, path, ec);
    if (!ec) {
        std::filesystem::remove(backup, ec);
        return true;
    }
    const std::string publishError = ec.message();
    std::error_code rollbackError;
    std::filesystem::rename(backup, path, rollbackError);
    std::filesystem::remove(temporary, ec);
    if (rollbackError) {
        set_error(error, "hair asset publish and rollback failed: " + publishError + "; " +
                         rollbackError.message());
    } else {
        set_error(error, "could not publish hair asset: " + publishError);
    }
    return false;
}

[[nodiscard]] bool valid_solver_settings(const CpuHairSolverSettings& settings,
                                         std::string* error) {
    const bool valid = std::isfinite(settings.fixedStepSeconds) &&
        settings.fixedStepSeconds > 0.0F &&
        settings.maximumSubsteps > 0U && settings.solverIterations > 0U &&
        settings.collisionIterations <= settings.solverIterations &&
        settings.workerChunkGuides > 0U && settings.updateRateDivisor > 0U &&
        std::isfinite(settings.maximumFrameDeltaSeconds) &&
        settings.maximumFrameDeltaSeconds > 0.0F &&
        std::isfinite(settings.maximumVelocityMetersPerSecond) &&
        settings.maximumVelocityMetersPerSecond > 0.0F &&
        std::isfinite(settings.collisionFriction) && settings.collisionFriction >= 0.0F &&
        settings.collisionFriction <= 1.0F &&
        std::isfinite(settings.windDragPerSecond) && settings.windDragPerSecond >= 0.0F &&
        std::isfinite(settings.sleepVelocityMetersPerSecond) &&
        settings.sleepVelocityMetersPerSecond >= 0.0F &&
        std::isfinite(settings.wakeRootMotionMeters) && settings.wakeRootMotionMeters >= 0.0F &&
        std::isfinite(settings.wakeWindMetersPerSecond) &&
        settings.wakeWindMetersPerSecond >= 0.0F &&
        std::isfinite(settings.teleportDistanceMeters) && settings.teleportDistanceMeters > 0.0F &&
        settings.selfCollisionIterations > 0U &&
        settings.selfCollisionMaximumNeighbors > 0U &&
        std::isfinite(settings.selfCollisionRadiusScale) &&
        settings.selfCollisionRadiusScale > 0.0F &&
        std::isfinite(settings.selfCollisionStiffness) &&
        settings.selfCollisionStiffness >= 0.0F && settings.selfCollisionStiffness <= 1.0F;
    if (!valid) set_error(error, "invalid CPU hair solver settings");
    return valid;
}

[[nodiscard]] Float3 closest_point_on_segment(Float3 point, Float3 a, Float3 b) noexcept {
    const Float3 ab = subtract(b, a);
    const float denominator = length_squared(ab);
    if (!(denominator > kEpsilon)) return a;
    const float t = std::clamp(dot(subtract(point, a), ab) / denominator, 0.0F, 1.0F);
    return add(a, multiply(ab, t));
}

[[nodiscard]] bool collision_set_valid(const HairCollisionSet& collision) noexcept {
    for (const auto& sphere : collision.spheres) {
        if (!finite(sphere.center) || !std::isfinite(sphere.radiusMeters) ||
            sphere.radiusMeters < 0.0F) return false;
    }
    for (const auto& capsule : collision.capsules) {
        if (!finite(capsule.pointA) || !finite(capsule.pointB) ||
            !std::isfinite(capsule.radiusMeters) || capsule.radiusMeters < 0.0F) return false;
    }
    if (collision.plane.enabled) {
        if (!finite(collision.plane.normal) ||
            !(length_squared(collision.plane.normal) > kEpsilon) ||
            !std::isfinite(collision.plane.offset)) return false;
    }
    return true;
}

void normalize_collision_set(HairCollisionSet& collision) noexcept {
    if (collision.plane.enabled) collision.plane.normal = normalize(collision.plane.normal);
}

[[nodiscard]] float quaternion_alignment(Quaternion a, Quaternion b) noexcept {
    a = normalize(a);
    b = normalize(b);
    return std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

struct ParsedPlyHeader {
    std::size_t vertexCount{};
    std::vector<std::string> properties{};
    bool ascii{};
};

[[nodiscard]] bool parse_ply_header(std::istream& stream, ParsedPlyHeader& header,
                                    std::string& error) {
    std::string line;
    if (!std::getline(stream, line) || line != "ply") {
        error = "Sisir PLY is missing the ply signature";
        return false;
    }
    bool inVertex = false;
    bool ended = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::string token;
        row >> token;
        if (token == "format") {
            std::string format;
            row >> format;
            header.ascii = format == "ascii";
        } else if (token == "element") {
            std::string type;
            std::size_t count{};
            row >> type >> count;
            inVertex = type == "vertex";
            if (inVertex) header.vertexCount = count;
        } else if (token == "property" && inVertex) {
            std::string type;
            std::string name;
            row >> type >> name;
            if (name.empty() || type == "list") {
                error = "unsupported Sisir PLY vertex property";
                return false;
            }
            header.properties.push_back(std::move(name));
        } else if (token == "end_header") {
            ended = true;
            break;
        }
    }
    if (!ended || !header.ascii || header.vertexCount == 0U) {
        error = "Sisir PLY must be non-empty ASCII PLY";
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::size_t> property_index(
    const ParsedPlyHeader& header, std::string_view name) {
    for (std::size_t index = 0U; index < header.properties.size(); ++index) {
        if (header.properties[index] == name) return index;
    }
    return std::nullopt;
}

struct ImportedPoint {
    Float3 position{};
    std::uint8_t anchored{};
    std::int64_t curveId{};
    std::int64_t layerId{};
    std::size_t sequence{};
};

} // namespace

std::size_t HairAsset::point_count() const noexcept {
    std::size_t count{};
    for (const HairGuide& guide : guides) count += guide.points.size();
    return count;
}

bool HairAsset::validate(std::string* error) const {
    if (name.size() > kMaximumHairNameBytes) {
        set_error(error, "hair asset name is too long");
        return false;
    }
    if (guides.empty()) {
        set_error(error, "hair asset has no guides");
        return false;
    }
    if (guides.size() > kMaximumHairGuides) {
        set_error(error, "hair asset exceeds the guide limit");
        return false;
    }
    if (!std::isfinite(pointRadiusMeters) || pointRadiusMeters < 0.0F ||
        !std::isfinite(stretchCompliance) || stretchCompliance < 0.0F ||
        !std::isfinite(bendCompliance) || bendCompliance < 0.0F ||
        !std::isfinite(linearDampingPerSecond) || linearDampingPerSecond < 0.0F) {
        set_error(error, "hair asset settings are invalid");
        return false;
    }
    std::size_t totalPoints{};
    for (const HairGuide& guide : guides) {
        if (guide.points.size() < 2U || guide.anchored.size() != guide.points.size()) {
            set_error(error, "hair guide point and anchor counts are invalid");
            return false;
        }
        if (guide.points.size() > kMaximumHairPoints ||
            totalPoints > kMaximumHairPoints - guide.points.size()) {
            set_error(error, "hair asset exceeds the point limit");
            return false;
        }
        totalPoints += guide.points.size();
        bool hasAnchor = false;
        for (std::size_t point = 0U; point < guide.points.size(); ++point) {
            if (!finite(guide.points[point])) {
                set_error(error, "hair guide contains a non-finite point");
                return false;
            }
            if (guide.anchored[point] > 1U) {
                set_error(error, "hair guide anchor flag is invalid");
                return false;
            }
            hasAnchor = hasAnchor || guide.anchored[point] != 0U;
            if (point > 0U && !(safe_length(subtract(guide.points[point], guide.points[point - 1U])) > kEpsilon)) {
                set_error(error, "hair guide contains a zero-length segment");
                return false;
            }
        }
        if (!hasAnchor) {
            set_error(error, "hair guide has no anchored point");
            return false;
        }
    }
    return true;
}

std::uint64_t hair_asset_content_hash(const HairAsset& asset) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_string(hash, asset.name);
    hash_value(hash, asset.pointRadiusMeters);
    hash_value(hash, asset.stretchCompliance);
    hash_value(hash, asset.bendCompliance);
    hash_value(hash, asset.linearDampingPerSecond);
    const std::uint64_t guideCount = static_cast<std::uint64_t>(asset.guides.size());
    hash_value(hash, guideCount);
    for (const HairGuide& guide : asset.guides) {
        hash_value(hash, guide.layerId);
        const std::uint64_t pointCount = static_cast<std::uint64_t>(guide.points.size());
        hash_value(hash, pointCount);
        for (std::size_t point = 0U; point < guide.points.size(); ++point) {
            hash_value(hash, guide.points[point]);
            hash_value(hash, guide.anchored[point]);
        }
    }
    return hash;
}

void HairAsset::recompute_hash() noexcept {
    contentHash = hair_asset_content_hash(*this);
}

HairAssetReadResult read_sisir_hair_ply(const std::filesystem::path& path,
                                        const SisirPlyImportOptions& options) {
    HairAssetReadResult result;
    if (!std::isfinite(options.scale) || options.scale == 0.0F ||
        options.maximumBytes == 0U || options.maximumPoints == 0U ||
        options.maximumGuides == 0U) {
        result.error = "invalid Sisir PLY import options";
        return result;
    }
    std::error_code ec;
    const std::uint64_t byteCount = std::filesystem::file_size(path, ec);
    if (ec || byteCount > options.maximumBytes) {
        result.error = ec ? "could not stat Sisir PLY" : "Sisir PLY exceeds byte limit";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "could not open Sisir PLY";
        return result;
    }
    ParsedPlyHeader header;
    if (!parse_ply_header(stream, header, result.error)) return result;
    if (header.vertexCount > options.maximumPoints) {
        result.error = "Sisir PLY exceeds point limit";
        return result;
    }
    const auto xIndex = property_index(header, "x");
    const auto yIndex = property_index(header, "y");
    const auto zIndex = property_index(header, "z");
    const auto anchorIndex = property_index(header, "anchor");
    const auto curveIndex = property_index(header, "curve_id");
    const auto layerIndex = property_index(header, "layer_id");
    if (!xIndex || !yIndex || !zIndex || (options.requireCurveIds && !curveIndex)) {
        result.error = "Sisir PLY is missing x/y/z or curve_id properties";
        return result;
    }

    std::vector<ImportedPoint> points;
    points.reserve(header.vertexCount);
    std::string line;
    for (std::size_t sequence = 0U; sequence < header.vertexCount; ++sequence) {
        if (!std::getline(stream, line)) {
            result.error = "Sisir PLY ended before all vertices were read";
            return result;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::vector<double> values;
        values.reserve(header.properties.size());
        double value{};
        while (row >> value) values.push_back(value);
        if (values.size() < header.properties.size()) {
            result.error = "Sisir PLY vertex row has too few values";
            return result;
        }
        const auto fetch = [&](std::size_t index) { return values[index]; };
        ImportedPoint imported;
        const double curveValue = curveIndex ? fetch(*curveIndex) : 0.0;
        const double layerValue = layerIndex ? fetch(*layerIndex) : 0.0;
        constexpr double kMaximumIndex =
            static_cast<double>(std::numeric_limits<std::uint32_t>::max());
        if (!std::isfinite(curveValue) || !std::isfinite(layerValue) || curveValue < 0.0 ||
            layerValue < 0.0 || curveValue > kMaximumIndex || layerValue > kMaximumIndex ||
            std::trunc(curveValue) != curveValue || std::trunc(layerValue) != layerValue) {
            result.error = "Sisir PLY contains an invalid curve_id or layer_id";
            return result;
        }
        const double anchorValue = anchorIndex ? fetch(*anchorIndex) : 0.0;
        if (!std::isfinite(anchorValue) ||
            (anchorIndex && anchorValue != 0.0 && anchorValue != 1.0)) {
            result.error = "Sisir PLY contains an invalid anchor value";
            return result;
        }
        imported.position = {
            static_cast<float>(fetch(*xIndex) * static_cast<double>(options.scale)),
            static_cast<float>(fetch(*yIndex) * static_cast<double>(options.scale)),
            static_cast<float>(fetch(*zIndex) * static_cast<double>(options.scale))};
        imported.anchored = anchorValue != 0.0 ? 1U : 0U;
        imported.curveId = static_cast<std::int64_t>(curveValue);
        imported.layerId = static_cast<std::int64_t>(layerValue);
        imported.sequence = sequence;
        if (!finite(imported.position)) {
            result.error = "Sisir PLY contains an invalid vertex";
            return result;
        }
        points.push_back(imported);
    }

    std::stable_sort(points.begin(), points.end(), [](const ImportedPoint& a, const ImportedPoint& b) {
        if (a.curveId != b.curveId) return a.curveId < b.curveId;
        return a.sequence < b.sequence;
    });
    result.asset.name = path.stem().string();
    std::size_t begin = 0U;
    while (begin < points.size()) {
        std::size_t end = begin + 1U;
        while (end < points.size() && points[end].curveId == points[begin].curveId) ++end;
        if (result.asset.guides.size() >= options.maximumGuides) {
            result.error = "Sisir PLY exceeds guide limit";
            return result;
        }
        if (end - begin < 2U) {
            result.error = "Sisir PLY contains a guide with fewer than two points";
            return result;
        }
        HairGuide guide;
        guide.layerId = static_cast<std::uint32_t>(points[begin].layerId);
        guide.points.reserve(end - begin);
        guide.anchored.reserve(end - begin);
        bool hasAnchor = false;
        for (std::size_t index = begin; index < end; ++index) {
            guide.points.push_back(points[index].position);
            guide.anchored.push_back(points[index].anchored);
            hasAnchor = hasAnchor || points[index].anchored != 0U;
        }
        if (!hasAnchor && options.pinFirstPointWhenMissingAnchor) guide.anchored.front() = 1U;
        result.asset.guides.push_back(std::move(guide));
        begin = end;
    }
    if (!result.asset.validate(&result.error)) return result;
    result.asset.recompute_hash();
    return result;
}

bool write_dvehair(const std::filesystem::path& path, const HairAsset& asset,
                   std::string* error) {
    if (!asset.validate(error)) return false;
    const std::uint64_t hash = hair_asset_content_hash(asset);
    if (asset.contentHash != 0U && asset.contentHash != hash) {
        set_error(error, "hair asset content hash is stale");
        return false;
    }
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "DVE_HAIR 1\n";
    stream << "name " << std::quoted(asset.name) << "\n";
    stream << "settings " << asset.pointRadiusMeters << ' ' << asset.stretchCompliance << ' '
           << asset.bendCompliance << ' ' << asset.linearDampingPerSecond << "\n";
    stream << "guides " << asset.guides.size() << "\n";
    for (const HairGuide& guide : asset.guides) {
        stream << "guide " << guide.layerId << ' ' << guide.points.size() << "\n";
        for (std::size_t point = 0U; point < guide.points.size(); ++point) {
            const Float3 p = guide.points[point];
            stream << "p " << p.x << ' ' << p.y << ' ' << p.z << ' '
                   << static_cast<unsigned>(guide.anchored[point]) << "\n";
        }
    }
    stream << "hash " << hash << "\n";
    return write_atomic(path, stream.str(), error);
}

HairAssetReadResult read_dvehair(const std::filesystem::path& path, std::uint64_t maximumBytes) {
    HairAssetReadResult result;
    std::error_code ec;
    const std::uint64_t byteCount = std::filesystem::file_size(path, ec);
    if (ec || byteCount > maximumBytes) {
        result.error = ec ? "could not stat hair asset" : "hair asset exceeds byte limit";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    std::string magic;
    unsigned version{};
    if (!stream || !(stream >> magic >> version) || magic != "DVE_HAIR" || version != 1U) {
        result.error = "invalid DVE hair header";
        return result;
    }
    std::string token;
    if (!(stream >> token) || token != "name" || !(stream >> std::quoted(result.asset.name)) ||
        result.asset.name.size() > kMaximumHairNameBytes) {
        result.error = "invalid DVE hair name";
        return result;
    }
    if (!(stream >> token) || token != "settings" ||
        !(stream >> result.asset.pointRadiusMeters >> result.asset.stretchCompliance >>
          result.asset.bendCompliance >> result.asset.linearDampingPerSecond)) {
        result.error = "invalid DVE hair settings";
        return result;
    }
    std::size_t guideCount{};
    if (!(stream >> token >> guideCount) || token != "guides" || guideCount == 0U ||
        guideCount > kMaximumHairGuides) {
        result.error = "invalid DVE hair guide count";
        return result;
    }
    result.asset.guides.resize(guideCount);
    std::size_t totalPoints{};
    for (HairGuide& guide : result.asset.guides) {
        std::size_t pointCount{};
        if (!(stream >> token >> guide.layerId >> pointCount) || token != "guide" ||
            pointCount < 2U || pointCount > 1ULL * 1024ULL * 1024ULL) {
            result.error = "invalid DVE hair guide record";
            return result;
        }
        if (pointCount > kMaximumHairPoints || totalPoints > kMaximumHairPoints - pointCount) {
            result.error = "DVE hair point count exceeds limit";
            return result;
        }
        totalPoints += pointCount;
        guide.points.resize(pointCount);
        guide.anchored.resize(pointCount);
        for (std::size_t point = 0U; point < pointCount; ++point) {
            unsigned anchored{};
            if (!(stream >> token >> guide.points[point].x >> guide.points[point].y >>
                  guide.points[point].z >> anchored) || token != "p" || anchored > 1U) {
                result.error = "invalid DVE hair point record";
                return result;
            }
            guide.anchored[point] = static_cast<std::uint8_t>(anchored);
        }
    }
    std::uint64_t storedHash{};
    if (!(stream >> token >> storedHash) || token != "hash") {
        result.error = "DVE hair asset is missing its hash";
        return result;
    }
    if (!result.asset.validate(&result.error)) return result;
    result.asset.recompute_hash();
    if (result.asset.contentHash != storedHash) {
        result.error = "DVE hair asset hash mismatch";
        return result;
    }
    return result;
}

namespace {

struct HairChunkTelemetry {
    std::uint64_t simulatedGuides{};
    std::uint64_t sleepingGuides{};
    std::uint64_t pointsIntegrated{};
    std::uint64_t stretchConstraintsSolved{};
    std::uint64_t bendConstraintsSolved{};
    std::uint64_t collisionTests{};
    std::uint64_t collisionProjections{};
    std::uint64_t selfCollisionPoints{};
    std::uint64_t selfCollisionTests{};
    std::uint64_t selfCollisionProjections{};
    std::uint64_t selfCollisionHashInsertFailures{};
    std::uint64_t nonFiniteCorrections{};
};

struct HairSelfCollisionCell {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    std::uint32_t head{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t generation{};
};

struct HairRuntimeEntry {
    HairAsset asset{};
    CpuHairInstanceDesc desc{};
    RigidTransform previousRootTransform{};
    RigidTransform pendingRootTransform{};
    bool rootTransformDirty{};
    bool forceTeleport{};
    float accumulatorSeconds{};
    std::uint64_t simulationFrame{};
    std::vector<HairStrandRange> strands{};
    std::vector<Float3> localRestPositions{};
    std::vector<Float3> positions{};
    std::vector<Float3> previousPositions{};
    std::vector<Float3> velocities{};
    std::vector<float> inverseMass{};
    std::vector<float> restSegmentLength{};
    std::vector<float> restBendLength{};
    std::vector<float> stretchLambda{};
    std::vector<float> bendLambda{};
    std::vector<std::uint8_t> collisionFlags{};
    std::vector<std::uint8_t> sleeping{};
    std::vector<std::uint32_t> sleepCounters{};
    std::vector<std::uint8_t> activeGuideMask{};
    std::vector<std::uint32_t> lineIndices{};
    std::vector<std::uint32_t> rootPoints{};
    std::vector<Float3> rootTargetOverrides{};
    std::vector<std::uint8_t> hasRootTargetOverride{};
    std::vector<std::uint32_t> pointGuideIndices{};
    std::vector<std::uint8_t> selfCollisionGuideMask{};
    std::vector<std::uint32_t> selfCollisionPointIndices{};
    std::vector<std::uint32_t> selfCollisionNextPoint{};
    std::vector<Float3> selfCollisionCorrections{};
    std::vector<HairSelfCollisionCell> selfCollisionCells{};
    std::vector<HairChunkTelemetry> chunkTelemetry{};
    std::vector<HairChunkTelemetry> selfCollisionChunkTelemetry{};
    std::uint64_t frozenGuideCount{};
    std::uint32_t selfCollisionGeneration{};
};

[[nodiscard]] std::uint32_t guide_root_point(const HairRuntimeEntry& entry,
                                             std::uint32_t guideIndex) noexcept {
    return entry.rootPoints[guideIndex];
}

void rebuild_active_mask(HairRuntimeEntry& entry) {
    const std::size_t guideCount = entry.strands.size();
    entry.activeGuideMask.assign(guideCount, 1U);
    entry.frozenGuideCount = 0U;
    const std::uint32_t limit = entry.desc.solver.maximumActiveGuides;
    if (limit == 0U || static_cast<std::size_t>(limit) >= guideCount) return;
    std::fill(entry.activeGuideMask.begin(), entry.activeGuideMask.end(), 0U);
    const std::size_t activeCount = static_cast<std::size_t>(limit);
    for (std::size_t active = 0U; active < activeCount; ++active) {
        const std::size_t index = (active * guideCount) / activeCount;
        entry.activeGuideMask[std::min(index, guideCount - 1U)] = 1U;
    }
    entry.frozenGuideCount = static_cast<std::uint64_t>(guideCount - activeCount);
}

constexpr std::uint32_t kInvalidHairPoint = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kSelfCollisionPointChunk = 256U;

[[nodiscard]] std::size_t next_power_of_two(std::size_t value) noexcept {
    std::size_t result = 1U;
    while (result < value && result <= std::numeric_limits<std::size_t>::max() / 2U) {
        result *= 2U;
    }
    return result;
}

void rebuild_self_collision_selection(HairRuntimeEntry& entry) {
    entry.selfCollisionGuideMask.assign(entry.strands.size(), 0U);
    entry.selfCollisionPointIndices.clear();
    entry.selfCollisionCells.clear();
    entry.selfCollisionChunkTelemetry.clear();
    if (!entry.desc.solver.enableSelfCollision || entry.asset.pointRadiusMeters <= 0.0F) return;

    std::vector<std::uint32_t> activeGuides;
    activeGuides.reserve(entry.strands.size());
    for (std::uint32_t guide = 0U; guide < static_cast<std::uint32_t>(entry.strands.size()); ++guide) {
        if (entry.activeGuideMask[guide] != 0U) activeGuides.push_back(guide);
    }
    std::size_t selectedCount = activeGuides.size();
    const std::uint32_t guideLimit = entry.desc.solver.maximumSelfCollisionGuides;
    if (guideLimit != 0U) selectedCount = std::min(selectedCount, static_cast<std::size_t>(guideLimit));
    if (selectedCount == 0U) return;

    for (std::size_t selected = 0U; selected < selectedCount; ++selected) {
        const std::size_t activeIndex = selectedCount == 1U
            ? activeGuides.size() / 2U
            : (selected * (activeGuides.size() - 1U)) / (selectedCount - 1U);
        entry.selfCollisionGuideMask[activeGuides[activeIndex]] = 1U;
    }
    for (std::uint32_t guide = 0U; guide < static_cast<std::uint32_t>(entry.strands.size()); ++guide) {
        if (entry.selfCollisionGuideMask[guide] == 0U) continue;
        const HairStrandRange& strand = entry.strands[guide];
        for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
            const std::uint32_t point = strand.firstPoint + local;
            if (entry.inverseMass[point] > 0.0F) entry.selfCollisionPointIndices.push_back(point);
        }
    }

    entry.selfCollisionNextPoint.assign(entry.positions.size(), kInvalidHairPoint);
    entry.selfCollisionCorrections.assign(entry.positions.size(), Float3{});
    const std::size_t desiredCells = std::max<std::size_t>(8U, entry.selfCollisionPointIndices.size() * 2U);
    entry.selfCollisionCells.resize(next_power_of_two(desiredCells));
    const std::size_t chunkCount =
        (entry.selfCollisionPointIndices.size() + kSelfCollisionPointChunk - 1U) /
        kSelfCollisionPointChunk;
    entry.selfCollisionChunkTelemetry.resize(chunkCount);
    entry.selfCollisionGeneration = 0U;
}

[[nodiscard]] std::int32_t self_collision_coordinate(float value, float inverseCellSize) noexcept {
    constexpr std::int64_t minimum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) + 1LL;
    constexpr std::int64_t maximum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) - 1LL;
    const double scaled = std::floor(static_cast<double>(value) * static_cast<double>(inverseCellSize));
    if (scaled <= static_cast<double>(minimum)) return static_cast<std::int32_t>(minimum);
    if (scaled >= static_cast<double>(maximum)) return static_cast<std::int32_t>(maximum);
    return static_cast<std::int32_t>(scaled);
}

[[nodiscard]] std::uint64_t self_collision_hash(std::int32_t x, std::int32_t y,
                                                std::int32_t z) noexcept {
    std::uint64_t hash = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x));
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0x9e3779b185ebca87ULL;
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 0xc2b2ae3d27d4eb4fULL;
    hash ^= hash >> 33U;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33U;
    return hash;
}

[[nodiscard]] bool insert_self_collision_point(HairRuntimeEntry& entry, std::uint32_t point,
                                               std::int32_t x, std::int32_t y,
                                               std::int32_t z) noexcept {
    if (entry.selfCollisionCells.empty()) return false;
    const std::size_t mask = entry.selfCollisionCells.size() - 1U;
    std::size_t slot = static_cast<std::size_t>(self_collision_hash(x, y, z)) & mask;
    for (std::size_t probe = 0U; probe < entry.selfCollisionCells.size(); ++probe) {
        HairSelfCollisionCell& cell = entry.selfCollisionCells[slot];
        if (cell.generation != entry.selfCollisionGeneration) {
            cell.x = x;
            cell.y = y;
            cell.z = z;
            cell.head = point;
            cell.generation = entry.selfCollisionGeneration;
            entry.selfCollisionNextPoint[point] = kInvalidHairPoint;
            return true;
        }
        if (cell.x == x && cell.y == y && cell.z == z) {
            entry.selfCollisionNextPoint[point] = cell.head;
            cell.head = point;
            return true;
        }
        slot = (slot + 1U) & mask;
    }
    return false;
}

[[nodiscard]] const HairSelfCollisionCell* find_self_collision_cell(
    const HairRuntimeEntry& entry, std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    if (entry.selfCollisionCells.empty()) return nullptr;
    const std::size_t mask = entry.selfCollisionCells.size() - 1U;
    std::size_t slot = static_cast<std::size_t>(self_collision_hash(x, y, z)) & mask;
    for (std::size_t probe = 0U; probe < entry.selfCollisionCells.size(); ++probe) {
        const HairSelfCollisionCell& cell = entry.selfCollisionCells[slot];
        if (cell.generation != entry.selfCollisionGeneration) return nullptr;
        if (cell.x == x && cell.y == y && cell.z == z) return &cell;
        slot = (slot + 1U) & mask;
    }
    return nullptr;
}

[[nodiscard]] Float3 coincident_self_collision_normal(std::uint32_t point,
                                                       std::uint32_t other) noexcept {
    const std::uint32_t low = std::min(point, other);
    const std::uint32_t high = std::max(point, other);
    const std::uint32_t mixed = (low * 1664525U) ^ (high * 1013904223U);
    const std::uint32_t axis = mixed % 3U;
    const float sign = point < other ? 1.0F : -1.0F;
    if (axis == 0U) return {sign, 0.0F, 0.0F};
    if (axis == 1U) return {0.0F, sign, 0.0F};
    return {0.0F, 0.0F, sign};
}

void reset_entry_state(HairRuntimeEntry& entry) {
    for (std::size_t point = 0U; point < entry.localRestPositions.size(); ++point) {
        const Float3 world = transform_point(entry.pendingRootTransform,
                                             entry.localRestPositions[point]);
        entry.positions[point] = world;
        entry.previousPositions[point] = world;
        entry.velocities[point] = {};
    }
    std::fill(entry.stretchLambda.begin(), entry.stretchLambda.end(), 0.0F);
    std::fill(entry.bendLambda.begin(), entry.bendLambda.end(), 0.0F);
    std::fill(entry.collisionFlags.begin(), entry.collisionFlags.end(), 0U);
    std::fill(entry.selfCollisionCorrections.begin(), entry.selfCollisionCorrections.end(), Float3{});
    entry.selfCollisionGeneration = 0U;
    std::fill(entry.sleeping.begin(), entry.sleeping.end(), 0U);
    std::fill(entry.sleepCounters.begin(), entry.sleepCounters.end(), 0U);
    entry.previousRootTransform = entry.pendingRootTransform;
    entry.desc.rootTransform = entry.pendingRootTransform;
    entry.rootTransformDirty = false;
    entry.forceTeleport = false;
    entry.accumulatorSeconds = 0.0F;
    entry.simulationFrame = 0U;
}

[[nodiscard]] Float3 root_target_for(const HairRuntimeEntry& entry,
                                     std::uint32_t guideIndex) noexcept {
    if (entry.hasRootTargetOverride[guideIndex] != 0U) {
        return entry.rootTargetOverrides[guideIndex];
    }
    const std::uint32_t rootPoint = guide_root_point(entry, guideIndex);
    return transform_point(entry.pendingRootTransform, entry.localRestPositions[rootPoint]);
}

void rigidly_transform_strand(HairRuntimeEntry& entry, std::uint32_t guideIndex,
                              const RigidTransform& from, const RigidTransform& to) noexcept {
    const HairStrandRange& strand = entry.strands[guideIndex];
    for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
        const std::uint32_t point = strand.firstPoint + local;
        const Float3 transformed = transform_point(to, inverse_transform_point(from, entry.positions[point]));
        const Float3 transformedPrevious = transform_point(
            to, inverse_transform_point(from, entry.previousPositions[point]));
        entry.positions[point] = transformed;
        entry.previousPositions[point] = transformedPrevious;
    }
}

void translate_strand(HairRuntimeEntry& entry, std::uint32_t guideIndex, Float3 delta) noexcept {
    const HairStrandRange& strand = entry.strands[guideIndex];
    for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
        const std::uint32_t point = strand.firstPoint + local;
        entry.positions[point] = add(entry.positions[point], delta);
        entry.previousPositions[point] = add(entry.previousPositions[point], delta);
    }
}

void clear_strand_velocity(HairRuntimeEntry& entry, std::uint32_t guideIndex) noexcept {
    const HairStrandRange& strand = entry.strands[guideIndex];
    for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
        const std::uint32_t point = strand.firstPoint + local;
        entry.velocities[point] = {};
    }
}

[[nodiscard]] bool should_teleport_transform(const HairRuntimeEntry& entry) noexcept {
    const float translation = safe_length(subtract(entry.pendingRootTransform.position,
                                                   entry.previousRootTransform.position));
    const float alignment = quaternion_alignment(entry.pendingRootTransform.rotation,
                                                 entry.previousRootTransform.rotation);
    return entry.forceTeleport || translation > entry.desc.solver.teleportDistanceMeters ||
           alignment < 0.5F;
}

void apply_root_motion(HairRuntimeEntry& entry) {
    if (!entry.rootTransformDirty) return;

    const bool teleport = should_teleport_transform(entry);
    for (std::uint32_t guide = 0U; guide < static_cast<std::uint32_t>(entry.strands.size()); ++guide) {
        const std::uint32_t rootPoint = guide_root_point(entry, guide);
        const Float3 previousRoot = entry.positions[rootPoint];
        const Float3 target = root_target_for(entry, guide);
        const Float3 rootDelta = subtract(target, previousRoot);
        const float rootMotion = safe_length(rootDelta);
        const bool frozen = entry.activeGuideMask[guide] == 0U;
        const bool asleep = entry.sleeping[guide] != 0U;

        if (teleport) {
            if (entry.hasRootTargetOverride[guide] != 0U) {
                translate_strand(entry, guide, rootDelta);
            } else {
                rigidly_transform_strand(entry, guide, entry.previousRootTransform,
                                         entry.pendingRootTransform);
            }
            clear_strand_velocity(entry, guide);
            entry.sleeping[guide] = frozen ? 1U : 0U;
            entry.sleepCounters[guide] = 0U;
        } else if (frozen || (asleep && rootMotion < entry.desc.solver.wakeRootMotionMeters)) {
            if (entry.hasRootTargetOverride[guide] != 0U) {
                translate_strand(entry, guide, rootDelta);
            } else if (entry.rootTransformDirty) {
                rigidly_transform_strand(entry, guide, entry.previousRootTransform,
                                         entry.pendingRootTransform);
            }
        } else if (rootMotion >= entry.desc.solver.wakeRootMotionMeters) {
            entry.sleeping[guide] = 0U;
            entry.sleepCounters[guide] = 0U;
        }

        const HairStrandRange& strand = entry.strands[guide];
        for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
            const std::uint32_t point = strand.firstPoint + local;
            if (entry.inverseMass[point] != 0.0F) continue;
            Float3 anchorTarget = transform_point(entry.pendingRootTransform,
                                                  entry.localRestPositions[point]);
            if (point == rootPoint && entry.hasRootTargetOverride[guide] != 0U) anchorTarget = target;
            entry.positions[point] = anchorTarget;
            entry.previousPositions[point] = anchorTarget;
            entry.velocities[point] = {};
        }
    }
    entry.previousRootTransform = entry.pendingRootTransform;
    entry.desc.rootTransform = entry.pendingRootTransform;
    entry.rootTransformDirty = false;
    entry.forceTeleport = false;
}

void solve_distance_pair(HairRuntimeEntry& entry, std::uint32_t pointA,
                         std::uint32_t pointB, float restLength, float alpha,
                         float& lambda) noexcept {
    const float inverseMassA = entry.inverseMass[pointA];
    const float inverseMassB = entry.inverseMass[pointB];
    const float inverseMassSum = inverseMassA + inverseMassB;
    if (!(inverseMassSum > 0.0F)) return;
    const Float3 delta = subtract(entry.positions[pointA], entry.positions[pointB]);
    const float distance = safe_length(delta);
    if (!(distance > kEpsilon)) return;
    const Float3 gradient = multiply(delta, 1.0F / distance);
    const float constraint = distance - restLength;
    const float denominator = inverseMassSum + alpha;
    const float deltaLambda = (-constraint - alpha * lambda) / denominator;
    lambda += deltaLambda;
    entry.positions[pointA] = add(entry.positions[pointA],
                                  multiply(gradient, inverseMassA * deltaLambda));
    entry.positions[pointB] = subtract(entry.positions[pointB],
                                       multiply(gradient, inverseMassB * deltaLambda));
}

void project_collisions(HairRuntimeEntry& entry, std::uint32_t point,
                        HairChunkTelemetry& telemetry) noexcept {
    if (entry.inverseMass[point] == 0.0F) return;
    const float pointRadius = entry.asset.pointRadiusMeters;
    Float3& position = entry.positions[point];
    for (const HairSphereCollider& sphere : entry.desc.collision.spheres) {
        ++telemetry.collisionTests;
        const Float3 delta = subtract(position, sphere.center);
        const float radius = sphere.radiusMeters + pointRadius;
        const float squared = length_squared(delta);
        if (!(squared < radius * radius)) continue;
        const float distance = squared > kEpsilon ? std::sqrt(squared) : 0.0F;
        const Float3 normal = distance > kEpsilon ? multiply(delta, 1.0F / distance)
                                                  : Float3{0.0F, 1.0F, 0.0F};
        position = add(sphere.center, multiply(normal, radius));
        entry.collisionFlags[point] = 1U;
        ++telemetry.collisionProjections;
    }
    for (const HairCapsuleCollider& capsule : entry.desc.collision.capsules) {
        ++telemetry.collisionTests;
        const Float3 closest = closest_point_on_segment(position, capsule.pointA, capsule.pointB);
        const Float3 delta = subtract(position, closest);
        const float radius = capsule.radiusMeters + pointRadius;
        const float squared = length_squared(delta);
        if (!(squared < radius * radius)) continue;
        const float distance = squared > kEpsilon ? std::sqrt(squared) : 0.0F;
        Float3 normal = distance > kEpsilon ? multiply(delta, 1.0F / distance)
                                            : normalize(cross(subtract(capsule.pointB, capsule.pointA),
                                                              {1.0F, 0.0F, 0.0F}));
        if (!(length_squared(normal) > kEpsilon)) normal = {0.0F, 1.0F, 0.0F};
        position = add(closest, multiply(normal, radius));
        entry.collisionFlags[point] = 1U;
        ++telemetry.collisionProjections;
    }
    if (entry.desc.collision.plane.enabled) {
        ++telemetry.collisionTests;
        const Float3 normal = entry.desc.collision.plane.normal;
        const float signedDistance = dot(normal, position) - entry.desc.collision.plane.offset;
        if (signedDistance < pointRadius) {
            position = add(position, multiply(normal, pointRadius - signedDistance));
            entry.collisionFlags[point] = 1U;
            ++telemetry.collisionProjections;
        }
    }
}

void begin_self_collision_grid(HairRuntimeEntry& entry,
                               float inverseCellSize,
                               HairChunkTelemetry& telemetry) noexcept {
    ++entry.selfCollisionGeneration;
    if (entry.selfCollisionGeneration == 0U) {
        for (HairSelfCollisionCell& cell : entry.selfCollisionCells) cell.generation = 0U;
        entry.selfCollisionGeneration = 1U;
    }
    for (const std::uint32_t point : entry.selfCollisionPointIndices) {
        const Float3 position = entry.positions[point];
        const std::int32_t x = self_collision_coordinate(position.x, inverseCellSize);
        const std::int32_t y = self_collision_coordinate(position.y, inverseCellSize);
        const std::int32_t z = self_collision_coordinate(position.z, inverseCellSize);
        if (!insert_self_collision_point(entry, point, x, y, z)) {
            ++telemetry.selfCollisionHashInsertFailures;
        }
        ++telemetry.selfCollisionPoints;
    }
}

void query_self_collision_chunk(HairRuntimeEntry& entry, std::size_t chunkIndex,
                                float inverseCellSize, float minimumDistance,
                                float minimumDistanceSquared) noexcept {
    HairChunkTelemetry telemetry{};
    const std::size_t begin = chunkIndex * kSelfCollisionPointChunk;
    const std::size_t end = std::min(begin + kSelfCollisionPointChunk,
                                     entry.selfCollisionPointIndices.size());
    const std::uint32_t maximumNeighbors = entry.desc.solver.selfCollisionMaximumNeighbors;
    const float stiffness = entry.desc.solver.selfCollisionStiffness;
    for (std::size_t selected = begin; selected < end; ++selected) {
        const std::uint32_t point = entry.selfCollisionPointIndices[selected];
        entry.selfCollisionCorrections[point] = {};
        const std::uint32_t pointGuide = entry.pointGuideIndices[point];
        if (entry.sleeping[pointGuide] != 0U) continue;
        const Float3 position = entry.positions[point];
        const std::int32_t cellX = self_collision_coordinate(position.x, inverseCellSize);
        const std::int32_t cellY = self_collision_coordinate(position.y, inverseCellSize);
        const std::int32_t cellZ = self_collision_coordinate(position.z, inverseCellSize);
        std::uint32_t testedNeighbors{};
        std::uint32_t contacts{};
        Float3 correction{};
        bool exhausted = false;
        for (std::int32_t offsetZ = -1; offsetZ <= 1 && !exhausted; ++offsetZ) {
            for (std::int32_t offsetY = -1; offsetY <= 1 && !exhausted; ++offsetY) {
                for (std::int32_t offsetX = -1; offsetX <= 1 && !exhausted; ++offsetX) {
                    const HairSelfCollisionCell* cell = find_self_collision_cell(
                        entry, cellX + offsetX, cellY + offsetY, cellZ + offsetZ);
                    if (cell == nullptr) continue;
                    std::uint32_t other = cell->head;
                    while (other != kInvalidHairPoint) {
                        if (other != point && entry.pointGuideIndices[other] != pointGuide) {
                            if (testedNeighbors >= maximumNeighbors) {
                                exhausted = true;
                                break;
                            }
                            ++testedNeighbors;
                            ++telemetry.selfCollisionTests;
                            const Float3 delta = subtract(position, entry.positions[other]);
                            const float distanceSquared = length_squared(delta);
                            if (distanceSquared < minimumDistanceSquared) {
                                const float distance = distanceSquared > kEpsilon
                                    ? std::sqrt(distanceSquared) : 0.0F;
                                const Float3 normal = distance > kEpsilon
                                    ? multiply(delta, 1.0F / distance)
                                    : coincident_self_collision_normal(point, other);
                                const float inverseMass = entry.inverseMass[point];
                                const std::uint32_t otherGuide = entry.pointGuideIndices[other];
                                const float otherInverseMass = entry.sleeping[otherGuide] != 0U
                                    ? 0.0F : entry.inverseMass[other];
                                const float inverseMassSum = inverseMass + otherInverseMass;
                                if (inverseMassSum > 0.0F) {
                                    const float share = inverseMass / inverseMassSum;
                                    const float penetration = minimumDistance - distance;
                                    correction = add(correction, multiply(
                                        normal, penetration * stiffness * share));
                                    ++contacts;
                                    ++telemetry.selfCollisionProjections;
                                }
                            }
                        }
                        other = entry.selfCollisionNextPoint[other];
                    }
                }
            }
        }
        if (contacts > 1U) correction = multiply(correction, 1.0F / static_cast<float>(contacts));
        entry.selfCollisionCorrections[point] = correction;
    }
    entry.selfCollisionChunkTelemetry[chunkIndex] = telemetry;
}

void apply_self_collision_corrections(HairRuntimeEntry& entry,
                                      HairChunkTelemetry& telemetry) noexcept {
    for (const std::uint32_t point : entry.selfCollisionPointIndices) {
        const Float3 correction = entry.selfCollisionCorrections[point];
        if (!(length_squared(correction) > kEpsilon)) continue;
        entry.positions[point] = add(entry.positions[point], correction);
        entry.collisionFlags[point] = 1U;
        const std::uint32_t guide = entry.pointGuideIndices[point];
        entry.sleeping[guide] = 0U;
        entry.sleepCounters[guide] = 0U;
        project_collisions(entry, point, telemetry);
    }
}

[[nodiscard]] HairChunkTelemetry run_self_collision(HairRuntimeEntry& entry,
                                                     JobSystem& jobs) {
    HairChunkTelemetry total{};
    if (!entry.desc.solver.enableSelfCollision || entry.selfCollisionPointIndices.empty()) {
        return total;
    }
    bool hasAwakeGuide = false;
    for (std::size_t guide = 0U; guide < entry.selfCollisionGuideMask.size(); ++guide) {
        if (entry.selfCollisionGuideMask[guide] != 0U && entry.sleeping[guide] == 0U) {
            hasAwakeGuide = true;
            break;
        }
    }
    if (!hasAwakeGuide) return total;
    const float radius = entry.asset.pointRadiusMeters *
        entry.desc.solver.selfCollisionRadiusScale;
    const float minimumDistance = radius * 2.0F;
    if (!(minimumDistance > kEpsilon)) return total;
    const float inverseCellSize = 1.0F / minimumDistance;
    const float minimumDistanceSquared = minimumDistance * minimumDistance;
    for (std::uint32_t iteration = 0U;
         iteration < entry.desc.solver.selfCollisionIterations; ++iteration) {
        begin_self_collision_grid(entry, inverseCellSize, total);
        jobs.parallel_for(entry.selfCollisionChunkTelemetry.size(), [&](std::size_t chunk) {
            query_self_collision_chunk(entry, chunk, inverseCellSize, minimumDistance,
                                       minimumDistanceSquared);
        });
        for (const HairChunkTelemetry& chunk : entry.selfCollisionChunkTelemetry) {
            total.selfCollisionTests += chunk.selfCollisionTests;
            total.selfCollisionProjections += chunk.selfCollisionProjections;
        }
        apply_self_collision_corrections(entry, total);
    }
    return total;
}

void integrate_strand(HairRuntimeEntry& entry, std::uint32_t guideIndex,
                      float deltaSeconds, float damping,
                      HairChunkTelemetry& telemetry) noexcept {
    const HairStrandRange& strand = entry.strands[guideIndex];
    for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
        const std::uint32_t point = strand.firstPoint + local;
        entry.stretchLambda[point] = 0.0F;
        entry.bendLambda[point] = 0.0F;
        entry.collisionFlags[point] = 0U;
        if (entry.inverseMass[point] == 0.0F) continue;
        Float3 velocity = multiply(entry.velocities[point], damping);
        const Float3 windAcceleration = multiply(
            subtract(entry.desc.windVelocity, velocity), entry.desc.solver.windDragPerSecond);
        velocity = add(velocity, multiply(add(entry.desc.gravity, windAcceleration), deltaSeconds));
        velocity = clamp_length(velocity, entry.desc.solver.maximumVelocityMetersPerSecond);
        entry.previousPositions[point] = entry.positions[point];
        entry.positions[point] = add(entry.positions[point], multiply(velocity, deltaSeconds));
        entry.velocities[point] = velocity;
        ++telemetry.pointsIntegrated;
    }
}

void solve_strand(HairRuntimeEntry& entry, std::uint32_t guideIndex,
                  float stretchAlpha, float bendAlpha,
                  HairChunkTelemetry& telemetry) noexcept {
    const HairStrandRange& strand = entry.strands[guideIndex];
    const std::uint32_t iterations = entry.desc.solver.solverIterations;
    const std::uint32_t collisionStart = iterations - entry.desc.solver.collisionIterations;
    for (std::uint32_t iteration = 0U; iteration < iterations; ++iteration) {
        const bool reverse = (iteration & 1U) != 0U;
        if (!reverse) {
            for (std::uint32_t local = 1U; local < strand.pointCount; ++local) {
                const std::uint32_t point = strand.firstPoint + local;
                solve_distance_pair(entry, point - 1U, point, entry.restSegmentLength[point],
                                    stretchAlpha, entry.stretchLambda[point]);
                ++telemetry.stretchConstraintsSolved;
            }
        } else {
            for (std::uint32_t local = strand.pointCount; local-- > 1U;) {
                const std::uint32_t point = strand.firstPoint + local;
                solve_distance_pair(entry, point - 1U, point, entry.restSegmentLength[point],
                                    stretchAlpha, entry.stretchLambda[point]);
                ++telemetry.stretchConstraintsSolved;
            }
        }
        if (entry.desc.solver.enableBendConstraints && strand.pointCount > 2U) {
            if (!reverse) {
                for (std::uint32_t local = 2U; local < strand.pointCount; ++local) {
                    const std::uint32_t point = strand.firstPoint + local;
                    solve_distance_pair(entry, point - 2U, point, entry.restBendLength[point],
                                        bendAlpha, entry.bendLambda[point]);
                    ++telemetry.bendConstraintsSolved;
                }
            } else {
                for (std::uint32_t local = strand.pointCount; local-- > 2U;) {
                    const std::uint32_t point = strand.firstPoint + local;
                    solve_distance_pair(entry, point - 2U, point, entry.restBendLength[point],
                                        bendAlpha, entry.bendLambda[point]);
                    ++telemetry.bendConstraintsSolved;
                }
            }
        }
        if (iteration >= collisionStart) {
            for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
                project_collisions(entry, strand.firstPoint + local, telemetry);
            }
        }
    }
}

void finish_strand(HairRuntimeEntry& entry, std::uint32_t guideIndex,
                   float deltaSeconds, float windSpeed,
                   HairChunkTelemetry& telemetry) noexcept {
    const HairStrandRange& strand = entry.strands[guideIndex];
    const bool evaluateSleep = entry.desc.solver.enableSleeping;
    float maximumSpeed{};
    for (std::uint32_t local = 0U; local < strand.pointCount; ++local) {
        const std::uint32_t point = strand.firstPoint + local;
        if (entry.inverseMass[point] == 0.0F) continue;
        if (!finite(entry.positions[point])) {
            entry.positions[point] = entry.previousPositions[point];
            entry.velocities[point] = {};
            ++telemetry.nonFiniteCorrections;
            continue;
        }
        Float3 velocity = multiply(subtract(entry.positions[point], entry.previousPositions[point]),
                                   1.0F / deltaSeconds);
        if (entry.collisionFlags[point] != 0U) {
            velocity = multiply(velocity, 1.0F - entry.desc.solver.collisionFriction);
        }
        velocity = clamp_length(velocity, entry.desc.solver.maximumVelocityMetersPerSecond);
        entry.velocities[point] = velocity;
        if (evaluateSleep) maximumSpeed = std::max(maximumSpeed, safe_length(velocity));
    }
    if (!evaluateSleep) return;
    if (maximumSpeed <= entry.desc.solver.sleepVelocityMetersPerSecond &&
        windSpeed <= entry.desc.solver.wakeWindMetersPerSecond) {
        std::uint32_t& counter = entry.sleepCounters[guideIndex];
        if (counter < entry.desc.solver.sleepFrames) ++counter;
        if (counter >= entry.desc.solver.sleepFrames) {
            entry.sleeping[guideIndex] = 1U;
            clear_strand_velocity(entry, guideIndex);
        }
    } else {
        entry.sleepCounters[guideIndex] = 0U;
        entry.sleeping[guideIndex] = 0U;
    }
}

void simulate_chunk(HairRuntimeEntry& entry, std::size_t chunkIndex,
                    float deltaSeconds, float damping, float stretchAlpha,
                    float bendAlpha, float windSpeed) noexcept {
    HairChunkTelemetry telemetry{};
    const std::size_t chunkSize = static_cast<std::size_t>(entry.desc.solver.workerChunkGuides);
    const std::size_t begin = chunkIndex * chunkSize;
    const std::size_t end = std::min(begin + chunkSize, entry.strands.size());
    for (std::size_t index = begin; index < end; ++index) {
        const std::uint32_t guide = static_cast<std::uint32_t>(index);
        if (entry.activeGuideMask[index] == 0U) continue;
        if (entry.sleeping[index] != 0U) {
            ++telemetry.sleepingGuides;
            continue;
        }
        ++telemetry.simulatedGuides;
        integrate_strand(entry, guide, deltaSeconds, damping, telemetry);
        solve_strand(entry, guide, stretchAlpha, bendAlpha, telemetry);
        finish_strand(entry, guide, deltaSeconds, windSpeed, telemetry);
    }
    entry.chunkTelemetry[chunkIndex] = telemetry;
}

void prepare_self_collision_chunk(HairRuntimeEntry& entry, std::size_t chunkIndex,
                                  float deltaSeconds, float damping, float stretchAlpha,
                                  float bendAlpha) noexcept {
    HairChunkTelemetry telemetry{};
    const std::size_t chunkSize = static_cast<std::size_t>(entry.desc.solver.workerChunkGuides);
    const std::size_t begin = chunkIndex * chunkSize;
    const std::size_t end = std::min(begin + chunkSize, entry.strands.size());
    for (std::size_t index = begin; index < end; ++index) {
        const std::uint32_t guide = static_cast<std::uint32_t>(index);
        if (entry.activeGuideMask[index] == 0U) continue;
        if (entry.sleeping[index] != 0U) {
            ++telemetry.sleepingGuides;
            continue;
        }
        ++telemetry.simulatedGuides;
        integrate_strand(entry, guide, deltaSeconds, damping, telemetry);
        solve_strand(entry, guide, stretchAlpha, bendAlpha, telemetry);
    }
    entry.chunkTelemetry[chunkIndex] = telemetry;
}

void finish_self_collision_chunk(HairRuntimeEntry& entry, std::size_t chunkIndex,
                                 float deltaSeconds, float windSpeed) noexcept {
    HairChunkTelemetry telemetry{};
    const std::size_t chunkSize = static_cast<std::size_t>(entry.desc.solver.workerChunkGuides);
    const std::size_t begin = chunkIndex * chunkSize;
    const std::size_t end = std::min(begin + chunkSize, entry.strands.size());
    for (std::size_t index = begin; index < end; ++index) {
        if (entry.activeGuideMask[index] == 0U || entry.sleeping[index] != 0U) continue;
        finish_strand(entry, static_cast<std::uint32_t>(index), deltaSeconds,
                      windSpeed, telemetry);
    }
    entry.chunkTelemetry[chunkIndex] = telemetry;
}

void accumulate_chunk_telemetry(const HairChunkTelemetry& chunk,
                                CpuHairStepTelemetry& total) noexcept {
    total.simulatedGuides += chunk.simulatedGuides;
    total.sleepingGuides += chunk.sleepingGuides;
    total.pointsIntegrated += chunk.pointsIntegrated;
    total.stretchConstraintsSolved += chunk.stretchConstraintsSolved;
    total.bendConstraintsSolved += chunk.bendConstraintsSolved;
    total.collisionTests += chunk.collisionTests;
    total.collisionProjections += chunk.collisionProjections;
    total.selfCollisionPoints += chunk.selfCollisionPoints;
    total.selfCollisionTests += chunk.selfCollisionTests;
    total.selfCollisionProjections += chunk.selfCollisionProjections;
    total.selfCollisionHashInsertFailures += chunk.selfCollisionHashInsertFailures;
    total.nonFiniteCorrections += chunk.nonFiniteCorrections;
}

[[nodiscard]] HairRuntimeEntry make_runtime_entry(HairAsset asset,
                                                  CpuHairInstanceDesc desc) {
    HairRuntimeEntry entry;
    entry.asset = std::move(asset);
    entry.desc = std::move(desc);
    entry.previousRootTransform = entry.desc.rootTransform;
    entry.pendingRootTransform = entry.desc.rootTransform;
    entry.strands.reserve(entry.asset.guides.size());
    const std::size_t totalPoints = entry.asset.point_count();
    entry.localRestPositions.reserve(totalPoints);
    entry.positions.reserve(totalPoints);
    entry.previousPositions.reserve(totalPoints);
    entry.velocities.resize(totalPoints);
    entry.inverseMass.reserve(totalPoints);
    entry.restSegmentLength.resize(totalPoints);
    entry.restBendLength.resize(totalPoints);
    entry.stretchLambda.resize(totalPoints);
    entry.bendLambda.resize(totalPoints);
    entry.collisionFlags.resize(totalPoints);
    entry.sleeping.resize(entry.asset.guides.size());
    entry.sleepCounters.resize(entry.asset.guides.size());
    entry.rootTargetOverrides.resize(entry.asset.guides.size());
    entry.hasRootTargetOverride.resize(entry.asset.guides.size());
    entry.rootPoints.reserve(entry.asset.guides.size());
    entry.pointGuideIndices.reserve(totalPoints);
    entry.lineIndices.reserve((totalPoints - entry.asset.guides.size()) * 2U);

    std::uint32_t firstPoint{};
    for (const HairGuide& guide : entry.asset.guides) {
        const std::uint32_t pointCount = static_cast<std::uint32_t>(guide.points.size());
        entry.strands.push_back({firstPoint, pointCount, guide.layerId});
        std::uint32_t rootPoint = firstPoint;
        bool rootFound = false;
        for (std::uint32_t local = 0U; local < pointCount; ++local) {
            const Float3 rest = guide.points[local];
            const Float3 world = transform_point(entry.desc.rootTransform, rest);
            entry.localRestPositions.push_back(rest);
            entry.positions.push_back(world);
            entry.previousPositions.push_back(world);
            entry.inverseMass.push_back(guide.anchored[local] != 0U ? 0.0F : 1.0F);
            entry.pointGuideIndices.push_back(static_cast<std::uint32_t>(entry.strands.size() - 1U));
            const std::uint32_t point = firstPoint + local;
            if (guide.anchored[local] != 0U && !rootFound) {
                rootPoint = point;
                rootFound = true;
            }
            if (local > 0U) {
                entry.restSegmentLength[point] = safe_length(subtract(
                    guide.points[local], guide.points[local - 1U]));
                entry.lineIndices.push_back(point - 1U);
                entry.lineIndices.push_back(point);
            }
            if (local > 1U) {
                entry.restBendLength[point] = safe_length(subtract(
                    guide.points[local], guide.points[local - 2U]));
            }
        }
        entry.rootPoints.push_back(rootPoint);
        firstPoint += pointCount;
    }
    rebuild_active_mask(entry);
    rebuild_self_collision_selection(entry);
    const std::size_t chunkSize = static_cast<std::size_t>(entry.desc.solver.workerChunkGuides);
    const std::size_t chunkCount = (entry.strands.size() + chunkSize - 1U) / chunkSize;
    entry.chunkTelemetry.resize(chunkCount);
    return entry;
}

} // namespace

struct CpuHairWorld::Impl {
    explicit Impl(std::size_t workerCount) : jobs(workerCount) {}

    JobSystem jobs;
    CpuHairId nextId{1U};
    std::map<CpuHairId, HairRuntimeEntry> entries{};
};

CpuHairWorld::CpuHairWorld(std::size_t workerCount)
    : impl_(std::make_unique<Impl>(workerCount)) {}

CpuHairWorld::~CpuHairWorld() = default;
CpuHairWorld::CpuHairWorld(CpuHairWorld&&) noexcept = default;
CpuHairWorld& CpuHairWorld::operator=(CpuHairWorld&&) noexcept = default;

CpuHairId CpuHairWorld::create(HairAsset asset, CpuHairInstanceDesc desc,
                               std::string* error) {
    if (!asset.validate(error) || !valid_solver_settings(desc.solver, error) ||
        !finite(desc.gravity) || !finite(desc.windVelocity) ||
        !collision_set_valid(desc.collision)) {
        if (error != nullptr && error->empty()) *error = "invalid CPU hair instance";
        return kInvalidCpuHairId;
    }
    normalize_collision_set(desc.collision);
    if (asset.contentHash == 0U) asset.recompute_hash();
    const CpuHairId id = impl_->nextId++;
    impl_->entries.emplace(id, make_runtime_entry(std::move(asset), std::move(desc)));
    return id;
}

CpuHairId CpuHairWorld::create_from_asset(const std::filesystem::path& path,
                                          CpuHairInstanceDesc desc,
                                          std::string* error) {
    HairAssetReadResult loaded = read_dvehair(path);
    if (!loaded) {
        set_error(error, loaded.error);
        return kInvalidCpuHairId;
    }
    return create(std::move(loaded.asset), std::move(desc), error);
}

bool CpuHairWorld::destroy(CpuHairId id) noexcept {
    return impl_->entries.erase(id) != 0U;
}

bool CpuHairWorld::set_running(CpuHairId id, bool running) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) return false;
    found->second.desc.running = running;
    return true;
}

bool CpuHairWorld::set_visible(CpuHairId id, bool visible) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) return false;
    found->second.desc.visible = visible;
    return true;
}

bool CpuHairWorld::set_root_transform(CpuHairId id, const RigidTransform& transform,
                                      bool teleport) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || !finite(transform.position) ||
        !std::isfinite(transform.rotation.x) || !std::isfinite(transform.rotation.y) ||
        !std::isfinite(transform.rotation.z) || !std::isfinite(transform.rotation.w)) return false;
    HairRuntimeEntry& entry = found->second;
    const RigidTransform normalized = make_rigid_transform(transform.position, transform.rotation);
    const float translationSquared = length_squared(subtract(
        normalized.position, entry.pendingRootTransform.position));
    const float alignment = quaternion_alignment(normalized.rotation,
                                                 entry.pendingRootTransform.rotation);
    if (!teleport && translationSquared <= 1.0e-16F && alignment >= 0.9999999F) return true;
    entry.pendingRootTransform = normalized;
    entry.rootTransformDirty = true;
    entry.forceTeleport = entry.forceTeleport || teleport;
    return true;
}

bool CpuHairWorld::set_root_targets(CpuHairId id, std::span<const HairRootTarget> targets,
                                    bool teleport) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) return false;
    HairRuntimeEntry& entry = found->second;
    for (const HairRootTarget& target : targets) {
        if (target.guideIndex >= entry.strands.size() || !finite(target.worldPosition)) return false;
    }
    bool changed = teleport;
    for (const HairRootTarget& target : targets) {
        const std::uint32_t guide = target.guideIndex;
        changed = changed || entry.hasRootTargetOverride[guide] == 0U ||
            length_squared(subtract(entry.rootTargetOverrides[guide], target.worldPosition)) > 1.0e-16F;
        entry.rootTargetOverrides[guide] = target.worldPosition;
        entry.hasRootTargetOverride[guide] = 1U;
    }
    entry.rootTransformDirty = entry.rootTransformDirty || changed;
    entry.forceTeleport = entry.forceTeleport || teleport;
    return true;
}

bool CpuHairWorld::clear_root_targets(CpuHairId id) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) return false;
    HairRuntimeEntry& entry = found->second;
    const bool hadOverrides = std::any_of(entry.hasRootTargetOverride.begin(),
                                          entry.hasRootTargetOverride.end(),
                                          [](std::uint8_t value) { return value != 0U; });
    std::fill(entry.hasRootTargetOverride.begin(), entry.hasRootTargetOverride.end(), 0U);
    entry.rootTransformDirty = entry.rootTransformDirty || hadOverrides;
    return true;
}

bool CpuHairWorld::set_wind(CpuHairId id, Float3 windVelocity) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || !finite(windVelocity)) return false;
    found->second.desc.windVelocity = windVelocity;
    if (safe_length(windVelocity) > found->second.desc.solver.wakeWindMetersPerSecond) {
        std::fill(found->second.sleeping.begin(), found->second.sleeping.end(), 0U);
        std::fill(found->second.sleepCounters.begin(), found->second.sleepCounters.end(), 0U);
    }
    return true;
}

bool CpuHairWorld::set_gravity(CpuHairId id, Float3 gravity) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || !finite(gravity)) return false;
    found->second.desc.gravity = gravity;
    return true;
}

bool CpuHairWorld::set_collision(CpuHairId id, HairCollisionSet collision) {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || !collision_set_valid(collision)) return false;
    normalize_collision_set(collision);
    found->second.desc.collision = std::move(collision);
    return true;
}

bool CpuHairWorld::set_solver_settings(CpuHairId id, CpuHairSolverSettings settings,
                                       std::string* error) {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) {
        set_error(error, "CPU hair instance was not found");
        return false;
    }
    if (!valid_solver_settings(settings, error)) return false;
    HairRuntimeEntry& entry = found->second;
    entry.desc.solver = settings;
    rebuild_active_mask(entry);
    rebuild_self_collision_selection(entry);
    const std::size_t chunkSize = static_cast<std::size_t>(settings.workerChunkGuides);
    const std::size_t chunkCount = (entry.strands.size() + chunkSize - 1U) / chunkSize;
    entry.chunkTelemetry.resize(chunkCount);
    for (std::size_t guide = 0U; guide < entry.strands.size(); ++guide) {
        if (entry.activeGuideMask[guide] == 0U) {
            entry.sleeping[guide] = 1U;
            entry.sleepCounters[guide] = settings.sleepFrames;
        } else {
            entry.sleeping[guide] = 0U;
            entry.sleepCounters[guide] = 0U;
        }
    }
    return true;
}

bool CpuHairWorld::wake(CpuHairId id) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) return false;
    std::fill(found->second.sleeping.begin(), found->second.sleeping.end(), 0U);
    std::fill(found->second.sleepCounters.begin(), found->second.sleepCounters.end(), 0U);
    return true;
}

bool CpuHairWorld::reset(CpuHairId id) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) return false;
    reset_entry_state(found->second);
    return true;
}

bool CpuHairWorld::apply_impulse(CpuHairId id, Float3 impulse) noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || !finite(impulse)) return false;
    HairRuntimeEntry& entry = found->second;
    for (std::size_t point = 0U; point < entry.velocities.size(); ++point) {
        if (entry.inverseMass[point] > 0.0F) {
            entry.velocities[point] = add(entry.velocities[point], impulse);
        }
    }
    std::fill(entry.sleeping.begin(), entry.sleeping.end(), 0U);
    std::fill(entry.sleepCounters.begin(), entry.sleepCounters.end(), 0U);
    return true;
}

CpuHairStepTelemetry CpuHairWorld::step(float deltaSeconds) {
    CpuHairStepTelemetry telemetry;
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0F) return telemetry;
    for (auto& [id, entry] : impl_->entries) {
        (void)id;
        apply_root_motion(entry);
        if (!entry.desc.running) continue;
        ++telemetry.instancesStepped;
        telemetry.frozenGuides += entry.frozenGuideCount;
        const float acceptedDelta = std::min(deltaSeconds, entry.desc.solver.maximumFrameDeltaSeconds);
        telemetry.droppedTimeSeconds += static_cast<double>(deltaSeconds - acceptedDelta);
        entry.accumulatorSeconds += acceptedDelta;
        const bool updateFrame = ((entry.simulationFrame + 1U) %
                                  entry.desc.solver.updateRateDivisor) == 0U;
        if (!updateFrame) {
            ++entry.simulationFrame;
            continue;
        }
        const float stepSeconds = entry.desc.solver.fixedStepSeconds *
            static_cast<float>(entry.desc.solver.updateRateDivisor);
        std::uint32_t substeps{};
        while (entry.accumulatorSeconds + 1.0e-9F >= stepSeconds &&
               substeps < entry.desc.solver.maximumSubsteps) {
            const std::size_t chunkSize = static_cast<std::size_t>(entry.desc.solver.workerChunkGuides);
            const std::size_t chunkCount = (entry.strands.size() + chunkSize - 1U) / chunkSize;
            const float damping = std::exp(-entry.asset.linearDampingPerSecond * stepSeconds);
            const float inverseStepSquared = 1.0F / (stepSeconds * stepSeconds);
            const float stretchAlpha = entry.asset.stretchCompliance * inverseStepSquared;
            const float bendAlpha = entry.asset.bendCompliance * inverseStepSquared;
            const float windSpeed = entry.desc.solver.enableSleeping
                ? safe_length(entry.desc.windVelocity) : 0.0F;
            const bool useSelfCollisionPath = entry.desc.solver.enableSelfCollision &&
                !entry.selfCollisionPointIndices.empty();
            if (!useSelfCollisionPath) {
                impl_->jobs.parallel_for(chunkCount, [&](std::size_t chunk) {
                    simulate_chunk(entry, chunk, stepSeconds, damping, stretchAlpha,
                                   bendAlpha, windSpeed);
                });
                telemetry.workerBatches += static_cast<std::uint64_t>(chunkCount);
                for (std::size_t chunk = 0U; chunk < chunkCount; ++chunk) {
                    accumulate_chunk_telemetry(entry.chunkTelemetry[chunk], telemetry);
                }
            } else {
                impl_->jobs.parallel_for(chunkCount, [&](std::size_t chunk) {
                    prepare_self_collision_chunk(entry, chunk, stepSeconds, damping,
                                                 stretchAlpha, bendAlpha);
                });
                telemetry.workerBatches += static_cast<std::uint64_t>(chunkCount);
                for (std::size_t chunk = 0U; chunk < chunkCount; ++chunk) {
                    accumulate_chunk_telemetry(entry.chunkTelemetry[chunk], telemetry);
                }
                const HairChunkTelemetry selfCollision = run_self_collision(entry, impl_->jobs);
                accumulate_chunk_telemetry(selfCollision, telemetry);
                telemetry.workerBatches += static_cast<std::uint64_t>(
                    entry.selfCollisionChunkTelemetry.size()) *
                    static_cast<std::uint64_t>(entry.desc.solver.selfCollisionIterations);
                impl_->jobs.parallel_for(chunkCount, [&](std::size_t chunk) {
                    finish_self_collision_chunk(entry, chunk, stepSeconds, windSpeed);
                });
                telemetry.workerBatches += static_cast<std::uint64_t>(chunkCount);
                for (std::size_t chunk = 0U; chunk < chunkCount; ++chunk) {
                    accumulate_chunk_telemetry(entry.chunkTelemetry[chunk], telemetry);
                }
            }
            entry.accumulatorSeconds -= stepSeconds;
            ++substeps;
            ++telemetry.substeps;
        }
        if (entry.accumulatorSeconds >= stepSeconds) {
            const float retained = std::fmod(entry.accumulatorSeconds, stepSeconds);
            telemetry.droppedTimeSeconds += static_cast<double>(entry.accumulatorSeconds - retained);
            entry.accumulatorSeconds = retained;
        }
        ++entry.simulationFrame;
    }
    return telemetry;
}

CpuHairView CpuHairWorld::view(CpuHairId id) const noexcept {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end()) return {};
    const HairRuntimeEntry& entry = found->second;
    return {entry.positions, entry.previousPositions, entry.strands, entry.lineIndices,
            entry.sleeping, entry.simulationFrame, entry.desc.visible};
}

const HairAsset* CpuHairWorld::asset(CpuHairId id) const noexcept {
    const auto found = impl_->entries.find(id);
    return found == impl_->entries.end() ? nullptr : &found->second.asset;
}

std::size_t CpuHairWorld::worker_count() const noexcept {
    return impl_->jobs.worker_count();
}

HairAsset make_straight_hair_groom(std::uint32_t guideCount,
                                   std::uint32_t pointsPerGuide,
                                   float guideSpacingMeters,
                                   float segmentLengthMeters) {
    HairAsset asset;
    asset.name = "Straight Hair Groom";
    guideCount = std::max(1U, guideCount);
    pointsPerGuide = std::max(2U, pointsPerGuide);
    guideSpacingMeters = std::max(1.0e-5F, guideSpacingMeters);
    segmentLengthMeters = std::max(1.0e-5F, segmentLengthMeters);
    const std::uint32_t side = static_cast<std::uint32_t>(
        std::ceil(std::sqrt(static_cast<double>(guideCount))));
    asset.guides.reserve(guideCount);
    for (std::uint32_t guideIndex = 0U; guideIndex < guideCount; ++guideIndex) {
        const std::uint32_t xIndex = guideIndex % side;
        const std::uint32_t zIndex = guideIndex / side;
        const float x = (static_cast<float>(xIndex) - static_cast<float>(side - 1U) * 0.5F) *
                        guideSpacingMeters;
        const float z = (static_cast<float>(zIndex) - static_cast<float>(side - 1U) * 0.5F) *
                        guideSpacingMeters;
        HairGuide guide;
        guide.points.reserve(pointsPerGuide);
        guide.anchored.assign(pointsPerGuide, 0U);
        guide.anchored.front() = 1U;
        for (std::uint32_t point = 0U; point < pointsPerGuide; ++point) {
            guide.points.push_back({x, -static_cast<float>(point) * segmentLengthMeters, z});
        }
        asset.guides.push_back(std::move(guide));
    }
    asset.recompute_hash();
    return asset;
}

} // namespace dve
