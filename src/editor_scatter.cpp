#include "dve/editor_scatter.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <variant>

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

ScatterSettings sanitize_scatter_settings(ScatterSettings settings) noexcept {
    const auto finite_or = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
    settings.count = std::clamp<std::uint32_t>(settings.count, 1U, kMaxScatterCount);
    settings.minSpacingMeters = std::clamp(finite_or(settings.minSpacingMeters, 1.0F), kMinScatterSpacingMeters,
                                           kMaxScatterSpacingMeters);
    settings.yawJitterDegrees = std::clamp(finite_or(settings.yawJitterDegrees, 0.0F), 0.0F, 360.0F);
    settings.minScale = std::clamp(finite_or(settings.minScale, 1.0F), kMinScatterScale, kMaxScatterScale);
    settings.maxScale = std::clamp(finite_or(settings.maxScale, 1.0F), kMinScatterScale, kMaxScatterScale);
    if (settings.maxScale < settings.minScale) std::swap(settings.minScale, settings.maxScale);
    settings.brushRadiusMeters = std::clamp(finite_or(settings.brushRadiusMeters, 2.0F), kMinScatterBrushRadius,
                                            kMaxScatterBrushRadius);
    settings.brushDensity = std::clamp(finite_or(settings.brushDensity, 1.0F), kMinScatterBrushDensity, 1.0F);
    return settings;
}

void assign_scatter_variation(std::vector<ScatterSample>& samples, std::size_t first, std::uint64_t seed,
                              float yawJitterDegrees, float minScale, float maxScale) {
    const float jitter = std::clamp(std::isfinite(yawJitterDegrees) ? yawJitterDegrees : 0.0F, 0.0F, 360.0F) *
                         (3.14159265F / 180.0F);
    const float low = std::min(minScale, maxScale);
    const float high = std::max(minScale, maxScale);
    for (std::size_t i = first; i < samples.size(); ++i) {
        // One small stream per sample index: values do not depend on how many spots came before.
        ScatterRandom random(seed * 0xD6E8FEB86659FD93ULL + 0x632BE59BD9B4E019ULL + i);
        const float yawUnit = random.unit();
        const float scaleUnit = random.unit();
        samples[i].yawRadians = jitter > 0.0F ? (yawUnit - 0.5F) * jitter : 0.0F;
        samples[i].scale = high > low ? low + scaleUnit * (high - low) : low;
    }
}

ScatterPlan plan_scatter(const EditorDocument& document, const std::vector<EditorObjectId>& targets,
                         std::size_t sourceCount, const ScatterSettings& settings) {
    ScatterPlan plan;
    if (targets.empty()) { plan.error = "Choose at least one surface to scatter onto"; return plan; }
    struct Surface { EditorObjectId id; EditorObjectBounds bounds; float area; float probe; };
    std::vector<Surface> surfaces;
    float totalArea = 0.0F;
    for (const EditorObjectId target : targets) {
        const EditorObject* surface = document.find_object(target);
        if (!surface) { plan.error = "A target surface no longer exists"; return plan; }
        if (!surface->voxels || surface->voxels->occupied_voxel_count() == 0 || surface->text3d || surface->gaborVolume) {
            plan.error = targets.size() == 1 ? "The target must be a voxel object with voxels to scatter onto"
                                             : "Every surface must be a voxel object with voxels ('" + surface->name + "' is not)";
            return plan;
        }
        const EditorObjectBounds bounds = object_world_bounds(*surface);
        if (!bounds.valid) { plan.error = "Surface '" + surface->name + "' has no bounds"; return plan; }
        const float area = std::max(1.0e-4F, (bounds.maximum.x - bounds.minimum.x) * (bounds.maximum.z - bounds.minimum.z));
        surfaces.push_back({target, bounds, area, std::max(0.05F, surface->voxelSizeMeters * 2.0F)});
        totalArea += area;
    }
    if (sourceCount == 0) { plan.error = "Nothing to scatter: select objects or a prefab asset"; return plan; }
    const std::set<EditorObjectId> targetSet(targets.begin(), targets.end());
    float top = surfaces.front().bounds.maximum.y;
    float bottom = surfaces.front().bounds.minimum.y;
    for (const Surface& surface : surfaces) {
        top = std::max(top, surface.bounds.maximum.y);
        bottom = std::min(bottom, surface.bounds.minimum.y);
    }
    const ScatterSettings clean = sanitize_scatter_settings(settings);
    const std::uint32_t count = clean.count;
    const float spacingSquared = clean.minSpacingMeters * clean.minSpacingMeters;
    const float rayStartY = top + 1.0F;
    const float rayLength = top - bottom + 2.0F;
    // Only targets count as ground: other objects on them (including the sources) are looked
    // through, not stood on.
    const auto drop = [&](float x, float z) {
        return drop_onto(document, x, z, rayStartY, rayLength, [&](EditorObjectId id) { return targetSet.contains(id); });
    };
    ScatterRandom random(clean.seed);
    const std::uint32_t maximumAttempts = std::max<std::uint32_t>(1U, count) * kScatterAttemptsPerCopy;
    while (plan.samples.size() < count && plan.attempts < maximumAttempts) {
        ++plan.attempts;
        // Draw the surface, both coordinates and the source every attempt so the sequence of
        // draws does not depend on which candidates were rejected. With one surface the first
        // draw is skipped, which keeps single-surface layouts identical to earlier versions.
        const Surface* surface = &surfaces.front();
        if (surfaces.size() > 1) {
            float pick = random.unit() * totalArea;
            for (const Surface& candidate : surfaces) {
                surface = &candidate;
                if (pick < candidate.area) break;
                pick -= candidate.area;
            }
        }
        const EditorObjectBounds& bounds = surface->bounds;
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
        plan.samples.push_back({hit.worldPosition, slope_normal(hit, surface->probe, drop), source});
    }
    assign_scatter_variation(plan.samples, 0, clean.seed, clean.yawJitterDegrees, clean.minScale, clean.maxScale);
    return plan;
}

ScatterPlan plan_scatter(const EditorDocument& document, EditorObjectId target, std::size_t sourceCount,
                         const ScatterSettings& settings) {
    return plan_scatter(document, std::vector<EditorObjectId>{target}, sourceCount, settings);
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
        // Turn about the copy's own up axis first, then tilt onto the surface.
        const Quaternion yaw = sample.yawRadians != 0.0F
            ? quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, sample.yawRadians) : Quaternion{};
        const Quaternion align = normalize(multiply(alignToSurface ? rotation_from_up(sample.normal) : Quaternion{}, yaw));
        const float scale = std::isfinite(sample.scale) ? std::clamp(sample.scale, kMinScatterScale, kMaxScatterScale) : 1.0F;
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
            // Scale about the source's footprint base: offsets and voxel size grow together, so
            // the copy still stands on its spot.
            const Float3 offset = multiply(subtract(original->transform.position, source.base), scale);
            copy.transform.position = add(sample.position, rotate(align, offset));
            copy.transform.rotation = normalize(multiply(align, original->transform.rotation));
            if (scale != 1.0F) {
                copy.voxelSizeMeters *= scale;
                if (copy.attachment)
                    copy.attachment->localTransform.position = multiply(copy.attachment->localTransform.position, scale);
            }
            if (roots.contains(oldId)) result.rootIds.push_back(copy.id);
            command.add(std::make_unique<AddObjectCommand>(std::move(copy), "Scatter copy"));
            ++result.objectCount;
        }
    }
    return result;
}

EditorObject make_scatter_group(EditorObjectId id, std::size_t copies, Float3 position, const ScatterSettings& settings) {
    EditorObject group(id, "Scatter (" + std::to_string(copies) + ")");
    group.transform = make_rigid_transform(position, {});
    group.tags.push_back(std::string(kScatterGroupTag));
    ComponentId componentId = 1;
    for (const Component& component : group.components) componentId = std::max(componentId, component.id + 1);
    group.components.push_back(make_scatter_settings_component(sanitize_scatter_settings(settings), componentId));
    return group;
}

namespace {
std::map<std::string, ComponentValue, std::less<>> scatter_settings_properties(const ScatterSettings& settings) {
    std::map<std::string, ComponentValue, std::less<>> properties;
    properties.emplace("count", static_cast<std::int64_t>(settings.count));
    properties.emplace("spacing", static_cast<double>(settings.minSpacingMeters));
    // Stored as text: a 64-bit seed does not always fit a signed integer property.
    properties.emplace("seed", std::to_string(settings.seed));
    properties.emplace("align", settings.alignToSurface);
    properties.emplace("yaw_jitter", static_cast<double>(settings.yawJitterDegrees));
    properties.emplace("scale_min", static_cast<double>(settings.minScale));
    properties.emplace("scale_max", static_cast<double>(settings.maxScale));
    properties.emplace("brush_radius", static_cast<double>(settings.brushRadiusMeters));
    properties.emplace("brush_density", static_cast<double>(settings.brushDensity));
    return properties;
}
} // namespace

Component make_scatter_settings_component(const ScatterSettings& settings, ComponentId id) {
    Component component;
    component.id = id;
    component.type = std::string(kScatterSettingsComponent);
    component.properties = scatter_settings_properties(settings);
    return component;
}

std::optional<ScatterSettings> read_scatter_settings(const EditorObject& object) {
    const auto found = std::find_if(object.components.begin(), object.components.end(),
                                    [](const Component& c) { return c.type == kScatterSettingsComponent; });
    if (found == object.components.end()) return std::nullopt;
    ScatterSettings settings;
    const auto number = [&](std::string_view name, double fallback) {
        const auto it = found->properties.find(name);
        if (it == found->properties.end()) return fallback;
        if (const auto* d = std::get_if<double>(&it->second)) return *d;
        if (const auto* i = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*i);
        return fallback;
    };
    settings.count = static_cast<std::uint32_t>(std::clamp(number("count", settings.count), 1.0, double(kMaxScatterCount)));
    settings.minSpacingMeters = static_cast<float>(number("spacing", settings.minSpacingMeters));
    if (const auto it = found->properties.find("seed"); it != found->properties.end()) {
        if (const auto* text = std::get_if<std::string>(&it->second)) {
            try { settings.seed = std::stoull(*text); } catch (...) {}
        } else if (const auto* i = std::get_if<std::int64_t>(&it->second)) {
            settings.seed = static_cast<std::uint64_t>(*i);
        }
    }
    if (const auto it = found->properties.find("align"); it != found->properties.end())
        if (const auto* b = std::get_if<bool>(&it->second)) settings.alignToSurface = *b;
    settings.yawJitterDegrees = static_cast<float>(number("yaw_jitter", settings.yawJitterDegrees));
    settings.minScale = static_cast<float>(number("scale_min", settings.minScale));
    settings.maxScale = static_cast<float>(number("scale_max", settings.maxScale));
    settings.brushRadiusMeters = static_cast<float>(number("brush_radius", settings.brushRadiusMeters));
    settings.brushDensity = static_cast<float>(number("brush_density", settings.brushDensity));
    return sanitize_scatter_settings(settings);
}

void append_scatter_settings_update(CompoundCommand& command, const EditorObject& group, const ScatterSettings& settings) {
    const ScatterSettings clean = sanitize_scatter_settings(settings);
    const auto found = std::find_if(group.components.begin(), group.components.end(),
                                    [](const Component& c) { return c.type == kScatterSettingsComponent; });
    if (found == group.components.end()) {
        ComponentId id = 1;
        for (const Component& component : group.components) id = std::max(id, component.id + 1);
        command.add(std::make_unique<AddComponentCommand>(group.id, make_scatter_settings_component(clean, id),
                                                          "Store scatter settings"));
        return;
    }
    for (auto& [name, value] : scatter_settings_properties(clean)) {
        const auto it = found->properties.find(name);
        std::optional<ComponentValue> before;
        if (it != found->properties.end()) {
            // Only scalar and text values are stored here (Float3 has no operator==).
            const bool same = it->second.index() == value.index() && std::visit([&](const auto& current) {
                using T = std::decay_t<decltype(current)>;
                if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> ||
                              std::is_same_v<T, double> || std::is_same_v<T, std::string>)
                    return current == std::get<T>(value);
                else
                    return false;
            }, it->second);
            if (same) continue;
            before = it->second;
        }
        command.add(std::make_unique<SetComponentPropertyCommand>(group.id, found->id, name, before, value,
                                                                  "Store scatter settings"));
    }
}

bool is_scatter_group(const EditorObject& object) noexcept {
    return std::find(object.tags.begin(), object.tags.end(), kScatterGroupTag) != object.tags.end();
}

ScatterBuildResult build_scatter_command(EditorDocument& document, const std::vector<EditorObjectId>& targets,
                                         const ScatterPlan& plan, const std::vector<ScatterSource>& sources,
                                         const ScatterSettings& settings) {
    ScatterBuildResult result;
    if (!plan.error.empty()) { result.error = plan.error; return result; }
    if (plan.samples.empty()) { result.error = "No spots found on the target surface"; return result; }
    EditorObjectBounds bounds;
    for (const EditorObjectId target : targets) {
        const EditorObject* surface = document.find_object(target);
        if (!surface) { result.error = "A target surface no longer exists"; return result; }
        const EditorObjectBounds b = object_world_bounds(*surface);
        if (!b.valid) continue;
        if (!bounds.valid) { bounds = b; continue; }
        bounds.minimum = {std::min(bounds.minimum.x, b.minimum.x), std::min(bounds.minimum.y, b.minimum.y),
                          std::min(bounds.minimum.z, b.minimum.z)};
        bounds.maximum = {std::max(bounds.maximum.x, b.maximum.x), std::max(bounds.maximum.y, b.maximum.y),
                          std::max(bounds.maximum.z, b.maximum.z)};
    }
    if (!bounds.valid) { result.error = "The target surface has no bounds"; return result; }

    result.groupId = document.allocate_object_id();
    auto command = std::make_unique<CompoundCommand>("Scatter objects");
    command->add(std::make_unique<AddObjectCommand>(
        make_scatter_group(result.groupId, plan.samples.size(),
                           {(bounds.minimum.x + bounds.maximum.x) * 0.5F, bounds.maximum.y,
                            (bounds.minimum.z + bounds.maximum.z) * 0.5F},
                           settings),
        "Scatter group"));
    const ScatterCopiesResult copies =
        append_scatter_copies(*command, document, result.groupId, plan.samples, sources, settings.alignToSurface);
    if (!copies.error.empty()) { result.error = copies.error; return result; }
    result.objectCount = copies.objectCount;
    result.command = std::move(command);
    return result;
}

ScatterBuildResult build_scatter_command(EditorDocument& document, EditorObjectId target, const ScatterPlan& plan,
                                         const std::vector<ScatterSource>& sources, const ScatterSettings& settings) {
    return build_scatter_command(document, std::vector<EditorObjectId>{target}, plan, sources, settings);
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
    // About as many tries as a tightly packed disk could hold, so a full-density dab fills most
    // of its room. Lower density caps how many spots one dab keeps, as a share of what random
    // packing at this spacing holds (about 0.7 per spacing squared), so 50% is about half as full.
    const auto attempts = static_cast<std::uint32_t>(
        std::clamp(area / spacingSquared * 2.0F, 4.0F, static_cast<float>(kMaxScatterDabAttempts)));
    const float density = std::clamp(std::isfinite(settings.density) ? settings.density : 1.0F, kMinScatterBrushDensity, 1.0F);
    // The cap counts copies already inside the disk, so overlapping dabs along a stroke (or
    // going over an area again) do not build up past the density.
    std::size_t maximumKept = std::numeric_limits<std::size_t>::max();
    if (density < 1.0F) {
        const auto cap = std::max<std::size_t>(1U, static_cast<std::size_t>(std::lround(density * 0.7F * area / spacingSquared)));
        const auto inside = static_cast<std::size_t>(std::count_if(occupied.begin(), occupied.end(), [&](Float3 p) {
            const float dx = p.x - center.x;
            const float dz = p.z - center.z;
            return dx * dx + dz * dz <= radius * radius;
        }));
        maximumKept = cap > inside ? cap - inside : 0U;
    }
    const float top = center.y + std::max(4.0F, radius * 2.0F);
    const float length = std::max(8.0F, radius * 4.0F);
    const auto drop = [&](float x, float z) { return drop_onto(document, x, z, top, length, ground); };
    const float probe = std::max(0.05F, settings.probeMeters);
    ScatterRandom random(settings.seed);
    for (std::uint32_t attempt = 0; attempt < attempts && samples.size() < maximumKept; ++attempt) {
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
    assign_scatter_variation(samples, 0, settings.seed, settings.yawJitterDegrees, settings.minScale, settings.maxScale);
    return samples;
}

} // namespace dve::editor
