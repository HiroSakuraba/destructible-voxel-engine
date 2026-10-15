#include "dve/editor_duplicate_special.hpp"

#include <cmath>

namespace dve::editor {
namespace {
constexpr float kRadiansPerDegree = 0.017453292519943295F;
bool finite3(Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}  // namespace

std::vector<DuplicateCopyPose> duplicate_special_poses(const RigidTransform& source, const DuplicateSpecialSettings& settings) {
    std::vector<DuplicateCopyPose> poses;
    if (settings.copies == 0 || settings.copies > kMaxDuplicateCopies) return poses;
    poses.reserve(settings.copies);
    const Quaternion step = normalize(quaternion_from_euler_xyz({settings.rotationDegrees.x * kRadiansPerDegree,
                                                                 settings.rotationDegrees.y * kRadiansPerDegree,
                                                                 settings.rotationDegrees.z * kRadiansPerDegree}));
    RigidTransform current = source;
    Quaternion turned{};  // step^k, world axes
    float scale = 1.0F;
    for (std::uint32_t k = 1; k <= settings.copies; ++k) {
        scale *= settings.scale;
        if (settings.pivot == DuplicatePivot::Point) {
            turned = normalize(multiply(step, turned));
            const Float3 moved{source.position.x + settings.offset.x * static_cast<float>(k),
                               source.position.y + settings.offset.y * static_cast<float>(k),
                               source.position.z + settings.offset.z * static_cast<float>(k)};
            const Float3 arm = subtract(moved, settings.point);
            current.position = add(settings.point, rotate(turned, arm));
            current.rotation = normalize(multiply(turned, source.rotation));
        } else if (settings.space == DuplicateSpace::Local) {
            // One step in the previous copy's own frame: offsets follow the turn (arcs, coils).
            current.position = add(current.position, rotate(current.rotation, settings.offset));
            current.rotation = normalize(multiply(current.rotation, step));
        } else {
            current.position = add(current.position, settings.offset);
            current.rotation = normalize(multiply(step, current.rotation));
        }
        poses.push_back({current, scale});
    }
    return poses;
}

std::string duplicate_special_problem(const DuplicateSpecialSettings& settings, std::size_t objectsPerCopy) {
    if (settings.copies == 0) return "Make at least 1 copy";
    if (settings.copies > kMaxDuplicateCopies) return "At most " + std::to_string(kMaxDuplicateCopies) + " copies";
    if (objectsPerCopy * settings.copies > kMaxDuplicateObjects)
        return std::to_string(objectsPerCopy * settings.copies) + " objects is too many (most is " +
               std::to_string(kMaxDuplicateObjects) + "); use fewer copies or a smaller selection";
    if (!finite3(settings.offset) || !finite3(settings.rotationDegrees) || !finite3(settings.point)) return "Values must be finite";
    if (!(settings.scale > 0.0F) || !std::isfinite(settings.scale)) return "Scale must be above 0";
    const double last = std::pow(static_cast<double>(settings.scale), static_cast<double>(settings.copies));
    if (last < 1.0e-3 || last > 1.0e3) return "Scale per copy shrinks or grows the last copy too far (over 1000x)";
    return {};
}

}  // namespace dve::editor
