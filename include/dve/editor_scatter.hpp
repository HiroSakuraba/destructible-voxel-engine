#pragma once

// Scatter Objects: pseudo-random placement of copies across a surface, for organic scenes
// (rocks, plants, debris).
//
//   1. Select the objects to scatter, then click the surface object last (it becomes the
//      active selection, the "target"). A prefab selected in the Assets panel joins the pool.
//      For several surfaces: select the sources, Create > Set Scatter Sources, then select
//      every surface; all selected voxel objects become targets.
//   2. Create > Scatter Objects opens a preview. Nothing in the document changes while
//      previewing; Esc cancels.
//   3. Enter commits one undoable command: an empty "Scatter" group under which every copy is
//      an ordinary, independent object (no link back to its source).
//
// Placement is deterministic: the same target, sources, settings and seed always give the
// same layout. Candidate spots are drawn uniformly over the target's footprint and dropped
// straight down onto the target's surface; a spot is kept only if it is at least
// `minSpacingMeters` (measured across the ground plane) from every spot already kept, so copies
// spread out naturally instead of clumping (dart-throwing Poisson-disk sampling).

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

#include "dve/component.hpp"
#include "dve/editor_command.hpp"
#include "dve/editor_document.hpp"

namespace dve::editor {

inline constexpr std::uint32_t kMaxScatterCount = 1000;
inline constexpr float kMinScatterSpacingMeters = 0.05F;
inline constexpr float kMaxScatterSpacingMeters = 100.0F;
// Candidate spots tried per requested copy before giving up on the rest.
inline constexpr std::uint32_t kScatterAttemptsPerCopy = 30;

inline constexpr float kMinScatterScale = 0.1F;
inline constexpr float kMaxScatterScale = 10.0F;
inline constexpr float kMinScatterBrushRadius = 0.25F;
inline constexpr float kMaxScatterBrushRadius = 50.0F;
inline constexpr float kMinScatterBrushDensity = 0.05F;

struct ScatterSettings {
    std::uint32_t count{20};
    float minSpacingMeters{1.0F};
    std::uint64_t seed{1};
    // Tilt each copy so its up axis follows the surface normal at its spot.
    bool alignToSurface{};
    // Per-copy variation, drawn from the seed: a random turn about the up axis within
    // +/- yawJitterDegrees / 2 (360 = any direction), and a uniform scale in [minScale, maxScale].
    // A voxel copy scales by changing its voxel size; the voxel count stays the same.
    float yawJitterDegrees{0.0F};
    float minScale{1.0F};
    float maxScale{1.0F};
    // Brush only: radius, and how full a dab gets (1 = as many copies as the spacing allows).
    float brushRadiusMeters{2.0F};
    float brushDensity{1.0F};
    auto operator<=>(const ScatterSettings&) const = default;
};

// Clamps every field into its valid range (and orders minScale <= maxScale).
[[nodiscard]] ScatterSettings sanitize_scatter_settings(ScatterSettings settings) noexcept;

// One kept spot. `source` indexes the caller's source list.
struct ScatterSample {
    Float3 position{};
    Float3 normal{0.0F, 1.0F, 0.0F};
    std::size_t source{};
    float yawRadians{};
    float scale{1.0F};
};

// Fills yawRadians and scale for samples[first..]. Drawn from a stream separate from placement,
// so turning variation on or off never moves a spot; sample i always gets the same values.
void assign_scatter_variation(std::vector<ScatterSample>& samples, std::size_t first, std::uint64_t seed,
                              float yawJitterDegrees, float minScale, float maxScale);

struct ScatterPlan {
    std::vector<ScatterSample> samples;
    std::uint32_t attempts{};
    std::uint32_t missedSurface{};   // candidate fell through a gap in the target
    std::uint32_t tooClose{};        // candidate within the spacing of a kept spot
    // Why nothing can be planned (bad target, no sources); empty when the plan is usable.
    std::string error;
};

// Places up to settings.count spots on the targets' upward-facing surfaces. With several
// targets each candidate first picks a target, weighted by its footprint area, so copies spread
// evenly over all of them; a spot may land on any target.
[[nodiscard]] ScatterPlan plan_scatter(const EditorDocument& document, const std::vector<EditorObjectId>& targets,
                                       std::size_t sourceCount, const ScatterSettings& settings);
[[nodiscard]] ScatterPlan plan_scatter(const EditorDocument& document, EditorObjectId target,
                                       std::size_t sourceCount, const ScatterSettings& settings);

// Something to copy: the subtrees under `roots` in `document` (the scene itself, or a prefab's
// template document). The copy keeps the source's shape, rotation and relative layout; its
// footprint's bottom centre sits on the spot.
struct ScatterSource {
    const EditorDocument* document{};
    std::vector<EditorObjectId> roots;
    std::string label;
};

struct ScatterBuildResult {
    std::unique_ptr<CompoundCommand> command;
    EditorObjectId groupId{};
    std::size_t objectCount{};  // copies including their children, excluding the group
    std::string error;
};

// Scatter groups (fill and brush) carry this tag; the brush erases only their children.
inline constexpr std::string_view kScatterGroupTag = "dve.scatter";
// The group also carries the settings that made it (component kScatterSettingsComponent), so
// they are saved with the scene and can be picked up again.
[[nodiscard]] EditorObject make_scatter_group(EditorObjectId id, std::size_t copies, Float3 position,
                                              const ScatterSettings& settings = {});
[[nodiscard]] bool is_scatter_group(const EditorObject& object) noexcept;

// Editor-only component; dropped from runtime exports.
inline constexpr std::string_view kScatterSettingsComponent = "dve.editor_scatter";
[[nodiscard]] Component make_scatter_settings_component(const ScatterSettings& settings, ComponentId id);
[[nodiscard]] std::optional<ScatterSettings> read_scatter_settings(const EditorObject& object);
// Adds the commands that make `group` carry `settings` (adding the component or changing only
// the properties that differ). Adds nothing when it already matches.
void append_scatter_settings_update(CompoundCommand& command, const EditorObject& group,
                                    const ScatterSettings& settings);

struct ScatterCopiesResult {
    std::vector<EditorObjectId> rootIds;  // one per copy
    std::size_t objectCount{};            // copies including their children
    std::string error;
};
// Adds one AddObjectCommand per copied object to `command`, parented under `groupId`.
[[nodiscard]] ScatterCopiesResult append_scatter_copies(CompoundCommand& command, EditorDocument& document,
                                                        EditorObjectId groupId,
                                                        const std::vector<ScatterSample>& samples,
                                                        const std::vector<ScatterSource>& sources,
                                                        bool alignToSurface);

// Builds (does not execute) the command that adds the "Scatter" group and every copy. Ids are
// allocated from `document`. Copies are independent: prefab links are dropped.
[[nodiscard]] ScatterBuildResult build_scatter_command(EditorDocument& document,
                                                       const std::vector<EditorObjectId>& targets,
                                                       const ScatterPlan& plan,
                                                       const std::vector<ScatterSource>& sources,
                                                       const ScatterSettings& settings);
[[nodiscard]] ScatterBuildResult build_scatter_command(EditorDocument& document, EditorObjectId target,
                                                       const ScatterPlan& plan,
                                                       const std::vector<ScatterSource>& sources,
                                                       const ScatterSettings& settings);

// Scatter brush: one dab places spots inside a disk around `center`, dropped onto any object
// `ground` accepts, at least the spacing from each other and from every point in `occupied`
// (copies already placed). Deterministic for a given seed.
inline constexpr std::uint32_t kMaxScatterDabAttempts = 200;
struct ScatterDabSettings {
    float minSpacingMeters{1.0F};
    std::uint64_t seed{1};
    std::size_t sourceCount{};
    float probeMeters{0.5F};  // slope sampling distance for the surface normal
    float density{1.0F};      // share of a full dab (0.05 to 1)
    float yawJitterDegrees{};
    float minScale{1.0F};
    float maxScale{1.0F};
};
[[nodiscard]] std::vector<ScatterSample> plan_scatter_dab(const EditorDocument& document, Float3 center, float radius,
                                                          const ScatterDabSettings& settings,
                                                          const std::vector<Float3>& occupied,
                                                          const std::function<bool(EditorObjectId)>& ground);

} // namespace dve::editor
