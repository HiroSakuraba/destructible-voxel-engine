#include <X11/Xlib.h>

#include "dve/editor_native.hpp"
#include "dve/game_ui.hpp"
#include "dve/physics_jolt_backend.hpp"
#include "dve/rigid_body_adapter.hpp"
#include "dve/runtime_scene.hpp"
#include "dve/platform/application_host.hpp"
#include "dve/rhi/device.hpp"

int main() {
    using namespace dve;
    static_assert(RigidBodyValidationError::NoError == static_cast<RigidBodyValidationError>(0));
    static_assert(RuntimeSceneErrorCode::NoError == static_cast<RuntimeSceneErrorCode>(0));
    static_assert(dve::ui::GameMenuScreen::Closed == static_cast<dve::ui::GameMenuScreen>(0));
    static_assert(JoltPhysicsUpdateErrorFlag::NoErrors == static_cast<JoltPhysicsUpdateErrorFlag>(0));
    static_assert(platform::EventType::NoEvent == static_cast<platform::EventType>(0));
    static_assert(rhi::BufferUsage::NoUsage == static_cast<rhi::BufferUsage>(0));
    return 0;
}
