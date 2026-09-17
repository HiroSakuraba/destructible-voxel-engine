#include "dve/rigid_body_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace dve {
namespace {

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
[[nodiscard]] bool finite(Quaternion value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}
[[nodiscard]] bool finite(const RigidTransform& value) noexcept {
    return finite(value.position) && finite(value.rotation);
}
[[nodiscard]] bool finite(const SolverInertiaTensor& value) noexcept {
    return finite(value.xx) && finite(value.yy) && finite(value.zz) && finite(value.xy) &&
        finite(value.xz) && finite(value.yz);
}

[[nodiscard]] float speed(Float3 value) noexcept { return std::sqrt(length_squared(value)); }

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

[[nodiscard]] Quaternion axis_angle(Float3 axis, float radians) noexcept {
    axis = normalize(axis);
    const float half = radians * 0.5F;
    const float sine = std::sin(half);
    return normalize({axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half)});
}

[[nodiscard]] SolverInertiaTensor round_inertia_to_float(const SolverInertiaTensor& value) noexcept {
    return {
        static_cast<double>(static_cast<float>(value.xx)),
        static_cast<double>(static_cast<float>(value.yy)),
        static_cast<double>(static_cast<float>(value.zz)),
        static_cast<double>(static_cast<float>(value.xy)),
        static_cast<double>(static_cast<float>(value.xz)),
        static_cast<double>(static_cast<float>(value.yz)),
    };
}

struct QueryAabb {
    Float3 minimum{};
    Float3 maximum{};
};

[[nodiscard]] QueryAabb world_aabb(
    const RigidTransform& transform, const SolverBox& box, float expansion = 0.0F) noexcept {
    const Float3 center = transform_point(transform, box.center);
    const Float3 axisX = rotate(transform.rotation, {box.halfExtents.x, 0.0F, 0.0F});
    const Float3 axisY = rotate(transform.rotation, {0.0F, box.halfExtents.y, 0.0F});
    const Float3 axisZ = rotate(transform.rotation, {0.0F, 0.0F, box.halfExtents.z});
    const Float3 extent{
        std::abs(axisX.x) + std::abs(axisY.x) + std::abs(axisZ.x) + expansion,
        std::abs(axisX.y) + std::abs(axisY.y) + std::abs(axisZ.y) + expansion,
        std::abs(axisX.z) + std::abs(axisY.z) + std::abs(axisZ.z) + expansion};
    return {subtract(center, extent), add(center, extent)};
}

[[nodiscard]] bool overlaps(const QueryAabb& a, const RigidBodyWorldBounds& b) noexcept {
    return a.minimum.x <= b.maximum.x && a.maximum.x >= b.minimum.x &&
        a.minimum.y <= b.maximum.y && a.maximum.y >= b.minimum.y &&
        a.minimum.z <= b.maximum.z && a.maximum.z >= b.minimum.z;
}

[[nodiscard]] bool ray_aabb(
    Float3 origin,
    Float3 direction,
    float maximumDistance,
    const QueryAabb& bounds,
    float& distance,
    Float3& normal) noexcept {
    float entry = 0.0F;
    float exit = maximumDistance;
    normal = {};
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const float originAxis = axis == 0U ? origin.x : (axis == 1U ? origin.y : origin.z);
        const float directionAxis = axis == 0U ? direction.x : (axis == 1U ? direction.y : direction.z);
        const float minimumAxis = axis == 0U ? bounds.minimum.x :
            (axis == 1U ? bounds.minimum.y : bounds.minimum.z);
        const float maximumAxis = axis == 0U ? bounds.maximum.x :
            (axis == 1U ? bounds.maximum.y : bounds.maximum.z);
        if (std::abs(directionAxis) <= 1.0e-8F) {
            if (originAxis < minimumAxis || originAxis > maximumAxis) return false;
            continue;
        }
        float nearDistance = (minimumAxis - originAxis) / directionAxis;
        float farDistance = (maximumAxis - originAxis) / directionAxis;
        float normalSign = -1.0F;
        if (nearDistance > farDistance) {
            std::swap(nearDistance, farDistance);
            normalSign = 1.0F;
        }
        if (nearDistance > entry) {
            entry = nearDistance;
            normal = axis == 0U ? Float3{normalSign, 0.0F, 0.0F} :
                (axis == 1U ? Float3{0.0F, normalSign, 0.0F} :
                              Float3{0.0F, 0.0F, normalSign});
        }
        exit = std::min(exit, farDistance);
        if (entry > exit) return false;
    }
    if (exit < 0.0F || entry > maximumDistance) return false;
    distance = std::max(0.0F, entry);
    return true;
}

[[nodiscard]] bool ignored(
    RigidBodyHandle handle, const RigidBodyQueryFilter& filter) noexcept {
    return std::find(filter.ignoredBodies.begin(), filter.ignoredBodies.end(), handle) !=
        filter.ignoredBodies.end();
}

void sort_query_hits(std::vector<RigidBodyQueryHit>& hits) {
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return std::tie(a.fraction, a.distance, a.body, a.subShape) <
            std::tie(b.fraction, b.distance, b.body, b.subShape);
    });
}

} // namespace

bool inertia_is_positive_semidefinite(
    const SolverInertiaTensor& inertia,
    double tolerance) noexcept {
    if (tolerance < 0.0 || !finite(inertia)) return false;
    if (inertia.xx < -tolerance || inertia.yy < -tolerance || inertia.zz < -tolerance) return false;
    const double minorXY = inertia.xx * inertia.yy - inertia.xy * inertia.xy;
    const double minorXZ = inertia.xx * inertia.zz - inertia.xz * inertia.xz;
    const double minorYZ = inertia.yy * inertia.zz - inertia.yz * inertia.yz;
    if (minorXY < -tolerance || minorXZ < -tolerance || minorYZ < -tolerance) return false;
    const double determinant = inertia.xx * inertia.yy * inertia.zz +
        2.0 * inertia.xy * inertia.xz * inertia.yz -
        inertia.xx * inertia.yz * inertia.yz -
        inertia.yy * inertia.xz * inertia.xz -
        inertia.zz * inertia.xy * inertia.xy;
    return determinant >= -tolerance;
}

bool inertia_is_positive_definite(const SolverInertiaTensor& inertia) noexcept {
    if (!finite(inertia)) return false;
    // Sylvester's criterion for a real symmetric 3x3 matrix. Strict comparisons are
    // intentional: a fully dynamic body needs an invertible inertia tensor.
    const double leading1 = inertia.xx;
    const double leading2 = inertia.xx * inertia.yy - inertia.xy * inertia.xy;
    const double determinant = inertia.xx * inertia.yy * inertia.zz +
        2.0 * inertia.xy * inertia.xz * inertia.yz -
        inertia.xx * inertia.yz * inertia.yz -
        inertia.yy * inertia.xz * inertia.xz -
        inertia.zz * inertia.xy * inertia.xy;
    return leading1 > 0.0 && leading2 > 0.0 && determinant > 0.0;
}

RigidBodyValidationResult validate_rigid_body_desc(const RigidBodyCreateDesc& desc) noexcept {
    RigidBodyValidationResult result;
    if (desc.boxes.empty()) {
        result.error = RigidBodyValidationError::EmptyCollisionShape;
        return result;
    }
    if (!(desc.massKilograms > 0.0) || !finite(desc.massKilograms) ||
        desc.massKilograms > static_cast<double>(std::numeric_limits<float>::max())) {
        result.error = RigidBodyValidationError::InvalidMass;
        return result;
    }
    if (!finite(desc.transform)) {
        result.error = RigidBodyValidationError::InvalidTransform;
        return result;
    }
    const double rotationNormSquared =
        static_cast<double>(desc.transform.rotation.x) * desc.transform.rotation.x +
        static_cast<double>(desc.transform.rotation.y) * desc.transform.rotation.y +
        static_cast<double>(desc.transform.rotation.z) * desc.transform.rotation.z +
        static_cast<double>(desc.transform.rotation.w) * desc.transform.rotation.w;
    if (!(rotationNormSquared > 1.0e-20) || !finite(rotationNormSquared)) {
        result.error = RigidBodyValidationError::InvalidTransform;
        return result;
    }
    if (!finite(desc.linearVelocity) || !finite(desc.angularVelocity)) {
        result.error = RigidBodyValidationError::InvalidVelocity;
        return result;
    }
    for (const SolverBox& box : desc.boxes) {
        if (!finite(box.center) || !finite(box.halfExtents) || !(box.halfExtents.x > 0.0F) ||
            !(box.halfExtents.y > 0.0F) || !(box.halfExtents.z > 0.0F)) {
            result.error = RigidBodyValidationError::InvalidCollisionBox;
            return result;
        }
    }
    if (!finite(desc.inertiaKilogramMetersSquared) ||
        !inertia_is_positive_semidefinite(desc.inertiaKilogramMetersSquared)) {
        result.error = RigidBodyValidationError::InvalidInertia;
        return result;
    }
    result.inertiaAfterFloatConversion = round_inertia_to_float(desc.inertiaKilogramMetersSquared);
    if (!inertia_is_positive_definite(result.inertiaAfterFloatConversion)) {
        result.error = RigidBodyValidationError::SingularInertiaAfterFloatConversion;
        return result;
    }

    result.normalizedTransform = make_rigid_transform(desc.transform.position, desc.transform.rotation);
    result.massKilogramsFloat = static_cast<float>(desc.massKilograms);
    return result;
}

bool rigid_body_state_is_finite(const RigidBodyState& state) noexcept {
    if (!finite(state.previousTransform) || !finite(state.currentTransform) ||
        !finite(state.linearVelocity) || !finite(state.angularVelocity)) return false;
    const auto valid_rotation = [](Quaternion rotation) {
        const double normSquared = static_cast<double>(rotation.x) * rotation.x +
            static_cast<double>(rotation.y) * rotation.y +
            static_cast<double>(rotation.z) * rotation.z +
            static_cast<double>(rotation.w) * rotation.w;
        return std::isfinite(normSquared) && normSquared > 1.0e-20;
    };
    return valid_rotation(state.previousTransform.rotation) &&
        valid_rotation(state.currentTransform.rotation);
}

bool IRigidBodyWorld::apply_impulse_at_point(
    RigidBodyHandle handle, Float3 worldImpulse, Float3 worldPoint) {
    if (!finite(worldPoint)) return false;
    return apply_impulse(handle, worldImpulse);
}

bool IRigidBodyWorld::apply_force_at_point(
    RigidBodyHandle handle, Float3 worldForce, Float3 worldPoint) {
    if (!finite(worldPoint)) return false;
    return apply_force(handle, worldForce);
}

bool IRigidBodyWorld::apply_angular_impulse(RigidBodyHandle, Float3) { return false; }
bool IRigidBodyWorld::apply_torque(RigidBodyHandle, Float3) { return false; }

std::vector<RigidBodyQueryHit> IRigidBodyWorld::ray_cast_all(
    Float3, Float3, float, const RigidBodyQueryFilter&) const {
    return {};
}

std::vector<RigidBodyQueryHit> IRigidBodyWorld::overlap_aabb(
    RigidBodyWorldBounds, const RigidBodyQueryFilter&) const {
    return {};
}

std::vector<RigidBodyQueryHit> IRigidBodyWorld::cast_sphere_all(
    Float3, float, Float3, float, const RigidBodyQueryFilter&) const {
    return {};
}

void IRigidBodyWorld::set_contact_sink(IPhysicsContactSink* sink) noexcept {
    (void)sink;
}

bool IRigidBodyWorld::set_contact_material(
    RigidBodyHandle handle, std::uint16_t material) noexcept {
    (void)handle;
    (void)material;
    return false;
}

bool validate_rigid_body_constraint_desc(const RigidBodyConstraintDesc& desc) noexcept {
    constexpr float kPi = 3.14159265358979323846F;
    if (desc.parentBody == kInvalidRigidBodyHandle || desc.childBody == kInvalidRigidBodyHandle ||
        desc.parentBody == desc.childBody || !finite(desc.parentAnchorLocal) ||
        !finite(desc.childAnchorLocal) || !finite(desc.parentAxisLocal) ||
        !(length_squared(desc.parentAxisLocal) > 1.0e-12F) || !finite(desc.referenceRotation) ||
        !finite(desc.minimumRadians) || !finite(desc.maximumRadians) ||
        desc.minimumRadians > desc.maximumRadians || desc.minimumRadians < -kPi ||
        desc.maximumRadians > kPi || !finite(desc.swingLimitRadians) ||
        desc.swingLimitRadians < 0.0F || desc.swingLimitRadians > kPi) return false;
    const float normSquared = desc.referenceRotation.x * desc.referenceRotation.x +
        desc.referenceRotation.y * desc.referenceRotation.y +
        desc.referenceRotation.z * desc.referenceRotation.z +
        desc.referenceRotation.w * desc.referenceRotation.w;
    return std::isfinite(normSquared) && normSquared > 1.0e-12F;
}


bool validate_static_rigid_body_desc(const StaticRigidBodyCreateDesc& desc) noexcept {
    if (desc.boxes.empty() || !finite(desc.transform)) return false;
    const double rotationNormSquared =
        static_cast<double>(desc.transform.rotation.x) * desc.transform.rotation.x +
        static_cast<double>(desc.transform.rotation.y) * desc.transform.rotation.y +
        static_cast<double>(desc.transform.rotation.z) * desc.transform.rotation.z +
        static_cast<double>(desc.transform.rotation.w) * desc.transform.rotation.w;
    if (!(rotationNormSquared > 1.0e-20) || !finite(rotationNormSquared)) return false;
    for (const SolverBox& box : desc.boxes) {
        if (!finite(box.center) || !finite(box.halfExtents) || !(box.halfExtents.x > 0.0F) ||
            !(box.halfExtents.y > 0.0F) || !(box.halfExtents.z > 0.0F)) return false;
    }
    return true;
}

Float3 inherited_child_center_of_mass_velocity(
    Float3 parentLinearVelocity,
    Float3 parentAngularVelocity,
    Float3 parentCenterOfMassWorld,
    Float3 childCenterOfMassWorld) noexcept {
    return add(
        parentLinearVelocity,
        cross(parentAngularVelocity, subtract(childCenterOfMassWorld, parentCenterOfMassWorld)));
}

std::optional<RigidBodyCreateDesc> make_rigid_body_desc(
    const FragmentSolverPackage& package,
    const FragmentSolverScale& scale,
    Float3 inheritedLinearVelocity,
    Float3 inheritedAngularVelocity) {
    if (!package.mass.exactWithinLimits || package.mass.massUnits == 0U || package.boxes.empty() ||
        !(scale.kilogramsPerDensityUnit > 0.0) || !(scale.metersPerVoxel > 0.0) ||
        !finite(scale.kilogramsPerDensityUnit) || !finite(scale.metersPerVoxel) ||
        !finite(inheritedLinearVelocity) || !finite(inheritedAngularVelocity)) return std::nullopt;

    const double massScale = scale.kilogramsPerDensityUnit;
    const double inertiaScale = massScale * scale.metersPerVoxel * scale.metersPerVoxel;
    const float meterScale = static_cast<float>(scale.metersPerVoxel);
    RigidBodyCreateDesc desc;
    desc.transform = package.bodyTransform;
    // FragmentSolverPackage stores all translations in voxel units, including the body-frame
    // world position. Convert that position here, at the same boundary that already converts
    // box centers/extents and inertia. Leaving it unscaled forced callers to post-correct the
    // descriptor and made moving-split velocity calculations mix meters with voxels.
    desc.transform.position = multiply(desc.transform.position, meterScale);
    desc.massKilograms = static_cast<double>(package.mass.massUnits) * massScale;
    desc.inertiaKilogramMetersSquared = {
        package.mass.inertiaAboutCenter.xx * inertiaScale,
        package.mass.inertiaAboutCenter.yy * inertiaScale,
        package.mass.inertiaAboutCenter.zz * inertiaScale,
        package.mass.inertiaAboutCenter.xy * inertiaScale,
        package.mass.inertiaAboutCenter.xz * inertiaScale,
        package.mass.inertiaAboutCenter.yz * inertiaScale,
    };
    desc.linearVelocity = inheritedLinearVelocity;
    desc.angularVelocity = inheritedAngularVelocity;
    desc.boxes.reserve(package.boxes.size());
    for (const SolverBox& source : package.boxes) {
        if (!(source.halfExtents.x > 0.0F) || !(source.halfExtents.y > 0.0F) ||
            !(source.halfExtents.z > 0.0F)) return std::nullopt;
        desc.boxes.push_back({multiply(source.center, meterScale), multiply(source.halfExtents, meterScale)});
    }
    if (!validate_rigid_body_desc(desc)) return std::nullopt;
    return desc;
}

std::optional<RigidBodyCreateDesc> make_moving_split_rigid_body_desc(
    const FragmentSolverPackage& childPackage,
    const FragmentSolverScale& scale,
    const RigidBodyState& parentState) {
    if (!rigid_body_state_is_finite(parentState)) return std::nullopt;
    std::optional<RigidBodyCreateDesc> desc = make_rigid_body_desc(childPackage, scale);
    if (!desc) return std::nullopt;
    desc->linearVelocity = inherited_child_center_of_mass_velocity(
        parentState.linearVelocity,
        parentState.angularVelocity,
        parentState.currentTransform.position,
        desc->transform.position);
    desc->angularVelocity = parentState.angularVelocity;
    if (!validate_rigid_body_desc(*desc)) return std::nullopt;
    return desc;
}

std::vector<RigidBodyHandle> IRigidBodyWorld::create_bodies(
    std::span<const RigidBodyCreateDesc> descs) {
    std::vector<RigidBodyHandle> handles;
    handles.reserve(descs.size());
    for (const RigidBodyCreateDesc& desc : descs) {
        const RigidBodyHandle handle = create_body(desc);
        if (handle == kInvalidRigidBodyHandle) {
            for (auto it = handles.rbegin(); it != handles.rend(); ++it) (void)destroy_body(*it);
            return {};
        }
        handles.push_back(handle);
    }
    return handles;
}

RigidBodyHandle IRigidBodyWorld::create_static_body(const StaticRigidBodyCreateDesc&) {
    return kInvalidRigidBodyHandle;
}

std::vector<RigidBodyHandle> IRigidBodyWorld::create_static_bodies(
    std::span<const StaticRigidBodyCreateDesc> descs) {
    std::vector<RigidBodyHandle> handles;
    handles.reserve(descs.size());
    for (const StaticRigidBodyCreateDesc& desc : descs) {
        const RigidBodyHandle handle = create_static_body(desc);
        if (handle == kInvalidRigidBodyHandle) {
            for (auto it = handles.rbegin(); it != handles.rend(); ++it) (void)destroy_body(*it);
            return {};
        }
        handles.push_back(handle);
    }
    return handles;
}

bool IRigidBodyWorld::destroy_bodies(std::span<const RigidBodyHandle> handles) {
    bool success = true;
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        success = destroy_body(*it) && success;
    }
    return success;
}

RigidBodyConstraintHandle IRigidBodyWorld::create_constraint(const RigidBodyConstraintDesc&) {
    return kInvalidRigidBodyConstraintHandle;
}

bool IRigidBodyWorld::destroy_constraint(RigidBodyConstraintHandle) { return false; }

bool IRigidBodyWorld::destroy_constraints(std::span<const RigidBodyConstraintHandle> handles) {
    bool success = true;
    for (auto it = handles.rbegin(); it != handles.rend(); ++it)
        success = destroy_constraint(*it) && success;
    return success;
}

RigidBodyHandle ReferenceRigidBodyWorld::create_body(const RigidBodyCreateDesc& desc) {
    const RigidBodyValidationResult validation = validate_rigid_body_desc(desc);
    if (!validation) return kInvalidRigidBodyHandle;

    Body body;
    body.alive = true;
    body.isStatic = false;
    body.desc = desc;
    body.desc.transform = validation.normalizedTransform;
    body.state.previousTransform = validation.normalizedTransform;
    body.state.currentTransform = validation.normalizedTransform;
    body.state.linearVelocity = desc.linearVelocity;
    body.state.angularVelocity = desc.angularVelocity;

    if (!freeHandles_.empty()) {
        const RigidBodyHandle handle = freeHandles_.back();
        freeHandles_.pop_back();
        bodies_[handle] = std::move(body);
        return handle;
    }
    if (bodies_.size() >= static_cast<std::size_t>(kInvalidRigidBodyHandle)) {
        return kInvalidRigidBodyHandle;
    }
    bodies_.push_back(std::move(body));
    return static_cast<RigidBodyHandle>(bodies_.size() - 1U);
}

std::vector<RigidBodyHandle> ReferenceRigidBodyWorld::create_bodies(
    std::span<const RigidBodyCreateDesc> descs) {
    return IRigidBodyWorld::create_bodies(descs);
}

RigidBodyHandle ReferenceRigidBodyWorld::create_static_body(
    const StaticRigidBodyCreateDesc& desc) {
    if (!validate_static_rigid_body_desc(desc)) return kInvalidRigidBodyHandle;
    Body body;
    body.alive = true;
    body.isStatic = true;
    body.staticDesc = desc;
    body.staticDesc.transform = make_rigid_transform(desc.transform.position, desc.transform.rotation);
    body.state.previousTransform = body.staticDesc.transform;
    body.state.currentTransform = body.staticDesc.transform;
    body.state.sleeping = true;

    if (!freeHandles_.empty()) {
        const RigidBodyHandle handle = freeHandles_.back();
        freeHandles_.pop_back();
        bodies_[handle] = std::move(body);
        return handle;
    }
    if (bodies_.size() >= static_cast<std::size_t>(kInvalidRigidBodyHandle)) {
        return kInvalidRigidBodyHandle;
    }
    bodies_.push_back(std::move(body));
    return static_cast<RigidBodyHandle>(bodies_.size() - 1U);
}

std::vector<RigidBodyHandle> ReferenceRigidBodyWorld::create_static_bodies(
    std::span<const StaticRigidBodyCreateDesc> descs) {
    return IRigidBodyWorld::create_static_bodies(descs);
}

bool ReferenceRigidBodyWorld::destroy_body(RigidBodyHandle handle) {
    if (handle >= bodies_.size() || !bodies_[handle].alive) return false;
    for (RigidBodyConstraintHandle constraintHandle = 0U;
         constraintHandle < constraints_.size(); ++constraintHandle) {
        Constraint& constraint = constraints_[constraintHandle];
        if (!constraint.alive || (constraint.desc.parentBody != handle && constraint.desc.childBody != handle)) continue;
        constraint = {};
        freeConstraintHandles_.push_back(constraintHandle);
    }
    bodies_[handle] = {};
    freeHandles_.push_back(handle);
    return true;
}

std::optional<RigidBodyState> ReferenceRigidBodyWorld::state(RigidBodyHandle handle) const {
    if (handle >= bodies_.size() || !bodies_[handle].alive) return std::nullopt;
    return bodies_[handle].state;
}

bool ReferenceRigidBodyWorld::set_state(RigidBodyHandle handle, const RigidBodyState& stateValue) {
    if (handle >= bodies_.size() || !bodies_[handle].alive ||
        !rigid_body_state_is_finite(stateValue)) return false;
    bodies_[handle].state = stateValue;
    bodies_[handle].state.previousTransform = make_rigid_transform(
        stateValue.previousTransform.position, stateValue.previousTransform.rotation);
    bodies_[handle].state.currentTransform = make_rigid_transform(
        stateValue.currentTransform.position, stateValue.currentTransform.rotation);
    if (bodies_[handle].isStatic) {
        bodies_[handle].state.linearVelocity = {};
        bodies_[handle].state.angularVelocity = {};
        bodies_[handle].state.sleeping = true;
        bodies_[handle].staticDesc.transform = bodies_[handle].state.currentTransform;
    }
    bodies_[handle].quietSteps = 0U;
    return true;
}

bool ReferenceRigidBodyWorld::apply_impulse(RigidBodyHandle handle, Float3 worldImpulse) {
    if (handle >= bodies_.size() || !bodies_[handle].alive || bodies_[handle].isStatic) return false;
    if (!std::isfinite(worldImpulse.x) || !std::isfinite(worldImpulse.y) ||
        !std::isfinite(worldImpulse.z)) return false;
    const double mass = bodies_[handle].desc.massKilograms;
    if (!(mass > 0.0)) return false;
    Body& body = bodies_[handle];
    body.state.linearVelocity = add(
        body.state.linearVelocity, multiply(worldImpulse, static_cast<float>(1.0 / mass)));
    body.state.sleeping = false;
    body.quietSteps = 0U;
    return true;
}

bool ReferenceRigidBodyWorld::apply_force(RigidBodyHandle handle, Float3 worldForce) {
    // The deterministic reference world has no force accumulator. Interpret one call as one
    // 60 Hz step of force; callers sustain a force by issuing it every simulation step.
    constexpr float kReferenceFixedStep = 1.0F / 60.0F;
    return apply_impulse(handle, multiply(worldForce, kReferenceFixedStep));
}

bool ReferenceRigidBodyWorld::apply_angular_impulse(
    RigidBodyHandle handle, Float3 worldAngularImpulse) {
    if (handle >= bodies_.size() || !bodies_[handle].alive || bodies_[handle].isStatic ||
        !finite(worldAngularImpulse)) return false;
    const auto& inertia = bodies_[handle].desc.inertiaKilogramMetersSquared;
    const double determinant =
        inertia.xx * (inertia.yy * inertia.zz - inertia.yz * inertia.yz) -
        inertia.xy * (inertia.xy * inertia.zz - inertia.xz * inertia.yz) +
        inertia.xz * (inertia.xy * inertia.yz - inertia.xz * inertia.yy);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-18) return false;
    const double inv00 = (inertia.yy * inertia.zz - inertia.yz * inertia.yz) / determinant;
    const double inv01 = (inertia.xz * inertia.yz - inertia.xy * inertia.zz) / determinant;
    const double inv02 = (inertia.xy * inertia.yz - inertia.xz * inertia.yy) / determinant;
    const double inv11 = (inertia.xx * inertia.zz - inertia.xz * inertia.xz) / determinant;
    const double inv12 = (inertia.xy * inertia.xz - inertia.xx * inertia.yz) / determinant;
    const double inv22 = (inertia.xx * inertia.yy - inertia.xy * inertia.xy) / determinant;
    const Float3 delta{
        static_cast<float>(inv00 * worldAngularImpulse.x + inv01 * worldAngularImpulse.y + inv02 * worldAngularImpulse.z),
        static_cast<float>(inv01 * worldAngularImpulse.x + inv11 * worldAngularImpulse.y + inv12 * worldAngularImpulse.z),
        static_cast<float>(inv02 * worldAngularImpulse.x + inv12 * worldAngularImpulse.y + inv22 * worldAngularImpulse.z)};
    Body& body = bodies_[handle];
    body.state.angularVelocity = add(body.state.angularVelocity, delta);
    body.state.sleeping = false;
    body.quietSteps = 0U;
    return true;
}

bool ReferenceRigidBodyWorld::apply_torque(RigidBodyHandle handle, Float3 worldTorque) {
    constexpr float kReferenceFixedStep = 1.0F / 60.0F;
    return apply_angular_impulse(handle, multiply(worldTorque, kReferenceFixedStep));
}

bool ReferenceRigidBodyWorld::apply_impulse_at_point(
    RigidBodyHandle handle, Float3 worldImpulse, Float3 worldPoint) {
    if (handle >= bodies_.size() || !bodies_[handle].alive || bodies_[handle].isStatic ||
        !finite(worldImpulse) || !finite(worldPoint)) return false;
    const Float3 lever = subtract(worldPoint, bodies_[handle].state.currentTransform.position);
    const Float3 angularImpulse{
        lever.y * worldImpulse.z - lever.z * worldImpulse.y,
        lever.z * worldImpulse.x - lever.x * worldImpulse.z,
        lever.x * worldImpulse.y - lever.y * worldImpulse.x};
    return apply_impulse(handle, worldImpulse) && apply_angular_impulse(handle, angularImpulse);
}

bool ReferenceRigidBodyWorld::apply_force_at_point(
    RigidBodyHandle handle, Float3 worldForce, Float3 worldPoint) {
    constexpr float kReferenceFixedStep = 1.0F / 60.0F;
    return apply_impulse_at_point(handle, multiply(worldForce, kReferenceFixedStep), worldPoint);
}

std::vector<RigidBodyQueryHit> ReferenceRigidBodyWorld::ray_cast_all(
    Float3 origin,
    Float3 direction,
    float maximumDistance,
    const RigidBodyQueryFilter& filter) const {
    std::vector<RigidBodyQueryHit> hits;
    if (!finite(origin) || !finite(direction) || !finite(maximumDistance) ||
        !(maximumDistance > 0.0F) || !(length_squared(direction) > 1.0e-12F)) return hits;
    direction = normalize(direction);
    for (RigidBodyHandle handle = 0U; handle < bodies_.size(); ++handle) {
        const Body& body = bodies_[handle];
        if (!body.alive || ignored(handle, filter) ||
            (body.isStatic && !filter.includeStatic) ||
            (!body.isStatic && !filter.includeDynamic)) continue;
        const auto& boxes = body.isStatic ? body.staticDesc.boxes : body.desc.boxes;
        const RigidTransform& transform = body.state.currentTransform;
        for (std::size_t index = 0U; index < boxes.size(); ++index) {
            float distance = 0.0F;
            Float3 normal{};
            if (!ray_aabb(origin, direction, maximumDistance,
                    world_aabb(transform, boxes[index]), distance, normal)) continue;
            hits.push_back({handle, add(origin, multiply(direction, distance)), normal,
                distance / maximumDistance, distance, body.material,
                static_cast<std::uint32_t>(index)});
        }
    }
    sort_query_hits(hits);
    return hits;
}

std::vector<RigidBodyQueryHit> ReferenceRigidBodyWorld::overlap_aabb(
    RigidBodyWorldBounds bounds,
    const RigidBodyQueryFilter& filter) const {
    std::vector<RigidBodyQueryHit> hits;
    if (!finite(bounds.minimum) || !finite(bounds.maximum) ||
        bounds.minimum.x > bounds.maximum.x || bounds.minimum.y > bounds.maximum.y ||
        bounds.minimum.z > bounds.maximum.z) return hits;
    for (RigidBodyHandle handle = 0U; handle < bodies_.size(); ++handle) {
        const Body& body = bodies_[handle];
        if (!body.alive || ignored(handle, filter) ||
            (body.isStatic && !filter.includeStatic) ||
            (!body.isStatic && !filter.includeDynamic)) continue;
        const auto& boxes = body.isStatic ? body.staticDesc.boxes : body.desc.boxes;
        const RigidTransform& transform = body.state.currentTransform;
        for (std::size_t index = 0U; index < boxes.size(); ++index) {
            const QueryAabb boxBounds = world_aabb(transform, boxes[index]);
            if (!overlaps(boxBounds, bounds)) continue;
            const Float3 center = multiply(add(boxBounds.minimum, boxBounds.maximum), 0.5F);
            hits.push_back({handle, center, {}, 0.0F, 0.0F, body.material,
                static_cast<std::uint32_t>(index)});
        }
    }
    sort_query_hits(hits);
    return hits;
}

std::vector<RigidBodyQueryHit> ReferenceRigidBodyWorld::cast_sphere_all(
    Float3 origin,
    float radius,
    Float3 direction,
    float maximumDistance,
    const RigidBodyQueryFilter& filter) const {
    std::vector<RigidBodyQueryHit> hits;
    if (!finite(origin) || !finite(direction) || !finite(radius) || radius < 0.0F ||
        !finite(maximumDistance) || !(maximumDistance > 0.0F) ||
        !(length_squared(direction) > 1.0e-12F)) return hits;
    direction = normalize(direction);
    for (RigidBodyHandle handle = 0U; handle < bodies_.size(); ++handle) {
        const Body& body = bodies_[handle];
        if (!body.alive || ignored(handle, filter) ||
            (body.isStatic && !filter.includeStatic) ||
            (!body.isStatic && !filter.includeDynamic)) continue;
        const auto& boxes = body.isStatic ? body.staticDesc.boxes : body.desc.boxes;
        const RigidTransform& transform = body.state.currentTransform;
        for (std::size_t index = 0U; index < boxes.size(); ++index) {
            float distance = 0.0F;
            Float3 normal{};
            if (!ray_aabb(origin, direction, maximumDistance,
                    world_aabb(transform, boxes[index], radius), distance, normal)) continue;
            hits.push_back({handle, add(origin, multiply(direction, distance)), normal,
                distance / maximumDistance, distance, body.material,
                static_cast<std::uint32_t>(index)});
        }
    }
    sort_query_hits(hits);
    return hits;
}

bool ReferenceRigidBodyWorld::set_contact_material(
    RigidBodyHandle handle, std::uint16_t material) noexcept {
    if (handle >= bodies_.size() || !bodies_[handle].alive) return false;
    bodies_[handle].material = material;
    return true;
}

RigidBodyConstraintHandle ReferenceRigidBodyWorld::create_constraint(
    const RigidBodyConstraintDesc& desc) {
    if (!validate_rigid_body_constraint_desc(desc) || desc.parentBody >= bodies_.size() ||
        desc.childBody >= bodies_.size() || !bodies_[desc.parentBody].alive ||
        !bodies_[desc.childBody].alive || bodies_[desc.parentBody].isStatic ||
        bodies_[desc.childBody].isStatic) return kInvalidRigidBodyConstraintHandle;
    Constraint constraint;
    constraint.alive = true;
    constraint.desc = desc;
    constraint.desc.parentAxisLocal = normalize(desc.parentAxisLocal);
    constraint.desc.referenceRotation = normalize(desc.referenceRotation);
    if (!freeConstraintHandles_.empty()) {
        const RigidBodyConstraintHandle handle = freeConstraintHandles_.back();
        freeConstraintHandles_.pop_back();
        constraints_[handle] = std::move(constraint);
        return handle;
    }
    if (constraints_.size() >= static_cast<std::size_t>(kInvalidRigidBodyConstraintHandle))
        return kInvalidRigidBodyConstraintHandle;
    constraints_.push_back(std::move(constraint));
    return static_cast<RigidBodyConstraintHandle>(constraints_.size() - 1U);
}

bool ReferenceRigidBodyWorld::destroy_constraint(RigidBodyConstraintHandle handle) {
    if (handle >= constraints_.size() || !constraints_[handle].alive) return false;
    constraints_[handle] = {};
    freeConstraintHandles_.push_back(handle);
    return true;
}

void ReferenceRigidBodyWorld::step(float fixedDeltaSeconds) {
    if (!(fixedDeltaSeconds > 0.0F) || !std::isfinite(fixedDeltaSeconds)) return;
    for (Body& body : bodies_) {
        if (!body.alive || body.isStatic || body.state.sleeping) continue;
        body.state.previousTransform = body.state.currentTransform;
        body.state.linearVelocity = add(body.state.linearVelocity, multiply(gravity_, fixedDeltaSeconds));
        body.state.currentTransform.position = add(
            body.state.currentTransform.position,
            multiply(body.state.linearVelocity, fixedDeltaSeconds));

        const float angularSpeed = speed(body.state.angularVelocity);
        if (angularSpeed > 0.0F) {
            const float halfAngle = 0.5F * angularSpeed * fixedDeltaSeconds;
            const float sine = std::sin(halfAngle);
            const Float3 axis = multiply(body.state.angularVelocity, 1.0F / angularSpeed);
            const Quaternion delta{axis.x * sine, axis.y * sine, axis.z * sine, std::cos(halfAngle)};
            body.state.currentTransform.rotation = normalize(multiply(delta, body.state.currentTransform.rotation));
        }

        if (body.desc.allowSleeping && speed(body.state.linearVelocity) <= sleepLinearSpeed_ &&
            speed(body.state.angularVelocity) <= sleepAngularSpeed_) {
            ++body.quietSteps;
            if (body.quietSteps >= sleepQuietSteps_) {
                body.state.sleeping = true;
                body.state.linearVelocity = {};
                body.state.angularVelocity = {};
            }
        } else {
            body.quietSteps = 0U;
        }
    }
    constexpr std::size_t kConstraintIterations = 6U;
    for (std::size_t iteration = 0U; iteration < kConstraintIterations; ++iteration) {
        for (const Constraint& constraint : constraints_) {
            if (!constraint.alive) continue;
            Body& parent = bodies_[constraint.desc.parentBody];
            Body& child = bodies_[constraint.desc.childBody];
            if (!parent.alive || !child.alive) continue;
            const Float3 parentAnchor = transform_point(
                parent.state.currentTransform, constraint.desc.parentAnchorLocal);
            const Float3 childAnchor = transform_point(
                child.state.currentTransform, constraint.desc.childAnchorLocal);
            const Float3 error = subtract(childAnchor, parentAnchor);
            const Float3 halfCorrection = multiply(error, 0.5F);
            parent.state.currentTransform.position = add(
                parent.state.currentTransform.position, halfCorrection);
            child.state.currentTransform.position = subtract(
                child.state.currentTransform.position, halfCorrection);
            if (length_squared(error) > 1.0e-8F) {
                parent.state.sleeping = false;
                child.state.sleeping = false;
                parent.quietSteps = child.quietSteps = 0U;
            }

            if (constraint.desc.kind == RigidBodyConstraintKind::Ball) continue;
            const Quaternion parentRotation = normalize(parent.state.currentTransform.rotation);
            const Quaternion childRotation = normalize(child.state.currentTransform.rotation);
            const Quaternion relative = normalize(multiply(conjugate(parentRotation), childRotation));
            const Quaternion delta = normalize(multiply(
                conjugate(constraint.desc.referenceRotation), relative));
            const Float3 vector{delta.x, delta.y, delta.z};
            const float vectorLength = std::sqrt(length_squared(vector));
            const float angle = 2.0F * std::atan2(vectorLength, std::max(0.000001F, delta.w));
            Quaternion desiredRelative = constraint.desc.referenceRotation;
            if (constraint.desc.kind == RigidBodyConstraintKind::Hinge) {
                const Float3 deltaAxis = vectorLength > 1.0e-6F
                    ? multiply(vector, 1.0F / vectorLength) : constraint.desc.parentAxisLocal;
                const float sign = dot(deltaAxis, constraint.desc.parentAxisLocal) < 0.0F ? -1.0F : 1.0F;
                const float clamped = std::clamp(angle * sign,
                    constraint.desc.minimumRadians, constraint.desc.maximumRadians);
                desiredRelative = normalize(multiply(
                    constraint.desc.referenceRotation,
                    axis_angle(constraint.desc.parentAxisLocal, clamped)));
            } else if (constraint.desc.kind == RigidBodyConstraintKind::ConeTwist &&
                       angle > constraint.desc.swingLimitRadians && vectorLength > 1.0e-6F) {
                desiredRelative = normalize(multiply(
                    constraint.desc.referenceRotation,
                    axis_angle(multiply(vector, 1.0F / vectorLength), constraint.desc.swingLimitRadians)));
            }
            child.state.currentTransform.rotation = normalize(multiply(parentRotation, desiredRelative));
        }
    }
}

void ReferenceRigidBodyWorld::set_sleep_thresholds(
    float linearSpeed,
    float angularSpeed,
    std::uint32_t quietSteps) noexcept {
    sleepLinearSpeed_ = std::max(0.0F, linearSpeed);
    sleepAngularSpeed_ = std::max(0.0F, angularSpeed);
    sleepQuietSteps_ = std::max(1U, quietSteps);
}

RigidTransform ReferenceRigidBodyWorld::interpolated_transform(
    RigidBodyHandle handle,
    float alpha) const {
    const auto current = state(handle);
    if (!current) return {};
    return interpolate_rigid_transform(
        current->previousTransform,
        current->currentTransform,
        std::clamp(alpha, 0.0F, 1.0F));
}

RigidBodyCounts ReferenceRigidBodyWorld::body_counts() const noexcept {
    RigidBodyCounts counts;
    for (const Body& body : bodies_) {
        if (!body.alive) continue;
        ++counts.total;
        if (body.isStatic) {
            ++counts.staticBodies;
            continue;
        }
        ++counts.dynamicBodies;
        if (body.state.sleeping) ++counts.sleepingDynamicBodies;
        else ++counts.awakeDynamicBodies;
    }
    return counts;
}

std::size_t ReferenceRigidBodyWorld::body_count() const noexcept { return body_counts().total; }
std::size_t ReferenceRigidBodyWorld::dynamic_body_count() const noexcept { return body_counts().dynamicBodies; }
std::size_t ReferenceRigidBodyWorld::awake_body_count() const noexcept { return body_counts().awakeDynamicBodies; }
std::size_t ReferenceRigidBodyWorld::sleeping_body_count() const noexcept { return body_counts().sleepingDynamicBodies; }
std::size_t ReferenceRigidBodyWorld::constraint_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(constraints_.begin(), constraints_.end(),
        [](const Constraint& constraint) { return constraint.alive; }));
}

} // namespace dve
