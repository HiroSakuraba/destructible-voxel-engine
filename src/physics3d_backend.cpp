#include "dve/physics3d_backend.hpp"

#include <exception>
#include <utility>

#ifdef DVE_HAVE_BOX3D
#include "dve/physics_box3d_backend.hpp"
#endif
#ifdef DVE_HAVE_JOLT
#include "dve/physics_jolt_backend.hpp"
#endif

namespace dve {

Physics3DBackendAvailability physics3d_backend_availability() noexcept {
    Physics3DBackendAvailability availability;
#ifdef DVE_HAVE_JOLT
    availability.jolt = true;
#endif
#ifdef DVE_HAVE_BOX3D
    availability.box3d = true;
#endif
    return availability;
}

bool physics3d_backend_available(Physics3DBackend backend) noexcept {
    const Physics3DBackendAvailability availability = physics3d_backend_availability();
    switch (backend) {
    case Physics3DBackend::Automatic:
    case Physics3DBackend::Reference:
        return true;
    case Physics3DBackend::Jolt:
        return availability.jolt;
    case Physics3DBackend::Box3D:
        return availability.box3d;
    }
    return false;
}

std::string_view physics3d_backend_label(Physics3DBackend backend) noexcept {
    switch (backend) {
    case Physics3DBackend::Automatic: return "Automatic";
    case Physics3DBackend::Reference: return "Reference";
    case Physics3DBackend::Jolt: return "Jolt";
    case Physics3DBackend::Box3D: return "Box3D";
    }
    return "Unknown";
}

std::unique_ptr<IRigidBodyWorld> create_physics3d_world(
    Physics3DBackend backend,
    Physics3DBackend* resolvedBackend,
    std::string* error,
    Physics3DWorldConfig config) {
    const auto resolve = [&](Physics3DBackend value, std::unique_ptr<IRigidBodyWorld> world) {
        if (resolvedBackend != nullptr) *resolvedBackend = value;
        if (error != nullptr) error->clear();
        return world;
    };
    const auto unavailable = [&](const char* message) -> std::unique_ptr<IRigidBodyWorld> {
        if (error != nullptr) *error = message;
        return {};
    };
#ifdef DVE_HAVE_JOLT
    const auto makeJolt = [&]() -> std::unique_ptr<IRigidBodyWorld> {
        JoltWorldConfig jolt;
        if (config.workerThreads != 0U) jolt.workerThreads = config.workerThreads;
        if (config.subStepCount != 0U) jolt.collisionSteps = config.subStepCount;
        if (config.velocityIterations != 0U) jolt.velocityIterations = config.velocityIterations;
        if (config.positionIterations != 0U) jolt.positionIterations = config.positionIterations;
        jolt.deterministicSimulation = config.deterministicSimulation;
        jolt.allowSleeping = config.allowSleeping;
        return std::make_unique<JoltRigidBodyWorld>(jolt);
    };
#endif
#ifdef DVE_HAVE_BOX3D
    const auto makeBox3D = [&]() -> std::unique_ptr<IRigidBodyWorld> {
        Box3DWorldConfig box3d;
        if (config.workerThreads != 0U) box3d.workerThreads = config.workerThreads;
        if (config.subStepCount != 0U) box3d.subStepCount = config.subStepCount;
        box3d.enableSleeping = config.allowSleeping;
        box3d.enableContinuousCollision = config.continuousCollision;
        return std::make_unique<Box3DRigidBodyWorld>(box3d);
    };
#endif

    try {
        if (backend == Physics3DBackend::Automatic) {
#ifdef DVE_HAVE_JOLT
            return resolve(Physics3DBackend::Jolt, makeJolt());
#elif defined(DVE_HAVE_BOX3D)
            return resolve(Physics3DBackend::Box3D, makeBox3D());
#else
            return resolve(Physics3DBackend::Reference, std::make_unique<ReferenceRigidBodyWorld>());
#endif
        }

        switch (backend) {
        case Physics3DBackend::Reference:
            return resolve(backend, std::make_unique<ReferenceRigidBodyWorld>());
        case Physics3DBackend::Jolt:
#ifdef DVE_HAVE_JOLT
            return resolve(backend, makeJolt());
#else
            return unavailable("Jolt was not enabled in this DVE build");
#endif
        case Physics3DBackend::Box3D:
#ifdef DVE_HAVE_BOX3D
            return resolve(backend, makeBox3D());
#else
            return unavailable("Box3D was not enabled in this DVE build");
#endif
        case Physics3DBackend::Automatic:
            break;
        }
        return unavailable("unknown 3D physics backend");
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return {};
    } catch (...) {
        return unavailable("3D physics backend construction failed");
    }
}

} // namespace dve
