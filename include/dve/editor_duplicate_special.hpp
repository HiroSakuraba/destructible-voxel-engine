#pragma once

// Duplicate Special (artist request, 15 Oct 2026): N copies of the selection, each offset,
// turned and scaled from the one before. "Duplicate a feather, offset it a few inches, turn it
// 5 degrees, make 200 copies."
//
// Each copy k (1..count) is the source moved by k steps:
//   Each copy's pivot: the step is taken from the previous copy, in its own frame (Local) or in
//     world axes (World). Local offsets follow the turn, so offset + turn makes an arc.
//   Around a point: copy k is the source turned k * rotation about the point (world axes), after
//     moving it k * offset in world axes. A turn alone makes a ring or fan; adding an offset along
//     the turn axis makes a spiral.
// Scale multiplies the copy's size by scale^k (about its own origin).

#include <cstdint>
#include <string>
#include <vector>

#include "dve/transform.hpp"

namespace dve::editor {

enum class DuplicatePivot : std::uint8_t { EachCopy, Point };
enum class DuplicateSpace : std::uint8_t { Local, World };

struct DuplicateSpecialSettings {
    std::uint32_t copies{10};
    Float3 offset{0.1F, 0.0F, 0.0F};          // meters per copy
    Float3 rotationDegrees{0.0F, 5.0F, 0.0F};  // XYZ Euler degrees per copy
    float scale{1.0F};                         // size multiplier per copy
    DuplicatePivot pivot{DuplicatePivot::EachCopy};
    DuplicateSpace space{DuplicateSpace::Local};  // Each copy's pivot only
    Float3 point{};                            // the fixed pivot (Around a point)
    bool group{true};                          // put the copies in one new group
};

inline constexpr std::uint32_t kMaxDuplicateCopies = 2000;
inline constexpr std::size_t kMaxDuplicateObjects = 20000;  // copies x objects per copy

struct DuplicateCopyPose {
    RigidTransform transform;   // the copy's world transform
    float scale{1.0F};          // size multiplier relative to the source
};

// Poses of copies 1..settings.copies of an object whose world transform is `source`.
[[nodiscard]] std::vector<DuplicateCopyPose> duplicate_special_poses(const RigidTransform& source,
                                                                     const DuplicateSpecialSettings& settings);

// Empty when the settings are usable, else why not.
[[nodiscard]] std::string duplicate_special_problem(const DuplicateSpecialSettings& settings, std::size_t objectsPerCopy);

}  // namespace dve::editor
