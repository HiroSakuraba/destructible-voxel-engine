#include "dve/soft_body.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace dve {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] float six_volume(Float3 a, Float3 b, Float3 c, Float3 d) noexcept {
    return dot(subtract(b, a), cross(subtract(c, a), subtract(d, a)));
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= prime;
    }
}

template <class T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(T));
}

[[nodiscard]] std::uint32_t grid_index(std::uint32_t x, std::uint32_t y,
                                       std::uint32_t width) noexcept {
    return y * width + x;
}

void add_distance(std::vector<SoftBodyDistanceConstraint>& constraints,
                  const std::vector<SoftBodyVertex>& vertices,
                  std::uint32_t a, std::uint32_t b, float compliance) {
    constraints.push_back({a, b,
                           length(subtract(vertices[a].position, vertices[b].position)),
                           compliance});
}

[[nodiscard]] float dihedral_angle(Float3 p0, Float3 p1, Float3 p2, Float3 p3) noexcept {
    const Float3 edge = subtract(p1, p0);
    const float edgeLength = length(edge);
    if (!(edgeLength > 1.0e-8F)) return 0.0F;
    const Float3 edgeDirection = multiply(edge, 1.0F / edgeLength);
    const Float3 n0raw = cross(subtract(p1, p0), subtract(p2, p0));
    const Float3 n1raw = cross(subtract(p3, p0), subtract(p1, p0));
    const float n0Length = length(n0raw);
    const float n1Length = length(n1raw);
    if (!(n0Length > 1.0e-8F) || !(n1Length > 1.0e-8F)) return 0.0F;
    const Float3 n0 = multiply(n0raw, 1.0F / n0Length);
    const Float3 n1 = multiply(n1raw, 1.0F / n1Length);
    return std::atan2(dot(edgeDirection, cross(n0, n1)),
                      std::clamp(dot(n0, n1), -1.0F, 1.0F));
}

void build_dihedral_constraints(SoftBodyAsset& asset, float compliance) {
    struct EdgeUse { std::uint32_t opposite{}; };
    std::map<std::pair<std::uint32_t, std::uint32_t>, EdgeUse> firstUse;
    for (const auto& face : asset.faces) {
        for (std::uint32_t local = 0U; local < 3U; ++local) {
            std::uint32_t a = face.vertices[local];
            std::uint32_t b = face.vertices[(local + 1U) % 3U];
            const std::uint32_t opposite = face.vertices[(local + 2U) % 3U];
            if (a > b) std::swap(a, b);
            const auto key = std::make_pair(a, b);
            const auto found = firstUse.find(key);
            if (found == firstUse.end()) {
                firstUse.emplace(key, EdgeUse{opposite});
            } else {
                const auto firstOpposite = found->second.opposite;
                const float rest = dihedral_angle(asset.vertices[a].position,
                                                  asset.vertices[b].position,
                                                  asset.vertices[firstOpposite].position,
                                                  asset.vertices[opposite].position);
                asset.dihedralConstraints.push_back({{a, b, firstOpposite, opposite},
                                                     rest, compliance});
                firstUse.erase(found);
            }
        }
    }
}

void solve_dihedral(const SoftBodyAsset& asset, RuntimeSoftBodyState& state,
                    const SoftBodyDihedralConstraint& constraint, float& lambda,
                    float deltaSeconds) noexcept {
    const auto ids = constraint.vertices;
    const std::array<Float3, 4> base{state.positions[ids[0]], state.positions[ids[1]],
                                     state.positions[ids[2]], state.positions[ids[3]]};
    const float angle = dihedral_angle(base[0], base[1], base[2], base[3]);
    if (!std::isfinite(angle)) return;
    std::array<Float3, 4> gradients{};
    const float edgeLength = std::max(1.0e-4F, length(subtract(base[1], base[0])));
    const float epsilon = edgeLength * 1.0e-4F;
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            auto plus = base;
            auto minus = base;
            float* plusComponent = axis == 0U ? &plus[vertex].x :
                                   axis == 1U ? &plus[vertex].y : &plus[vertex].z;
            float* minusComponent = axis == 0U ? &minus[vertex].x :
                                    axis == 1U ? &minus[vertex].y : &minus[vertex].z;
            *plusComponent += epsilon;
            *minusComponent -= epsilon;
            float derivative = (dihedral_angle(plus[0], plus[1], plus[2], plus[3]) -
                                dihedral_angle(minus[0], minus[1], minus[2], minus[3])) /
                               (2.0F * epsilon);
            float* target = axis == 0U ? &gradients[vertex].x :
                            axis == 1U ? &gradients[vertex].y : &gradients[vertex].z;
            *target = derivative;
        }
    }
    float denominator{};
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex)
        denominator += asset.vertices[ids[vertex]].inverseMass *
                       length_squared(gradients[vertex]);
    const float alpha = constraint.compliance / (deltaSeconds * deltaSeconds);
    denominator += alpha;
    if (!(denominator > 1.0e-12F) || !std::isfinite(denominator)) return;
    float difference = angle - constraint.restAngle;
    while (difference > 3.14159265358979323846F) difference -= 6.28318530717958647692F;
    while (difference < -3.14159265358979323846F) difference += 6.28318530717958647692F;
    const float deltaLambda = (-difference - alpha * lambda) / denominator;
    lambda += deltaLambda;
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex)
        state.positions[ids[vertex]] = add(state.positions[ids[vertex]],
            multiply(gradients[vertex],
                     asset.vertices[ids[vertex]].inverseMass * deltaLambda));
}

[[nodiscard]] RuntimeSoftBodyState make_runtime_state(const SoftBodyAsset& asset,
                                                       const RigidTransform& transform) {
    RuntimeSoftBodyState state;
    state.positions.reserve(asset.vertices.size());
    state.previousPositions.reserve(asset.vertices.size());
    state.velocities.resize(asset.vertices.size());
    for (const auto& vertex : asset.vertices) {
        const Float3 position = transform_point(transform, vertex.position);
        state.positions.push_back(position);
        state.previousPositions.push_back(position);
    }
    state.stretchLambdas.resize(asset.stretchConstraints.size());
    state.bendLambdas.resize(asset.bendConstraints.size());
    state.dihedralLambdas.resize(asset.dihedralConstraints.size());
    state.volumeLambdas.resize(asset.volumeConstraints.size());
    state.contactLambdas.resize(asset.vertices.size());
    return state;
}

void solve_distance(const SoftBodyAsset& asset, RuntimeSoftBodyState& state,
                    const SoftBodyDistanceConstraint& constraint, float& lambda,
                    float deltaSeconds) noexcept {
    const float inverseMassA = asset.vertices[constraint.vertexA].inverseMass;
    const float inverseMassB = asset.vertices[constraint.vertexB].inverseMass;
    const float inverseMassSum = inverseMassA + inverseMassB;
    if (!(inverseMassSum > 0.0F)) return;
    const Float3 delta = subtract(state.positions[constraint.vertexA],
                                  state.positions[constraint.vertexB]);
    const float distance = length(delta);
    if (!(distance > 1.0e-8F) || !std::isfinite(distance)) return;
    const Float3 gradient = multiply(delta, 1.0F / distance);
    const float constraintValue = distance - constraint.restLength;
    const float alpha = constraint.compliance / (deltaSeconds * deltaSeconds);
    const float deltaLambda = (-constraintValue - alpha * lambda) /
                              (inverseMassSum + alpha);
    lambda += deltaLambda;
    state.positions[constraint.vertexA] = add(
        state.positions[constraint.vertexA], multiply(gradient, inverseMassA * deltaLambda));
    state.positions[constraint.vertexB] = subtract(
        state.positions[constraint.vertexB], multiply(gradient, inverseMassB * deltaLambda));
}

void solve_volume(const SoftBodyAsset& asset, RuntimeSoftBodyState& state,
                  const SoftBodyVolumeConstraint& constraint, float& lambda,
                  float deltaSeconds) noexcept {
    const auto& index = constraint.vertices;
    const Float3 p0 = state.positions[index[0]];
    const Float3 p1 = state.positions[index[1]];
    const Float3 p2 = state.positions[index[2]];
    const Float3 p3 = state.positions[index[3]];
    const std::array<Float3, 4> gradient{
        multiply(cross(subtract(p2, p1), subtract(p3, p1)), -1.0F),
        cross(subtract(p2, p0), subtract(p3, p0)),
        multiply(cross(subtract(p1, p0), subtract(p3, p0)), -1.0F),
        cross(subtract(p1, p0), subtract(p2, p0))};
    float denominator{};
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        denominator += asset.vertices[index[vertex]].inverseMass *
                       length_squared(gradient[vertex]);
    }
    const float alpha = constraint.compliance / (deltaSeconds * deltaSeconds);
    denominator += alpha;
    if (!(denominator > 1.0e-12F) || !std::isfinite(denominator)) return;
    const float constraintValue = six_volume(p0, p1, p2, p3) - constraint.sixRestVolume;
    const float deltaLambda = (-constraintValue - alpha * lambda) / denominator;
    lambda += deltaLambda;
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        state.positions[index[vertex]] = add(
            state.positions[index[vertex]],
            multiply(gradient[vertex], asset.vertices[index[vertex]].inverseMass * deltaLambda));
    }
}

} // namespace

bool SoftBodyAsset::validate(std::string* error) const {
    if (name.empty() || name.size() > 255U ||
        static_cast<unsigned>(kind) > static_cast<unsigned>(SoftBodyKind::DeformableProp) ||
        static_cast<unsigned>(bendModel) > static_cast<unsigned>(SoftBodyBendModel::DihedralShell)) {
        set_error(error, "soft body identity or kind is invalid");
        return false;
    }
    if (vertices.size() < 2U) {
        set_error(error, "soft body requires at least two vertices");
        return false;
    }
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
            !std::isfinite(vertex.position.z) || !std::isfinite(vertex.inverseMass) ||
            vertex.inverseMass < 0.0F || !std::isfinite(vertex.radius) || vertex.radius < 0.0F) {
            set_error(error, "soft body contains an invalid vertex");
            return false;
        }
    }
    auto validIndex = [&](std::uint32_t index) { return index < vertices.size(); };
    for (const auto& face : faces) {
        if (!validIndex(face.vertices[0]) || !validIndex(face.vertices[1]) ||
            !validIndex(face.vertices[2]) || face.vertices[0] == face.vertices[1] ||
            face.vertices[0] == face.vertices[2] || face.vertices[1] == face.vertices[2]) {
            set_error(error, "soft body face references an invalid vertex");
            return false;
        }
    }
    auto validateDistance = [&](const SoftBodyDistanceConstraint& constraint) {
        return validIndex(constraint.vertexA) && validIndex(constraint.vertexB) &&
               constraint.vertexA != constraint.vertexB && constraint.restLength > 0.0F &&
               std::isfinite(constraint.restLength) && constraint.compliance >= 0.0F &&
               std::isfinite(constraint.compliance);
    };
    for (const auto& constraint : stretchConstraints) {
        if (!validateDistance(constraint)) {
            set_error(error, "soft body contains an invalid stretch constraint");
            return false;
        }
    }
    for (const auto& constraint : bendConstraints) {
        if (!validateDistance(constraint)) {
            set_error(error, "soft body contains an invalid bend constraint");
            return false;
        }
    }
    for (const auto& constraint : dihedralConstraints) {
        std::set<std::uint32_t> distinct;
        for (const std::uint32_t index : constraint.vertices) {
            if (!validIndex(index) || !distinct.insert(index).second) {
                set_error(error, "soft body dihedral constraint references an invalid vertex");
                return false;
            }
        }
        if (!std::isfinite(constraint.restAngle) || constraint.compliance < 0.0F ||
            !std::isfinite(constraint.compliance)) {
            set_error(error, "soft body contains an invalid dihedral constraint");
            return false;
        }
    }
    for (const auto& constraint : volumeConstraints) {
        std::set<std::uint32_t> distinct;
        for (const std::uint32_t index : constraint.vertices) {
            if (!validIndex(index) || !distinct.insert(index).second) {
                set_error(error, "soft body volume references an invalid vertex");
                return false;
            }
        }
        if (!std::isfinite(constraint.sixRestVolume) || constraint.compliance < 0.0F ||
            !std::isfinite(constraint.compliance)) {
            set_error(error, "soft body contains an invalid volume constraint");
            return false;
        }
    }
    if (!std::isfinite(linearDamping) || linearDamping < 0.0F || linearDamping > 1.0F ||
        !std::isfinite(pressure)) {
        set_error(error, "soft body damping or pressure is invalid");
        return false;
    }
    return true;
}

void SoftBodyAsset::recompute_hash() noexcept {
    contentHash = soft_body_asset_content_hash(*this);
}

std::uint64_t soft_body_asset_content_hash(const SoftBodyAsset& asset) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto count = [&](std::size_t value) { const std::uint64_t converted = value; hash_value(hash, converted); };
    count(asset.name.size()); hash_bytes(hash, asset.name.data(), asset.name.size());
    hash_value(hash, static_cast<std::uint8_t>(asset.kind));
    hash_value(hash, static_cast<std::uint8_t>(asset.bendModel));
    hash_value(hash, static_cast<std::uint8_t>(asset.doubleSided ? 1U : 0U));
    hash_value(hash, asset.linearDamping); hash_value(hash, asset.pressure);
    count(asset.vertices.size());
    for (const auto& vertex : asset.vertices) {
        hash_value(hash, vertex.position.x); hash_value(hash, vertex.position.y);
        hash_value(hash, vertex.position.z); hash_value(hash, vertex.inverseMass);
        hash_value(hash, vertex.radius);
    }
    count(asset.faces.size());
    for (const auto& face : asset.faces) for (std::uint32_t index : face.vertices) hash_value(hash, index);
    const auto distances = [&](const std::vector<SoftBodyDistanceConstraint>& constraints) {
        count(constraints.size());
        for (const auto& constraint : constraints) {
            hash_value(hash, constraint.vertexA); hash_value(hash, constraint.vertexB);
            hash_value(hash, constraint.restLength); hash_value(hash, constraint.compliance);
        }
    };
    distances(asset.stretchConstraints); distances(asset.bendConstraints);
    count(asset.dihedralConstraints.size());
    for (const auto& constraint : asset.dihedralConstraints) {
        for (std::uint32_t index : constraint.vertices) hash_value(hash, index);
        hash_value(hash, constraint.restAngle); hash_value(hash, constraint.compliance);
    }
    count(asset.volumeConstraints.size());
    for (const auto& constraint : asset.volumeConstraints) {
        for (std::uint32_t index : constraint.vertices) hash_value(hash, index);
        hash_value(hash, constraint.sixRestVolume); hash_value(hash, constraint.compliance);
    }
    return hash == 0U ? 1U : hash;
}

SoftBodyAsset make_soft_body_cloth(const SoftBodyClothRecipe& recipe) {
    SoftBodyAsset asset;
    asset.name = "Cloth";
    asset.kind = SoftBodyKind::Cloth;
    asset.bendModel = SoftBodyBendModel::Distance;
    const std::uint32_t columns = std::max(2U, recipe.columns);
    const std::uint32_t rows = std::max(2U, recipe.rows);
    asset.vertices.reserve(static_cast<std::size_t>(columns) * rows);
    const float inverseMass = recipe.vertexMass > 0.0F ? 1.0F / recipe.vertexMass : 0.0F;
    for (std::uint32_t y = 0U; y < rows; ++y) {
        for (std::uint32_t x = 0U; x < columns; ++x) {
            const bool pinned = recipe.pinTopEdge && y == 0U;
            asset.vertices.push_back({
                {(static_cast<float>(x) - static_cast<float>(columns - 1U) * 0.5F) * recipe.spacing,
                 -static_cast<float>(y) * recipe.spacing, 0.0F},
                pinned ? 0.0F : inverseMass,
                recipe.spacing * 0.1F});
        }
    }
    for (std::uint32_t y = 0U; y + 1U < rows; ++y) {
        for (std::uint32_t x = 0U; x + 1U < columns; ++x) {
            const std::uint32_t a = grid_index(x, y, columns);
            const std::uint32_t b = grid_index(x + 1U, y, columns);
            const std::uint32_t c = grid_index(x, y + 1U, columns);
            const std::uint32_t d = grid_index(x + 1U, y + 1U, columns);
            asset.faces.push_back({{a, c, b}});
            asset.faces.push_back({{b, c, d}});
        }
    }
    for (std::uint32_t y = 0U; y < rows; ++y) {
        for (std::uint32_t x = 0U; x < columns; ++x) {
            const std::uint32_t a = grid_index(x, y, columns);
            if (x + 1U < columns)
                add_distance(asset.stretchConstraints, asset.vertices, a,
                             grid_index(x + 1U, y, columns), recipe.stretchCompliance);
            if (y + 1U < rows)
                add_distance(asset.stretchConstraints, asset.vertices, a,
                             grid_index(x, y + 1U, columns), recipe.stretchCompliance);
            if (x + 1U < columns && y + 1U < rows)
                add_distance(asset.stretchConstraints, asset.vertices, a,
                             grid_index(x + 1U, y + 1U, columns), recipe.stretchCompliance);
            if (x + 2U < columns)
                add_distance(asset.bendConstraints, asset.vertices, a,
                             grid_index(x + 2U, y, columns), recipe.bendCompliance);
            if (y + 2U < rows)
                add_distance(asset.bendConstraints, asset.vertices, a,
                             grid_index(x, y + 2U, columns), recipe.bendCompliance);
        }
    }
    build_dihedral_constraints(asset, recipe.bendCompliance);
    if (!asset.dihedralConstraints.empty()) asset.bendModel = SoftBodyBendModel::DihedralShell;
    asset.recompute_hash();
    return asset;
}

SoftBodyAsset make_soft_body_rope(const SoftBodyRopeRecipe& recipe) {
    SoftBodyAsset asset;
    asset.name = "Rope";
    asset.kind = SoftBodyKind::Rope;
    asset.bendModel = SoftBodyBendModel::Distance;
    const std::uint32_t segments = std::max(1U, recipe.segments);
    const float inverseMass = recipe.vertexMass > 0.0F ? 1.0F / recipe.vertexMass : 0.0F;
    asset.vertices.reserve(static_cast<std::size_t>(segments) + 1U);
    for (std::uint32_t index = 0U; index <= segments; ++index) {
        asset.vertices.push_back({{0.0F, -static_cast<float>(index) * recipe.spacing, 0.0F},
                                  recipe.pinRoot && index == 0U ? 0.0F : inverseMass,
                                  recipe.spacing * 0.15F});
        if (index > 0U)
            add_distance(asset.stretchConstraints, asset.vertices, index - 1U, index,
                         recipe.stretchCompliance);
        if (index > 1U)
            add_distance(asset.bendConstraints, asset.vertices, index - 2U, index,
                         recipe.bendCompliance);
    }
    asset.recompute_hash();
    return asset;
}

SoftBodyAsset make_soft_body_vegetation(const SoftBodyRopeRecipe& recipe) {
    SoftBodyAsset asset = make_soft_body_rope(recipe);
    asset.name = "Vegetation Stem";
    asset.kind = SoftBodyKind::Vegetation;
    asset.bendModel = SoftBodyBendModel::CosseratRod;
    asset.linearDamping = 0.08F;
    asset.recompute_hash();
    return asset;
}

SoftBodyAsset make_soft_body_deformable_box(const SoftBodyDeformableBoxRecipe& recipe) {
    SoftBodyAsset asset;
    asset.name = "Deformable Box";
    asset.kind = SoftBodyKind::DeformableProp;
    asset.bendModel = SoftBodyBendModel::Distance;
    const std::uint32_t nx = std::max(1U, recipe.cells[0]);
    const std::uint32_t ny = std::max(1U, recipe.cells[1]);
    const std::uint32_t nz = std::max(1U, recipe.cells[2]);
    const std::uint32_t vx = nx + 1U;
    const std::uint32_t vy = ny + 1U;
    const float inverseMass = recipe.vertexMass > 0.0F ? 1.0F / recipe.vertexMass : 0.0F;
    auto index = [vx, vy](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (z * vy + y) * vx + x;
    };
    asset.vertices.reserve(static_cast<std::size_t>(vx) * (ny + 1U) * (nz + 1U));
    for (std::uint32_t z = 0U; z <= nz; ++z)
        for (std::uint32_t y = 0U; y <= ny; ++y)
            for (std::uint32_t x = 0U; x <= nx; ++x)
                asset.vertices.push_back({
                    {(static_cast<float>(x) - static_cast<float>(nx) * 0.5F) * recipe.spacing,
                     static_cast<float>(y) * recipe.spacing,
                     (static_cast<float>(z) - static_cast<float>(nz) * 0.5F) * recipe.spacing},
                    inverseMass, recipe.spacing * 0.1F});
    std::set<std::pair<std::uint32_t, std::uint32_t>> uniqueEdges;
    auto edge = [&](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        if (uniqueEdges.emplace(a, b).second)
            add_distance(asset.stretchConstraints, asset.vertices, a, b,
                         recipe.stretchCompliance);
    };
    for (std::uint32_t z = 0U; z < nz; ++z) {
        for (std::uint32_t y = 0U; y < ny; ++y) {
            for (std::uint32_t x = 0U; x < nx; ++x) {
                const std::array<std::uint32_t, 8> v{
                    index(x, y, z), index(x + 1U, y, z), index(x, y + 1U, z),
                    index(x + 1U, y + 1U, z), index(x, y, z + 1U),
                    index(x + 1U, y, z + 1U), index(x, y + 1U, z + 1U),
                    index(x + 1U, y + 1U, z + 1U)};
                constexpr std::array<std::array<std::uint32_t, 4>, 5> tetraLocal{{
                    {{0U, 1U, 2U, 4U}}, {{1U, 3U, 2U, 7U}}, {{1U, 2U, 4U, 7U}},
                    {{1U, 4U, 5U, 7U}}, {{2U, 4U, 7U, 6U}}}};
                for (const auto& local : tetraLocal) {
                    const std::array<std::uint32_t, 4> tetra{
                        v[local[0]], v[local[1]], v[local[2]], v[local[3]]};
                    asset.volumeConstraints.push_back({tetra,
                        six_volume(asset.vertices[tetra[0]].position,
                                   asset.vertices[tetra[1]].position,
                                   asset.vertices[tetra[2]].position,
                                   asset.vertices[tetra[3]].position),
                        recipe.volumeCompliance});
                    for (std::size_t a = 0U; a < 4U; ++a)
                        for (std::size_t b = a + 1U; b < 4U; ++b)
                            edge(tetra[a], tetra[b]);
                }
            }
        }
    }
    const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        asset.faces.push_back({{a, b, c}});
        asset.faces.push_back({{a, c, d}});
    };
    for (std::uint32_t y = 0U; y < ny; ++y) for (std::uint32_t x = 0U; x < nx; ++x) {
        quad(index(x, y, 0U), index(x, y + 1U, 0U), index(x + 1U, y + 1U, 0U), index(x + 1U, y, 0U));
        quad(index(x, y, nz), index(x + 1U, y, nz), index(x + 1U, y + 1U, nz), index(x, y + 1U, nz));
    }
    for (std::uint32_t z = 0U; z < nz; ++z) for (std::uint32_t x = 0U; x < nx; ++x) {
        quad(index(x, 0U, z), index(x + 1U, 0U, z), index(x + 1U, 0U, z + 1U), index(x, 0U, z + 1U));
        quad(index(x, ny, z), index(x, ny, z + 1U), index(x + 1U, ny, z + 1U), index(x + 1U, ny, z));
    }
    for (std::uint32_t z = 0U; z < nz; ++z) for (std::uint32_t y = 0U; y < ny; ++y) {
        quad(index(0U, y, z), index(0U, y, z + 1U), index(0U, y + 1U, z + 1U), index(0U, y + 1U, z));
        quad(index(nx, y, z), index(nx, y + 1U, z), index(nx, y + 1U, z + 1U), index(nx, y, z + 1U));
    }
    asset.doubleSided = false;
    asset.recompute_hash();
    return asset;
}

JoltSoftBodyRecipe make_jolt_soft_body_recipe(const SoftBodyAsset& asset) {
    JoltSoftBodyRecipe recipe;
    recipe.kind = asset.kind;
    recipe.bendModel = asset.bendModel;
    recipe.vertices = asset.vertices;
    recipe.faces = asset.faces;
    recipe.edges = asset.stretchConstraints;
    recipe.edges.insert(recipe.edges.end(), asset.bendConstraints.begin(),
                        asset.bendConstraints.end());
    recipe.dihedrals = asset.dihedralConstraints;
    recipe.volumes = asset.volumeConstraints;
    recipe.facesDoubleSided = asset.doubleSided;
    recipe.pressure = asset.pressure;
    recipe.linearDamping = asset.linearDamping;
    return recipe;
}

RuntimeSoftBodyId SoftBodyWorld::create(SoftBodyAsset asset,
                                        RuntimeSoftBodyInstance instance,
                                        std::string* error) {
    asset.recompute_hash();
    if (!asset.validate(error)) return 0U;
    const RuntimeSoftBodyId objectId = nextObjectId_++;
    instance.objectId = objectId;
    instance.assetId = asset.contentHash;
    instance.solverIterations = std::clamp(instance.solverIterations, 1U, 64U);
    instance.maximumSubsteps = std::clamp(instance.maximumSubsteps, 1U, 64U);
    instance.maximumDisplacementFraction = std::clamp(
        instance.maximumDisplacementFraction, 0.05F, 1.0F);
    instance.groundContactCompliance = std::max(0.0F, instance.groundContactCompliance);
    instance.groundFriction = std::clamp(instance.groundFriction, 0.0F, 1.0F);
    instance.contactMultiplierDecay = std::clamp(instance.contactMultiplierDecay, 0.0F, 1.0F);
    assets_.try_emplace(asset.contentHash, asset);
    entries_.emplace(objectId, Entry{instance, make_runtime_state(asset, instance.transform)});
    return objectId;
}

bool SoftBodyWorld::destroy(RuntimeSoftBodyId objectId) noexcept {
    return entries_.erase(objectId) != 0U;
}

bool SoftBodyWorld::play(RuntimeSoftBodyId objectId) noexcept {
    auto found = entries_.find(objectId);
    if (found == entries_.end()) return false;
    found->second.instance.running = true;
    return true;
}

bool SoftBodyWorld::pause(RuntimeSoftBodyId objectId) noexcept {
    auto found = entries_.find(objectId);
    if (found == entries_.end()) return false;
    found->second.instance.running = false;
    return true;
}

bool SoftBodyWorld::reset(RuntimeSoftBodyId objectId) noexcept {
    auto found = entries_.find(objectId);
    if (found == entries_.end()) return false;
    const auto asset = assets_.find(found->second.instance.assetId);
    if (asset == assets_.end()) return false;
    found->second.state = make_runtime_state(asset->second, found->second.instance.transform);
    return true;
}

bool SoftBodyWorld::apply_impulse(RuntimeSoftBodyId objectId, Float3 worldImpulse) noexcept {
    auto found = entries_.find(objectId);
    if (found == entries_.end() || !std::isfinite(worldImpulse.x) ||
        !std::isfinite(worldImpulse.y) || !std::isfinite(worldImpulse.z)) return false;
    const SoftBodyAsset& asset = assets_.at(found->second.instance.assetId);
    double totalMass{};
    for (const SoftBodyVertex& vertex : asset.vertices)
        if (vertex.inverseMass > 0.0F) totalMass += 1.0 / static_cast<double>(vertex.inverseMass);
    if (!(totalMass > 0.0)) return false;
    const Float3 deltaVelocity = multiply(worldImpulse, static_cast<float>(1.0 / totalMass));
    for (std::size_t index = 0U; index < asset.vertices.size(); ++index)
        if (asset.vertices[index].inverseMass > 0.0F)
            found->second.state.velocities[index] = add(found->second.state.velocities[index], deltaVelocity);
    return true;
}

bool SoftBodyWorld::apply_radial_impulse(
    RuntimeSoftBodyId objectId, Float3 worldCenter, float radius, float strength) noexcept {
    auto found = entries_.find(objectId);
    if (found == entries_.end() || !(radius > 0.0F) || !std::isfinite(radius) ||
        !std::isfinite(strength) || !std::isfinite(worldCenter.x) ||
        !std::isfinite(worldCenter.y) || !std::isfinite(worldCenter.z)) return false;
    const SoftBodyAsset& asset = assets_.at(found->second.instance.assetId);
    bool affected{};
    for (std::size_t index = 0U; index < asset.vertices.size(); ++index) {
        if (asset.vertices[index].inverseMass <= 0.0F) continue;
        const Float3 delta = subtract(found->second.state.positions[index], worldCenter);
        const float distance = length(delta);
        if (!(distance < radius)) continue;
        const Float3 direction = distance > 1.0e-6F ? multiply(delta, 1.0F / distance) : Float3{0.0F, 1.0F, 0.0F};
        const float falloff = 1.0F - distance / radius;
        found->second.state.velocities[index] = add(
            found->second.state.velocities[index], multiply(direction, strength * falloff));
        affected = true;
    }
    return affected;
}

bool SoftBodyWorld::set_vertex_target(
    RuntimeSoftBodyId objectId, std::uint32_t vertex, Float3 worldPosition,
    bool clearVelocity) noexcept {
    auto found = entries_.find(objectId);
    if (found == entries_.end() || !std::isfinite(worldPosition.x) ||
        !std::isfinite(worldPosition.y) || !std::isfinite(worldPosition.z)) return false;
    const SoftBodyAsset& asset = assets_.at(found->second.instance.assetId);
    if (vertex >= asset.vertices.size() || asset.vertices[vertex].inverseMass != 0.0F) return false;
    RuntimeSoftBodyState& state = found->second.state;
    state.positions[vertex] = worldPosition;
    state.previousPositions[vertex] = worldPosition;
    if (clearVelocity) state.velocities[vertex] = {};
    return true;
}

SoftBodyStepTelemetry SoftBodyWorld::step(float deltaSeconds, Float3 gravity) {
    SoftBodyStepTelemetry telemetry;
    if (!(deltaSeconds > 0.0F) || !std::isfinite(deltaSeconds)) return telemetry;
    for (auto& [objectId, entry] : entries_) {
        (void)objectId;
        if (!entry.instance.running) continue;
        const SoftBodyAsset& asset = assets_.at(entry.instance.assetId);
        RuntimeSoftBodyState& state = entry.state;
        float maximumSpeed{};
        for (const auto velocity : state.velocities)
            maximumSpeed = std::max(maximumSpeed, length(velocity));
        float characteristicLength = std::numeric_limits<float>::max();
        for (const auto& constraint : asset.stretchConstraints)
            characteristicLength = std::min(characteristicLength, constraint.restLength);
        if (!std::isfinite(characteristicLength)) {
            for (const auto& vertex : asset.vertices)
                if (vertex.radius > 0.0F)
                    characteristicLength = std::min(characteristicLength, vertex.radius * 2.0F);
        }
        if (!std::isfinite(characteristicLength)) characteristicLength = 0.1F;
        const auto plan = plan_simulation_step({deltaSeconds, maximumSpeed,
            characteristicLength, entry.instance.maximumDisplacementFraction,
            1.0F / 60.0F, entry.instance.maximumSubsteps});
        if (!plan.enabled) continue;
        telemetry.substeps += plan.substeps;
        const float dt = plan.substepSeconds;
        for (std::uint32_t substep = 0U; substep < plan.substeps; ++substep) {
            std::fill(state.stretchLambdas.begin(), state.stretchLambdas.end(), 0.0F);
            std::fill(state.bendLambdas.begin(), state.bendLambdas.end(), 0.0F);
            std::fill(state.dihedralLambdas.begin(), state.dihedralLambdas.end(), 0.0F);
            std::fill(state.volumeLambdas.begin(), state.volumeLambdas.end(), 0.0F);
            std::vector<Float3> pressureAcceleration(asset.vertices.size());
            if (asset.pressure != 0.0F) {
                for (const auto& face : asset.faces) {
                    const Float3 a = state.positions[face.vertices[0]];
                    const Float3 b = state.positions[face.vertices[1]];
                    const Float3 c = state.positions[face.vertices[2]];
                    const Float3 areaNormal = multiply(cross(subtract(b, a), subtract(c, a)),
                                                       asset.pressure / 6.0F);
                    for (const std::uint32_t vertex : face.vertices)
                        pressureAcceleration[vertex] = add(pressureAcceleration[vertex],
                                                           areaNormal);
                }
            }
            for (std::size_t vertex = 0U; vertex < asset.vertices.size(); ++vertex) {
                state.previousPositions[vertex] = state.positions[vertex];
                if (asset.vertices[vertex].inverseMass == 0.0F) continue;
                state.velocities[vertex] = add(state.velocities[vertex], multiply(gravity, dt));
                state.velocities[vertex] = add(state.velocities[vertex],
                    multiply(pressureAcceleration[vertex],
                             asset.vertices[vertex].inverseMass * dt));
                state.velocities[vertex] = multiply(state.velocities[vertex],
                    std::max(0.0F, 1.0F - asset.linearDamping));
                state.positions[vertex] = add(state.positions[vertex],
                                              multiply(state.velocities[vertex], dt));
                ++telemetry.verticesIntegrated;
            }
            for (std::uint32_t iteration = 0U;
                 iteration < entry.instance.solverIterations; ++iteration) {
                for (std::size_t constraint = 0U;
                     constraint < asset.stretchConstraints.size(); ++constraint) {
                    solve_distance(asset, state, asset.stretchConstraints[constraint],
                                   state.stretchLambdas[constraint], dt);
                    ++telemetry.distanceConstraintsSolved;
                }
                for (std::size_t constraint = 0U;
                     constraint < asset.bendConstraints.size(); ++constraint) {
                    solve_distance(asset, state, asset.bendConstraints[constraint],
                                   state.bendLambdas[constraint], dt);
                    ++telemetry.distanceConstraintsSolved;
                }
                for (std::size_t constraint = 0U;
                     constraint < asset.dihedralConstraints.size(); ++constraint) {
                    solve_dihedral(asset, state, asset.dihedralConstraints[constraint],
                                   state.dihedralLambdas[constraint], dt);
                    ++telemetry.dihedralConstraintsSolved;
                }
                for (std::size_t constraint = 0U;
                     constraint < asset.volumeConstraints.size(); ++constraint) {
                    solve_volume(asset, state, asset.volumeConstraints[constraint],
                                 state.volumeLambdas[constraint], dt);
                    ++telemetry.volumeConstraintsSolved;
                }
                if (entry.instance.collideWithGround) {
                    for (std::size_t vertex = 0U; vertex < asset.vertices.size(); ++vertex) {
                        const float inverseMass = asset.vertices[vertex].inverseMass;
                        if (inverseMass == 0.0F) continue;
                        const float minimum = entry.instance.groundHeight +
                                              asset.vertices[vertex].radius;
                        const float constraintValue = state.positions[vertex].y - minimum;
                        float& lambda = state.contactLambdas[vertex];
                        if (constraintValue < 0.0F || lambda > 0.0F) {
                            const float alpha = entry.instance.groundContactCompliance /
                                                (dt * dt);
                            const float candidate = lambda +
                                (-constraintValue - alpha * lambda) / (inverseMass + alpha);
                            const float nextLambda = std::max(0.0F, candidate);
                            const float applied = nextLambda - lambda;
                            lambda = nextLambda;
                            state.positions[vertex].y += inverseMass * applied;
                            if (applied != 0.0F) ++telemetry.contactMultipliersUpdated;
                            if (constraintValue < 0.0F) ++telemetry.groundContacts;
                        } else {
                            lambda *= std::clamp(entry.instance.contactMultiplierDecay,
                                                 0.0F, 1.0F);
                        }
                    }
                }
            }
            for (std::size_t vertex = 0U; vertex < asset.vertices.size(); ++vertex) {
                state.velocities[vertex] = multiply(
                    subtract(state.positions[vertex], state.previousPositions[vertex]),
                    1.0F / dt);
                if (state.contactLambdas[vertex] > 0.0F) {
                    const float friction = std::clamp(entry.instance.groundFriction, 0.0F, 1.0F);
                    state.velocities[vertex].x *= 1.0F - friction;
                    state.velocities[vertex].z *= 1.0F - friction;
                }
                if (!std::isfinite(state.positions[vertex].x) ||
                    !std::isfinite(state.positions[vertex].y) ||
                    !std::isfinite(state.positions[vertex].z)) {
                    state.positions[vertex] = state.previousPositions[vertex];
                    state.velocities[vertex] = {};
                    ++telemetry.nonFiniteCorrections;
                }
            }
            ++state.simulationFrame;
        }
        ++telemetry.bodiesStepped;
    }
    return telemetry;
}

const SoftBodyAsset* SoftBodyWorld::find_asset(std::uint64_t assetId) const noexcept {
    const auto found = assets_.find(assetId);
    return found == assets_.end() ? nullptr : &found->second;
}

const RuntimeSoftBodyInstance* SoftBodyWorld::find_instance(
    RuntimeSoftBodyId objectId) const noexcept {
    const auto found = entries_.find(objectId);
    return found == entries_.end() ? nullptr : &found->second.instance;
}

const RuntimeSoftBodyState* SoftBodyWorld::find_state(RuntimeSoftBodyId objectId) const noexcept {
    const auto found = entries_.find(objectId);
    return found == entries_.end() ? nullptr : &found->second.state;
}

std::string soft_body_kind_name(SoftBodyKind kind) {
    switch (kind) {
    case SoftBodyKind::Cloth: return "Cloth";
    case SoftBodyKind::Rope: return "Rope";
    case SoftBodyKind::Vegetation: return "Vegetation";
    case SoftBodyKind::DeformableProp: return "Deformable Prop";
    }
    return "Unknown";
}

} // namespace dve
