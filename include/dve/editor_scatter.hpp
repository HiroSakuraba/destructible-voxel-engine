#pragma once

// Scatter Objects: pseudo-random placement of copies across a surface, for organic scenes
// (rocks, plants, debris).
//
//   1. Select the objects to scatter, then click the surface object last (it becomes the
//      active selection, the "target"). A prefab selected in the Assets panel joins the pool.
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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dve/editor_command.hpp"
#include "dve/editor_document.hpp"

namespace dve::editor {

inline constexpr std::uint32_t kMaxScatterCount = 1000;
inline constexpr float kMinScatterSpacingMeters = 0.05F;
inline constexpr float kMaxScatterSpacingMeters = 100.0F;
// Candidate spots tried per requested copy before giving up on the rest.
inline constexpr std::uint32_t kScatterAttemptsPerCopy = 30;

struct ScatterSettings {
    std::uint32_t count{20};
    float minSpacingMeters{1.0F};
    std::uint64_t seed{1};
    // Tilt each copy so its up axis follows the surface normal at its spot.
    bool alignToSurface{};
};

// One kept spot. `source` indexes the caller's source list.
struct ScatterSample {
    Float3 position{};
    Float3 normal{0.0F, 1.0F, 0.0F};
    std::size_t source{};
};

struct ScatterPlan {
    std::vector<ScatterSample> samples;
    std::uint32_t attempts{};
    std::uint32_t missedSurface{};   // candidate fell through a gap in the target
    std::uint32_t tooClose{};        // candidate within the spacing of a kept spot
    // Why nothing can be planned (bad target, no sources); empty when the plan is usable.
    std::string error;
};

// Places up to settings.count spots on `target`'s upward-facing surface.
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

// Builds (does not execute) the command that adds the "Scatter" group and every copy. Ids are
// allocated from `document`. Copies are independent: prefab links are dropped.
[[nodiscard]] ScatterBuildResult build_scatter_command(EditorDocument& document, EditorObjectId target,
                                                       const ScatterPlan& plan,
                                                       const std::vector<ScatterSource>& sources,
                                                       const ScatterSettings& settings);

} // namespace dve::editor
