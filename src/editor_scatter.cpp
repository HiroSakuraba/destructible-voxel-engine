#include "dve/editor_scatter.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "dve/editor_viewport.hpp"
#include "dve/transform.hpp"

namespace dve::editor {
namespace {

// splitmix64: small, fast and identical on every platform, so a seed reproduces a layout.
class ScatterRandom {
public:
    explicit ScatterRandom(std::uint64_t seed) noexcept : state_(seed ^ 0x5CA77E2D0F1A3B9DULL) {}
    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    // Uniform in [0, 1).
    float unit() noexcept { return static_cast<float>(next() >> 40U) / static_cast<float>(1ULL << 24U); }
    std::size_t index(std::size_t count) noexcept { return count == 0 ? 0 : static_cast<std::size_t>(next() % count); }

private:
    std::uint64_t state_;
};

Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Rotation taking +Y onto `normal`.
Quaternion rotation_from_up(Float3 normal) noexcept {
    const Float3 up{0.0F, 1.0F, 0.0F};
    const Float3 n = normalize(normal);
    const float cosine = std::clamp(dot(up, n), -1.0F, 1.0F);
    if (cosine > 0.9999F) return {};
    if (cosine < -0.9999F) return quaternion_from_axis_angle({1.0F, 0.0F, 0.0F}, 3.14159265F);
    return quaternion_from_axis_angle(normalize(cross(up, n)), std::acos(cosine));
}

EditorObjectBounds subtree_bounds(const EditorDocument& document, const std::vector<EditorObjectId>& ids) {
    EditorObjectBounds result;
    for (const EditorObjectId id : ids) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        EditorObjectBounds bounds = object_world_bounds(*object);
        if (!bounds.valid) bounds = {object->transform.position, object->transform.position, true};
        if (!result.valid) { result = bounds; continue; }
        result.minimum = {std::min(result.minimum.x, bounds.minimum.x), std::min(result.minimum.y, bounds.minimum.y),
                          std::min(result.minimum.z, bounds.minimum.z)};
        result.maximum = {std::max(result.maximum.x, bounds.maximum.x), std::max(result.maximum.y, bounds.maximum.y),
                          std::max(result.maximum.z, bounds.maximum.z)};
    }
    return result;
}

} // namespace

ScatterPlan plan_scatter(const EditorDocument& document, EditorObjectId target, std::size_t sourceCount,
                         const ScatterSettings& settings) {
    ScatterPlan plan;
    const EditorObject* surface = document.find_object(target);
    if (!surface) { plan.error = "The target surface no longer exists"; return plan; }
    if (!surface->voxels || surface->voxels->occupied_voxel_count() == 0 || surface->text3d || surface->gaborVolume) {
        plan.error = "The target must be a voxel object with voxels to scatter onto";
        return plan;
    }
    if (sourceCount == 0) { plan.error = "Nothing to scatter: select objects or a prefab asset"; return plan; }
    const EditorObjectBounds bounds = object_world_bounds(*surface);
    if (!bounds.valid) { plan.error = "The target has no bounds"; return plan; }

    const std::uint32_t count = std::min(settings.count, kMaxScatterCount);
    const float spacing = std::clamp(settings.minSpacingMeters, kMinScatterSpacingMeters, kMaxScatterSpacingMeters);
    const float spacingSquared = spacing * spacing;
    const float rayStartY = bounds.maximum.y + 1.0F;
    const float rayLength = bounds.maximum.y - bounds.minimum.y + 2.0F;
    // Only the target counts as ground: other objects on it (including the sources) are looked
    // through, not stood on.
    const auto drop = [&](float x, float z) -> std::optional<EditorPickResult> {
        const ViewportRay down{{x, rayStartY, z}, {0.0F, -1.0F, 0.0F}};
        for (const EditorPickResult& hit : pick_editor_document_all(document, down, rayLength))
            if (hit.objectId == target) return hit;
        return std::nullopt;
    };
    // A voxel face normal is axis-aligned, so on stepped voxel terrain it is almost always
    // straight up. Use the slope of the surface around the spot instead (heights two voxels
    // away on each side), falling back to the face normal at an edge.
    const float probe = std::max(0.05F, surface->voxelSizeMeters * 2.0F);
    const auto surface_normal = [&](const EditorPickResult& hit) {
        const Float3 face = length_squared(hit.worldNormal) > 1.0e-6F ? normalize(hit.worldNormal) : Float3{0, 1, 0};
        const auto east = drop(hit.worldPosition.x + probe, hit.worldPosition.z);
        const auto west = drop(hit.worldPosition.x - probe, hit.worldPosition.z);
        const auto north = drop(hit.worldPosition.x, hit.worldPosition.z + probe);
        const auto south = drop(hit.worldPosition.x, hit.worldPosition.z - probe);
        if (!east || !west || !north || !south) return face;
        const float slopeX = (east->worldPosition.y - west->worldPosition.y) / (2.0F * probe);
        const float slopeZ = (north->worldPosition.y - south->worldPosition.y) / (2.0F * probe);
        return normalize(Float3{-slopeX, 1.0F, -slopeZ});
    };
    ScatterRandom random(settings.seed);
    const std::uint32_t maximumAttempts = std::max<std::uint32_t>(1U, count) * kScatterAttemptsPerCopy;
    while (plan.samples.size() < count && plan.attempts < maximumAttempts) {
        ++plan.attempts;
        // Draw both coordinates and the source every attempt so the sequence of draws does not
        // depend on which candidates were rejected.
        const float x = bounds.minimum.x + random.unit() * (bounds.maximum.x - bounds.minimum.x);
        const float z = bounds.minimum.z + random.unit() * (bounds.maximum.z - bounds.minimum.z);
        const std::size_t source = random.index(sourceCount);
        const auto found = drop(x, z);
        if (!found) { ++plan.missedSurface; continue; }
        const EditorPickResult& hit = *found;
        const bool crowded = std::any_of(plan.samples.begin(), plan.samples.end(), [&](const ScatterSample& kept) {
            const float dx = kept.position.x - hit.worldPosition.x;
            const float dz = kept.position.z - hit.worldPosition.z;
            return dx * dx + dz * dz < spacingSquared;
        });
        if (crowded) { ++plan.tooClose; continue; }
        plan.samples.push_back({hit.worldPosition, surface_normal(hit), source});
    }
    return plan;
}

ScatterBuildResult build_scatter_command(EditorDocument& document, EditorObjectId target, const ScatterPlan& plan,
                                         const std::vector<ScatterSource>& sources, const ScatterSettings& settings) {
    ScatterBuildResult result;
    if (!plan.error.empty()) { result.error = plan.error; return result; }
    if (plan.samples.empty()) { result.error = "No spots found on the target surface"; return result; }
    const EditorObject* surface = document.find_object(target);
    if (!surface) { result.error = "The target surface no longer exists"; return result; }

    struct PreparedSource {
        const ScatterSource* source{};
        std::vector<EditorObjectId> closure;
        Float3 base{};
    };
    std::vector<PreparedSource> prepared;
    for (const ScatterSource& source : sources) {
        if (!source.document) { result.error = "A scatter source has no document"; return result; }
        PreparedSource entry{&source, collect_editor_object_subtree_ids(*source.document, source.roots), {}};
        if (entry.closure.empty()) { result.error = "Scatter source '" + source.label + "' no longer exists"; return result; }
        const EditorObjectBounds bounds = subtree_bounds(*source.document, entry.closure);
        entry.base = {(bounds.minimum.x + bounds.maximum.x) * 0.5F, bounds.minimum.y,
                      (bounds.minimum.z + bounds.maximum.z) * 0.5F};
        prepared.push_back(std::move(entry));
    }

    result.groupId = document.allocate_object_id();
    result.command = std::make_unique<CompoundCommand>("Scatter objects");
    EditorObject group(result.groupId, "Scatter (" + std::to_string(plan.samples.size()) + ")");
    const EditorObjectBounds targetBounds = object_world_bounds(*surface);
    group.transform = make_rigid_transform({(targetBounds.minimum.x + targetBounds.maximum.x) * 0.5F,
                                            targetBounds.maximum.y,
                                            (targetBounds.minimum.z + targetBounds.maximum.z) * 0.5F}, {});
    result.command->add(std::make_unique<AddObjectCommand>(std::move(group), "Scatter group"));

    for (const ScatterSample& sample : plan.samples) {
        if (sample.source >= prepared.size()) { result.error = "Scatter plan refers to a missing source"; return result; }
        const PreparedSource& source = prepared[sample.source];
        const Quaternion align = settings.alignToSurface ? rotation_from_up(sample.normal) : Quaternion{};
        std::map<EditorObjectId, EditorObjectId> idMap;
        for (const EditorObjectId oldId : source.closure) idMap.emplace(oldId, document.allocate_object_id());
        const std::set<EditorObjectId> roots(source.source->roots.begin(), source.source->roots.end());
        for (const EditorObjectId oldId : source.closure) {
            const EditorObject* original = source.source->document->find_object(oldId);
            if (!original) { result.error = "Scatter source changed while building"; return result; }
            EditorObject copy = clone_editor_object(*original);
            copy.id = idMap.at(oldId);
            copy.prefabLink.reset();  // independent copies
            if (roots.contains(oldId) || !original->parent || !idMap.contains(*original->parent)) {
                copy.parent = result.groupId;
                copy.attachment.reset();
            } else {
                copy.parent = idMap.at(*original->parent);
            }
            const Float3 offset = subtract(original->transform.position, source.base);
            copy.transform.position = add(sample.position, rotate(align, offset));
            copy.transform.rotation = normalize(multiply(align, original->transform.rotation));
            result.command->add(std::make_unique<AddObjectCommand>(std::move(copy), "Scatter copy"));
            ++result.objectCount;
        }
    }
    return result;
}

} // namespace dve::editor
