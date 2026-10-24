#pragma once
#include "dve/prototype_game.hpp"
namespace dve {
// Portable Z-up arcade racer. Geometry is a visual-only car; the runtime owns its
// conservative collision capsule and ground following, independently of Jolt.
struct RacingStatus {
    double elapsed{},lapStarted{},lastLap{},bestLap{},countdown{3};
    unsigned completedLaps{},nextGate{1},targetLaps{3},gateCount{};
    float speedMetersPerSecond{};
};
[[nodiscard]] bool scene_has_racing(std::span<const PrototypeObjectView>);
[[nodiscard]] bool validate_racing_scene(std::span<const PrototypeObjectView>,std::string* error=nullptr);
void register_racing_components(ComponentTypeRegistry&);
[[nodiscard]] bool racing_gate_crossed(Float3 from,Float3 to,const RigidTransform& gate,float halfWidth,float halfHeight);
class RacingGame {
public:
    explicit RacingGame(GameWorld&);~RacingGame();
    [[nodiscard]] bool start(std::string* error=nullptr);
    void step(float dt);
    [[nodiscard]] bool after_restore(std::string* error=nullptr);
    [[nodiscard]] const PrototypeHud& hud() const;
    [[nodiscard]] PrototypeCamera camera() const;
    [[nodiscard]] const RacingStatus& status() const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
