#include <cmath>
#include <iostream>
#include <stdexcept>

#include "dve/camera_system.hpp"

namespace {
using namespace dve;
using namespace dve::camera;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float epsilon = 1.0e-3F) {
    return std::abs(a - b) <= epsilon;
}

class WallCollision final : public ICameraCollisionWorld {
public:
    bool sweep_sphere(Float3 start, Float3 end, float radius, CameraCollisionHit& hit) const override {
        (void)radius;
        if ((start.z > 2.0F && end.z <= 2.0F) || (start.z < 2.0F && end.z >= 2.0F)) {
            const float denominator = end.z - start.z;
            if (std::abs(denominator) < 1.0e-6F) return false;
            hit.fraction = (2.0F - start.z) / denominator;
            if (hit.fraction < 0.0F || hit.fraction > 1.0F) return false;
            hit.position = add(start, multiply(subtract(end, start), hit.fraction));
            hit.normal = {0.0F, 0.0F, 1.0F};
            return true;
        }
        return false;
    }
};


class ShoulderCollision final : public ICameraCollisionWorld {
public:
    bool sweep_sphere(Float3 start, Float3 end, float, CameraCollisionHit& hit) const override {
        if (end.x <= start.x) return false;
        hit.fraction = 0.45F;
        hit.position = add(start, multiply(subtract(end, start), hit.fraction));
        hit.normal = {-1.0F, 0.0F, 0.0F};
        return true;
    }
};

class AlwaysCollision final : public ICameraCollisionWorld {
public:
    bool sweep_sphere(Float3 start, Float3 end, float, CameraCollisionHit& hit) const override {
        hit.fraction = 0.35F;
        hit.position = add(start, multiply(subtract(end, start), hit.fraction));
        hit.normal = {0.0F, 0.0F, 1.0F};
        hit.objectId = 4242U;
        hit.materialId = 17U;
        return true;
    }
};

CameraRig make_fixed(CameraRigId id, int priority, Float3 position) {
    CameraRig rig;
    rig.id = id;
    rig.name = "Fixed " + std::to_string(id);
    rig.mode = CameraRigMode::Fixed;
    rig.priority = priority;
    rig.authoredPose.position = position;
    rig.authoredPose.target = {0.0F, 0.0F, 0.0F};
    rig.lens = rig.authoredPose.lens;
    return rig;
}

void test_lens_and_validation() {
    CameraLens lens;
    lens.physical.enabled = true;
    lens.physical.focalLengthMillimeters = 35.0F;
    lens.physical.sensorHeightMillimeters = 24.0F;
    std::string error;
    require(lens.validate(&error), error.c_str());
    require(close(lens.effective_vertical_field_of_view_radians() * kRadiansToDegrees, 37.8493F, 0.02F),
            "physical-lens FOV mismatch");
    lens.nearPlaneMeters = 2.0F;
    lens.farPlaneMeters = 1.0F;
    require(!lens.validate(&error), "invalid clip planes accepted");
}

void test_priority_state_channels_and_blends() {
    CameraDirector director;
    std::string error;
    auto low = make_fixed(1, 1, {0.0F, 0.0F, 5.0F});
    auto high = make_fixed(2, 5, {10.0F, 0.0F, 5.0F});
    high.outputChannels = 2U;
    require(director.add_or_replace_rig(low, &error), error.c_str());
    require(director.add_or_replace_rig(high, &error), error.c_str());
    director.set_default_blend({CameraBlendCurve::Linear, 1.0F});
    director.set_channel_mask(1U);
    auto pose = director.update(0.0F);
    require(director.telemetry().liveRig == 1, "channel mask did not select low rig");
    require(close(pose.position.z, 5.0F), "low camera pose wrong");
    director.set_channel_mask(3U);
    pose = director.update(0.5F);
    require(director.telemetry().liveRig == 2, "priority did not select high rig");
    require(pose.position.x > 4.9F && pose.position.x < 5.1F, "camera blend midpoint wrong");
    pose = director.update(0.5F);
    require(close(pose.position.x, 10.0F), "camera blend did not finish");
    director.bind_state("inspection", 1);
    require(director.set_state("inspection"), "state binding was not found");
    (void)director.update(1.0F);
    require(director.telemetry().liveRig == 1, "state-driven rig did not override priority");
}

void test_third_person_collision_and_shake() {
    CameraDirector director;
    CameraRig rig;
    rig.id = 7;
    rig.name = "Third Person";
    rig.mode = CameraRigMode::ThirdPerson;
    rig.followTarget = 42;
    rig.lookAtTarget = 42;
    rig.framing.distanceMeters = 5.0F;
    rig.framing.heightMeters = 0.0F;
    rig.framing.shoulderOffsetMeters = 0.0F;
    rig.framing.localOffset = {0.0F, 0.0F, 0.0F};
    rig.framing.positionDampingSeconds = 0.0F;
    rig.framing.aimDampingSeconds = 0.0F;
    rig.collision.probeRadiusMeters = 0.1F;
    std::string error;
    require(director.add_or_replace_rig(rig, &error), error.c_str());
    director.set_target(42, {{0.0F,0.0F,0.0F},{0.0F,0.0F,-1.0F},{0.0F,1.0F,0.0F},{}});
    WallCollision wall;
    const CameraPose collided = director.update(0.016F, &wall);
    require(collided.position.z < 2.0F && collided.position.z > 1.5F, "camera collision did not pull camera in");
    require(director.telemetry().collisionCorrections == 1, "collision telemetry missing");

    CameraShake shake;
    shake.id = 10;
    shake.pattern = CameraShakePattern::SineWave;
    shake.positionAmplitudeMeters = {0.2F, 0.0F, 0.0F};
    shake.rotationAmplitudeRadians = {};
    shake.frequencyHertz = 1.0F;
    shake.durationSeconds = 1.0F;
    shake.blendInSeconds = 0.0F;
    shake.blendOutSeconds = 0.0F;
    require(director.start_shake(shake, &error), error.c_str());
    const CameraPose shaken = director.update(0.25F, nullptr);
    const float fullMotion = length(subtract(shaken.position, collided.position));
    require(fullMotion > 0.05F, "camera shake produced no motion");

    CameraDirector reduced;
    require(reduced.add_or_replace_rig(rig, &error), error.c_str());
    reduced.set_target(42, {{0.0F,0.0F,0.0F},{0.0F,0.0F,-1.0F},{0.0F,1.0F,0.0F},{}});
    reduced.set_reduced_motion(true);
    require(reduced.start_shake(shake, &error), error.c_str());
    const CameraPose base = reduced.update(0.0F, nullptr);
    const CameraPose reducedPose = reduced.update(0.25F, nullptr);
    const float reducedMotion = length(subtract(reducedPose.position, base.position));
    require(reducedMotion < fullMotion * 0.35F, "reduced motion did not attenuate camera shake");
}


void test_composer_offsets_and_collision_recovery() {
    CameraDirector director;
    CameraRig rig;
    rig.id = 12;
    rig.name = "Offset Follow";
    rig.mode = CameraRigMode::ThirdPerson;
    rig.followTarget = 1;
    rig.lookAtTarget = 1;
    rig.framing.distanceMeters = 5.0F;
    rig.framing.heightMeters = 0.0F;
    rig.framing.shoulderOffsetMeters = 0.0F;
    rig.framing.localOffset = {};
    rig.framing.screenOffsetX = 0.2F;
    rig.framing.screenOffsetY = 0.1F;
    rig.framing.positionDampingSeconds = 0.0F;
    rig.framing.aimDampingSeconds = 0.0F;
    rig.collision.probeRadiusMeters = 0.1F;
    rig.collision.recoverySeconds = 0.5F;
    std::string error;
    require(director.add_or_replace_rig(rig, &error), error.c_str());
    director.set_target(1, {{0.0F,0.0F,0.0F},{0.0F,0.0F,-1.0F},{0.0F,1.0F,0.0F},{}});
    WallCollision wall;
    const CameraPose blocked = director.update(0.016F, &wall);
    require(blocked.target.x > 0.9F && blocked.target.y > 0.4F, "screen-space framing offsets were ignored");
    const float blockedDistance = length(subtract(blocked.position, blocked.target));
    const CameraPose recovering = director.update(0.05F, nullptr);
    const float recoveringDistance = length(subtract(recovering.position, recovering.target));
    require(recoveringDistance > blockedDistance, "camera did not begin recovering after occlusion");
    require(recoveringDistance < rig.framing.distanceMeters, "camera recovery snapped instead of damping");
    (void)director.update(1.0F, nullptr);
    const CameraPose recovered = director.update(1.0F, nullptr);
    require(length(subtract(recovered.position, recovered.target)) > 4.8F, "camera did not recover to its ideal distance");
}


void test_dead_and_soft_zone_composer() {
    CameraDirector director;
    CameraRig rig;
    rig.id = 13;
    rig.name = "Composer";
    rig.mode = CameraRigMode::ThirdPerson;
    rig.followTarget = 3;
    rig.lookAtTarget = 3;
    rig.framing.distanceMeters = 10.0F;
    rig.framing.localOffset = {};
    rig.framing.heightMeters = 0.0F;
    rig.framing.shoulderOffsetMeters = 0.0F;
    rig.framing.deadZoneFraction = 0.1F;
    rig.framing.softZoneFraction = 0.5F;
    rig.framing.positionDampingSeconds = 0.0F;
    rig.framing.aimDampingSeconds = 0.0F;
    rig.collision.enabled = false;
    std::string error;
    require(director.add_or_replace_rig(rig, &error), error.c_str());
    director.set_target(3, {{0.0F,0.0F,0.0F},{0.0F,0.0F,-1.0F},{0.0F,1.0F,0.0F},{}});
    const CameraPose initial = director.update(0.016F);
    director.set_target(3, {{0.5F,0.0F,0.0F},{0.0F,0.0F,-1.0F},{0.0F,1.0F,0.0F},{}});
    const CameraPose insideDeadZone = director.update(0.016F);
    require(close(insideDeadZone.target.x, initial.target.x), "dead zone moved the composer target");
    director.set_target(3, {{3.0F,0.0F,0.0F},{0.0F,0.0F,-1.0F},{0.0F,1.0F,0.0F},{}});
    const CameraPose insideSoftZone = director.update(0.016F);
    require(insideSoftZone.target.x > initial.target.x && insideSoftZone.target.x < 3.0F,
            "soft zone did not damp target composition");
}


void test_obstruction_strategies_and_volume_constraints() {
    std::string error;
    CameraRig rig;
    rig.id = 21;
    rig.name = "Shoulder Strategy";
    rig.mode = CameraRigMode::ThirdPerson;
    rig.followTarget = 1;
    rig.lookAtTarget = 1;
    rig.framing.distanceMeters = 4.0F;
    rig.framing.heightMeters = 0.0F;
    rig.framing.localOffset = {};
    rig.framing.shoulderOffsetMeters = 0.6F;
    rig.framing.positionDampingSeconds = 0.0F;
    rig.framing.aimDampingSeconds = 0.0F;
    rig.collision.strategy = CameraObstructionStrategy::ShoulderSwap;
    rig.collision.shoulderSwapSearchMeters = 0.6F;
    CameraDirector shoulder;
    require(shoulder.add_or_replace_rig(rig, &error), error.c_str());
    shoulder.set_target(1, {{0,0,0},{0,0,-1},{0,1,0},{}});
    ShoulderCollision selective;
    const CameraPose swapped = shoulder.update(0.016F, &selective);
    require(swapped.position.x < 0.0F, "shoulder-swap obstruction strategy did not move to clear side");
    require(shoulder.telemetry().shoulderSwapActive && shoulder.telemetry().shoulderSwaps == 1,
            "shoulder-swap telemetry missing");

    rig.id = 22;
    rig.name = "Fade Strategy";
    rig.framing.shoulderOffsetMeters = 0.0F;
    rig.collision.strategy = CameraObstructionStrategy::FadeOccluders;
    CameraDirector fade;
    require(fade.add_or_replace_rig(rig, &error), error.c_str());
    fade.set_target(1, {{0,0,0},{0,0,-1},{0,1,0},{}});
    AlwaysCollision blocked;
    const CameraPose faded = fade.update(0.016F, &blocked);
    require(length(subtract(faded.position, faded.target)) > 3.8F,
            "fade-occluders strategy incorrectly pulled the camera forward");
    require(fade.telemetry().occluderFadeActive && fade.telemetry().occluderFadeRequests == 1,
            "fade-occluders telemetry missing");
    require(fade.occluder_fade_requests().size() == 1U,
            "fade-occluders did not publish a renderer request");
    const CameraOccluderFadeRequest& fadeRequest = fade.occluder_fade_requests().front();
    require(fadeRequest.rigId == 22U && fadeRequest.objectId == 4242U && fadeRequest.materialId == 17U,
            "fade-occluders request lost stable scene identity");
    require(close(fadeRequest.targetOpacity, rig.collision.occluderFadeOpacity),
            "fade-occluders request lost target opacity");
    (void)fade.update(0.016F, nullptr);
    require(fade.occluder_fade_requests().empty(),
            "fade requests were not frame-local");

    rig.id = 23;
    rig.name = "Room Constraint";
    rig.collision.enabled = false;
    rig.authoredPose.position = {5.0F, 4.0F, 3.0F};
    rig.mode = CameraRigMode::Fixed;
    rig.followTarget.reset();
    rig.lookAtTarget.reset();
    rig.volumeConstraint.enabled = true;
    rig.volumeConstraint.minimum = {-1.0F, -1.0F, -1.0F};
    rig.volumeConstraint.maximum = {1.0F, 2.0F, 1.0F};
    CameraDirector constrained;
    require(constrained.add_or_replace_rig(rig, &error), error.c_str());
    const CameraPose inside = constrained.update(0.016F);
    require(close(inside.position.x, 1.0F) && close(inside.position.y, 2.0F) && close(inside.position.z, 1.0F),
            "camera room constraint did not clamp the final pose");
    require(constrained.telemetry().volumeConstraintActive, "camera volume telemetry missing");
}

void test_library_round_trip() {
    CameraRigLibrary library;
    library.rigs.push_back(make_fixed(1, 2, {1.0F, 2.0F, 3.0F}));
    CameraRig follow = make_fixed(2, 3, {3.0F, 2.0F, 1.0F});
    follow.mode = CameraRigMode::ThirdPerson;
    follow.followTarget = 99;
    follow.lookAtTarget = 99;
    follow.framing.distanceMeters = 6.0F;
    follow.framing.localOffset = {0.2F, 1.8F, -0.1F};
    follow.framing.lookAheadSeconds = 0.4F;
    follow.framing.screenOffsetX = 0.12F;
    follow.framing.deadZoneFraction = 0.08F;
    follow.collision.preserveLineOfSight = false;
    follow.collision.strategy = CameraObstructionStrategy::FadeOccluders;
    follow.collision.recoverySeconds = 0.6F;
    follow.collision.shoulderSwapSearchMeters = 1.2F;
    follow.collision.occluderFadeOpacity = 0.25F;
    follow.volumeConstraint.enabled = true;
    follow.volumeConstraint.minimum = {-4.0F, -2.0F, -8.0F};
    follow.volumeConstraint.maximum = {4.0F, 8.0F, 2.0F};
    follow.lens.physical.enabled = true;
    follow.lens.physical.focalLengthMillimeters = 52.0F;
    follow.lens.physical.sensorWidthMillimeters = 24.89F;
    follow.lens.physical.lensShiftX = 0.1F;
    follow.lens.physical.gateFit = CameraGateFit::Fill;
    follow.authoredPose.worldUp = {0.0F, 0.9F, 0.1F};
    follow.postProcessWeight = 0.65F;
    library.rigs.push_back(follow);
    library.defaultBlend = {CameraBlendCurve::SmoothStep, 0.7F};
    library.customBlends[{1,2}] = {CameraBlendCurve::Cut, 0.0F};
    library.stateBindings["combat"] = 2;
    std::string error;
    require(library.validate(&error), error.c_str());
    const auto parsed = CameraRigLibrary::parse(library.serialize(), &error);
    require(parsed.has_value(), error.c_str());
    require(parsed->rigs[1].collision.strategy == CameraObstructionStrategy::FadeOccluders,
            "obstruction strategy was not preserved");
    require(close(parsed->rigs[1].collision.occluderFadeOpacity, 0.25F),
            "occluder fade opacity was not preserved");
    require(parsed->rigs[1].volumeConstraint.enabled &&
            close(parsed->rigs[1].volumeConstraint.maximum.y, 8.0F),
            "camera volume constraint was not preserved");
    require(parsed->rigs.size() == 2, "camera library rig count changed");
    require(parsed->rigs[1].followTarget == std::optional<CameraTargetId>(99), "camera target was not preserved");
    require(close(parsed->rigs[1].framing.localOffset.y, 1.8F), "camera framing offset was not preserved");
    require(close(parsed->rigs[1].framing.lookAheadSeconds, 0.4F), "camera look ahead was not preserved");
    require(!parsed->rigs[1].collision.preserveLineOfSight, "collision policy was not preserved");
    require(close(parsed->rigs[1].collision.recoverySeconds, 0.6F), "collision recovery was not preserved");
    require(parsed->rigs[1].lens.physical.enabled, "physical lens flag was not preserved");
    require(close(parsed->rigs[1].lens.physical.focalLengthMillimeters, 52.0F), "focal length was not preserved");
    require(parsed->rigs[1].lens.physical.gateFit == CameraGateFit::Fill, "gate fit was not preserved");
    require(close(parsed->rigs[1].postProcessWeight, 0.65F), "post process weight was not preserved");
    require(parsed->stateBindings.at("combat") == 2, "camera state binding was not preserved");
    require(parsed->customBlends.at({1,2}).curve == CameraBlendCurve::Cut, "custom blend was not preserved");

    const std::string legacy =
        "DVE_CAMERA_LIBRARY 1\n"
        "defaultBlend Linear 0.25\n"
        "rigs 1\n"
        "rig 3 \"Legacy\" Fixed 1 1 1 0 1 5 0 0 0 1.04719758 10 0.05 1000 0 0 0 4.5 0.4 0.45 0.12 0.08 1 0.22 1\n"
        "customBlends 0\n"
        "states 0\n";
    const auto legacyParsed = CameraRigLibrary::parse(legacy, &error);
    require(legacyParsed.has_value() && legacyParsed->rigs.size() == 1, "legacy camera library no longer loads");
}

} // namespace

int main() {
    try {
        test_lens_and_validation();
        test_priority_state_channels_and_blends();
        test_third_person_collision_and_shake();
        test_composer_offsets_and_collision_recovery();
        test_dead_and_soft_zone_composer();
        test_obstruction_strategies_and_volume_constraints();
        test_library_round_trip();
        std::cout << "dve_camera_system_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_camera_system_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
