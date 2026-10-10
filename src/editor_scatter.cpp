#include "dve/editor_scatter.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
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

// Drops a vertical ray at (x, z) and returns the first hit on an accepted object.
std::optional<EditorPickResult> drop_onto(const EditorDocument& document, float x, float z, float top, float length,
                                          const std::function<bool(EditorObjectId)>& ground) {
    const ViewportRay down{{x, top, z}, {0.0F, -1.0F, 0.0F}};
    for (const EditorPickResult& hit : pick_editor_document_all(document, down, length))
        if (ground(hit.objectId)) return hit;
    return std::nullopt;
}

// A voxel face normal is axis-aligned, so on stepped voxel terrain it is almost always straight
// up. Use the slope of the surface around the spot instead (heights `probe` away on each side),
// falling back to the face normal at an edge.
Float3 slope_normal(const EditorPickResult& hit, float probe,
                    const std::function<std::optional<EditorPickResult>(float, float)>& drop) {
    const Float3 face = length_squared(hit.worldNormal) > 1.0e-6F ? normalize(hit.worldNormal) : Float3{0, 1, 0};
    const auto east = drop(hit.worldPosition.x + probe, hit.worldPosition.z);
    const auto west = drop(hit.worldPosition.x - probe, hit.worldPosition.z);
    const auto north = drop(hit.worldPosition.x, hit.worldPosition.z + probe);
    const auto south = drop(hit.worldPosition.x, hit.worldPosition.z - probe);
    if (!east || !west || !north || !south) return face;
    const float slopeX = (east->worldPosition.y - west->worldPosition.y) / (2.0F * probe);
    const float slopeZ = (north->worldPosition.y - south->worldPosition.y) / (2.0F * probe);
    return normalize(Float3{-slopeX, 1.0F, -slopeZ});
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
    const auto drop = [&](float x, float z) {
        return drop_onto(document, x, z, rayStartY, rayLength, [&](EditorObjectId id) { return id == target; });
    };
    const float probe = std::max(0.05F, surface->voxelSizeMeters * 2.0F);
    const auto surface_normal = [&](const EditorPickResult& hit) { return slope_normal(hit, probe, drop); };
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

ScatterCopiesResult append_scatter_copies(CompoundCommand& command, EditorDocument& document, EditorObjectId groupId,
                                          const std::vector<ScatterSample>& samples,
                                          const std::vector<ScatterSource>& sources, bool alignToSurface) {
    ScatterCopiesResult result;
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
    for (const ScatterSample& sample : samples) {
        if (sample.source >= prepared.size()) { result.error = "Scatter plan refers to a missing source"; return result; }
        const PreparedSource& source = prepared[sample.source];
        const Quaternion align = alignToSurface ? rotation_from_up(sample.normal) : Quaternion{};
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
                copy.parent = groupId;
                copy.attachment.reset();
            } else {
                copy.parent = idMap.at(*original->parent);
            }
            const Float3 offset = subtract(original->transform.position, source.base);
            copy.transform.position = add(sample.position, rotate(align, offset));
            copy.transform.rotation = normalize(multiply(align, original->transform.rotation));
            if (roots.contains(oldId)) result.rootIds.push_back(copy.id);
            command.add(std::make_unique<AddObjectCommand>(std::move(copy), "Scatter copy"));
            ++result.objectCount;
        }
    }
    return result;
}

EditorObject make_scatter_group(EditorObjectId id, std::size_t copies, Float3 position) {
    EditorObject group(id, "Scatter (" + std::to_string(copies) + ")");
    group.transform = make_rigid_transform(position, {});
    group.tags.push_back(std::string(kScatterGroupTag));
    return group;
}

bool is_scatter_group(const EditorObject& object) noexcept {
    return std::find(object.tags.begin(), object.tags.end(), kScatterGroupTag) != object.tags.end();
}

ScatterBuildResult build_scatter_command(EditorDocument& document, EditorObjectId target, const ScatterPlan& plan,
                                         const std::vector<ScatterSource>& sources, const ScatterSettings& settings) {
    ScatterBuildResult result;
    if (!plan.error.empty()) { result.error = plan.error; return result; }
    if (plan.samples.empty()) { result.error = "No spots found on the target surface"; return result; }
    const EditorObject* surface = document.find_object(target);
    if (!surface) { result.error = "The target surface no longer exists"; return result; }

    result.groupId = document.allocate_object_id();
    auto command = std::make_unique<CompoundCommand>("Scatter objects");
    const EditorObjectBounds targetBounds = object_world_bounds(*surface);
    command->add(std::make_unique<AddObjectCommand>(
        make_scatter_group(result.groupId, plan.samples.size(),
                           {(targetBounds.minimum.x + targetBounds.maximum.x) * 0.5F, targetBounds.maximum.y,
                            (targetBounds.minimum.z + targetBounds.maximum.z) * 0.5F}),
        "Scatter group"));
    const ScatterCopiesResult copies =
        append_scatter_copies(*command, document, result.groupId, plan.samples, sources, settings.alignToSurface);
    if (!copies.error.empty()) { result.error = copies.error; return result; }
    result.objectCount = copies.objectCount;
    result.command = std::move(command);
    return result;
}

std::vector<ScatterSample> plan_scatter_dab(const EditorDocument& document, Float3 center, float radius,
                                            const ScatterDabSettings& settings,
                                            const std::vector<Float3>& occupied,
                                            const std::function<bool(EditorObjectId)>& ground) {
    std::vector<ScatterSample> samples;
    if (settings.sourceCount == 0 || !(radius > 0.0F)) return samples;
    const float spacing = std::clamp(settings.minSpacingMeters, kMinScatterSpacingMeters, kMaxScatterSpacingMeters);
    const float spacingSquared = spacing * spacing;
    // About as many tries as a tightly packed disk could hold, so a dab fills most of its room.
    const float area = 3.14159265F * radius * radius;
    const auto attempts = static_cast<std::uint32_t>(
        std::clamp(area / spacingSquared * 2.0F, 4.0F, static_cast<float>(kMaxScatterDabAttempts)));
    const float top = center.y + std::max(4.0F, radius * 2.0F);
    const float length = std::max(8.0F, radius * 4.0F);
    const auto drop = [&](float x, float z) { return drop_onto(document, x, z, top, length, ground); };
    const float probe = std::max(0.05F, settings.probeMeters);
    ScatterRandom random(settings.seed);
    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        // Uniform in the disk: sqrt keeps the density even out to the rim.
        const float r = radius * std::sqrt(random.unit());
        const float theta = 6.28318531F * random.unit();
        const std::size_t source = random.index(settings.sourceCount);
        const float x = center.x + r * std::cos(theta);
        const float z = center.z + r * std::sin(theta);
        const auto too_close = [&](Float3 p) {
            const float dx = p.x - x;
            const float dz = p.z - z;
            return dx * dx + dz * dz < spacingSquared;
        };
        if (std::any_of(occupied.begin(), occupied.end(), too_close)) continue;
        if (std::any_of(samples.begin(), samples.end(), [&](const ScatterSample& s) { return too_close(s.position); }))
            continue;
        const auto hit = drop(x, z);
        if (!hit) continue;
        samples.push_back({hit->worldPosition, slope_normal(*hit, probe, drop), source});
    }
    return samples;
}

} // namespace dve::editor
