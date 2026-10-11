#pragma once
#include "dve/rigid_body_adapter.hpp"
#include "dve/voxel_metric.hpp"
namespace dve {
// Convert an already SI, isotropic descriptor to positive local-axis dimensions.
// Center of mass, volume, and the full inertia tensor must change together.
inline bool scale_voxel_body(RigidBodyCreateDesc& body, const RigidTransform& origin,
                            Float3 localCenter, Float3 axes) noexcept {
    if (!valid_voxel_metric(axes)) return false;
    if (unit_voxel_scale(axes)) return static_cast<bool>(validate_rigid_body_desc(body));
    const double x=axes.x,y=axes.y,z=axes.z,volume=x*y*z;
    const auto old=body.inertiaKilogramMetersSquared;
    const double a=(old.yy+old.zz-old.xx)*0.5;
    const double b=(old.xx+old.zz-old.yy)*0.5;
    const double c=(old.xx+old.yy-old.zz)*0.5;
    body.inertiaKilogramMetersSquared={volume*(y*y*b+z*z*c),volume*(x*x*a+z*z*c),
        volume*(x*x*a+y*y*b),volume*x*y*old.xy,volume*x*z*old.xz,volume*y*z*old.yz};
    body.massKilograms*=volume;
    for (auto& box:body.boxes) {
        box.center=multiply_components(box.center,axes);
        box.halfExtents=multiply_components(box.halfExtents,axes);
    }
    body.transform.position=transform_point(origin,multiply_components(localCenter,axes));
    return static_cast<bool>(validate_rigid_body_desc(body));
}
} // namespace dve
