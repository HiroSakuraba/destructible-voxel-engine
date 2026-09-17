#include "dve/soft_body.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace dve {
namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool write_atomic(const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create soft-body asset directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary soft-body asset");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete soft-body asset");
        }
    }
    if (!std::filesystem::exists(path, ec)) {
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
        if (!ec) return true;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(error, "could not publish soft-body asset: " + ec.message());
    }
    std::filesystem::path backup = path;
    backup += ".bak";
    std::filesystem::remove(backup, ec);
    ec.clear();
    std::filesystem::rename(path, backup, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(error, "could not stage previous soft-body asset: " + ec.message());
    }
    std::filesystem::rename(temporary, path, ec);
    if (!ec) {
        std::error_code ignored;
        std::filesystem::remove(backup, ignored);
        return true;
    }
    const std::string publishError = ec.message();
    std::error_code rollbackError;
    std::filesystem::rename(backup, path, rollbackError);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (rollbackError)
        return fail(error, "soft-body publish and rollback failed: " + publishError + "; " + rollbackError.message());
    return fail(error, "could not publish soft-body asset: " + publishError);
}

template<class Record>
bool read_count(std::istream& stream, std::string_view expected, std::size_t maximum,
                std::vector<Record>& output, std::string& error) {
    std::string token;
    std::size_t count{};
    if (!(stream >> token >> count) || token != expected || count > maximum) {
        error = "invalid soft-body " + std::string(expected) + " count";
        return false;
    }
    output.resize(count);
    return true;
}

} // namespace

bool write_dvesoft(const std::filesystem::path& path, const SoftBodyAsset& asset,
                   std::string* error) {
    if (!asset.validate(error)) return false;
    const std::uint64_t hash = soft_body_asset_content_hash(asset);
    if (asset.contentHash != 0U && asset.contentHash != hash)
        return fail(error, "soft-body asset content hash is stale");
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "DVE_SOFT_BODY 1\n";
    stream << "name " << std::quoted(asset.name) << "\n";
    stream << "settings " << static_cast<unsigned>(asset.kind) << ' '
           << static_cast<unsigned>(asset.bendModel) << ' ' << (asset.doubleSided ? 1 : 0)
           << ' ' << asset.linearDamping << ' ' << asset.pressure << "\n";
    stream << "vertices " << asset.vertices.size() << "\n";
    for (const SoftBodyVertex& vertex : asset.vertices)
        stream << "v " << vertex.position.x << ' ' << vertex.position.y << ' '
               << vertex.position.z << ' ' << vertex.inverseMass << ' ' << vertex.radius << "\n";
    stream << "faces " << asset.faces.size() << "\n";
    for (const SoftBodyFace& face : asset.faces)
        stream << "f " << face.vertices[0] << ' ' << face.vertices[1] << ' ' << face.vertices[2] << "\n";
    const auto distances = [&](std::string_view name,
                               const std::vector<SoftBodyDistanceConstraint>& constraints) {
        stream << name << ' ' << constraints.size() << "\n";
        for (const auto& constraint : constraints)
            stream << "d " << constraint.vertexA << ' ' << constraint.vertexB << ' '
                   << constraint.restLength << ' ' << constraint.compliance << "\n";
    };
    distances("stretch", asset.stretchConstraints);
    distances("bend", asset.bendConstraints);
    stream << "dihedral " << asset.dihedralConstraints.size() << "\n";
    for (const auto& constraint : asset.dihedralConstraints)
        stream << "h " << constraint.vertices[0] << ' ' << constraint.vertices[1] << ' '
               << constraint.vertices[2] << ' ' << constraint.vertices[3] << ' '
               << constraint.restAngle << ' ' << constraint.compliance << "\n";
    stream << "volume " << asset.volumeConstraints.size() << "\n";
    for (const auto& constraint : asset.volumeConstraints)
        stream << "t " << constraint.vertices[0] << ' ' << constraint.vertices[1] << ' '
               << constraint.vertices[2] << ' ' << constraint.vertices[3] << ' '
               << constraint.sixRestVolume << ' ' << constraint.compliance << "\n";
    stream << "hash " << hash << "\n";
    return write_atomic(path, stream.str(), error);
}

SoftBodyAssetReadResult read_dvesoft(const std::filesystem::path& path,
                                     std::uint64_t maximumBytes) {
    SoftBodyAssetReadResult result;
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > maximumBytes) {
        result.error = ec ? "could not stat soft-body asset" : "soft-body asset exceeds byte limit";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    std::string token, magic;
    unsigned version{};
    if (!stream || !(stream >> magic >> version) || magic != "DVE_SOFT_BODY" || version != 1U ||
        !(stream >> token) || token != "name" || !(stream >> std::quoted(result.asset.name))) {
        result.error = "unsupported or invalid soft-body asset header";
        return result;
    }
    unsigned kind{}, bend{}, doubleSided{};
    if (!(stream >> token >> kind >> bend >> doubleSided >> result.asset.linearDamping >> result.asset.pressure) ||
        token != "settings" || kind > static_cast<unsigned>(SoftBodyKind::DeformableProp) ||
        bend > static_cast<unsigned>(SoftBodyBendModel::DihedralShell) || doubleSided > 1U) {
        result.error = "invalid soft-body settings record";
        return result;
    }
    result.asset.kind = static_cast<SoftBodyKind>(kind);
    result.asset.bendModel = static_cast<SoftBodyBendModel>(bend);
    result.asset.doubleSided = doubleSided != 0U;
    if (!read_count(stream, "vertices", 1'000'000U, result.asset.vertices, result.error)) return result;
    for (SoftBodyVertex& vertex : result.asset.vertices)
        if (!(stream >> token >> vertex.position.x >> vertex.position.y >> vertex.position.z >>
              vertex.inverseMass >> vertex.radius) || token != "v") {
            result.error = "invalid soft-body vertex record"; return result;
        }
    if (!read_count(stream, "faces", 2'000'000U, result.asset.faces, result.error)) return result;
    for (SoftBodyFace& face : result.asset.faces)
        if (!(stream >> token >> face.vertices[0] >> face.vertices[1] >> face.vertices[2]) || token != "f") {
            result.error = "invalid soft-body face record"; return result;
        }
    const auto distances = [&](std::string_view name,
                               std::vector<SoftBodyDistanceConstraint>& constraints) {
        if (!read_count(stream, name, 4'000'000U, constraints, result.error)) return false;
        for (auto& constraint : constraints)
            if (!(stream >> token >> constraint.vertexA >> constraint.vertexB >> constraint.restLength >>
                  constraint.compliance) || token != "d") {
                result.error = "invalid soft-body distance record"; return false;
            }
        return true;
    };
    if (!distances("stretch", result.asset.stretchConstraints) ||
        !distances("bend", result.asset.bendConstraints)) return result;
    if (!read_count(stream, "dihedral", 2'000'000U, result.asset.dihedralConstraints, result.error)) return result;
    for (auto& constraint : result.asset.dihedralConstraints)
        if (!(stream >> token >> constraint.vertices[0] >> constraint.vertices[1] >>
              constraint.vertices[2] >> constraint.vertices[3] >> constraint.restAngle >>
              constraint.compliance) || token != "h") {
            result.error = "invalid soft-body dihedral record"; return result;
        }
    if (!read_count(stream, "volume", 4'000'000U, result.asset.volumeConstraints, result.error)) return result;
    for (auto& constraint : result.asset.volumeConstraints)
        if (!(stream >> token >> constraint.vertices[0] >> constraint.vertices[1] >>
              constraint.vertices[2] >> constraint.vertices[3] >> constraint.sixRestVolume >>
              constraint.compliance) || token != "t") {
            result.error = "invalid soft-body volume record"; return result;
        }
    if (!(stream >> token >> result.asset.contentHash) || token != "hash") {
        result.error = "missing soft-body asset hash"; return result;
    }
    stream >> std::ws;
    if (!stream.eof()) { result.error = "trailing soft-body asset data"; return result; }
    if (!result.asset.validate(&result.error)) return result;
    if (result.asset.contentHash != soft_body_asset_content_hash(result.asset))
        result.error = "soft-body asset hash mismatch";
    return result;
}

} // namespace dve
