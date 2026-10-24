#pragma once
#include "dve/component.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace dve {
class GameWorld;
inline constexpr std::string_view kPrototypePrefix="dve.prototype.";
enum class PrototypePhase : unsigned { Playing,Paused,Won,Lost };
struct PrototypeHud {
    bool active{},debug{},racing{};
    std::string stats;
    std::string vehicleTelemetry;
    PrototypePhase phase{PrototypePhase::Playing};
    float health{},maximumHealth{};
    unsigned collected{},required{};
    std::string prompt,message;
    std::vector<std::string> trace;
};
struct PrototypeCamera { Float3 position{},target{},up{0,0,1};float verticalFovRadians{1.04719755F}; };
struct PrototypeObjectView { std::uint64_t id{};std::string name;std::span<const Component> components;bool marker{}; };
void register_prototype_components(ComponentTypeRegistry&);
[[nodiscard]] std::vector<std::string> prototype_role_types();
[[nodiscard]] std::vector<std::string> prototype_events(std::string_view role);
[[nodiscard]] std::vector<std::string> prototype_actions(std::string_view role);
[[nodiscard]] std::string prototype_object_key(std::span<const Component>);
// Empty scenes without prototype roles remain ordinary engine scenes.
[[nodiscard]] bool validate_prototype_scene(std::span<const PrototypeObjectView>,std::string* error=nullptr);
class PrototypeGame {
public:
    explicit PrototypeGame(GameWorld&);~PrototypeGame();
    PrototypeGame(const PrototypeGame&)=delete;PrototypeGame& operator=(const PrototypeGame&)=delete;
    [[nodiscard]] bool start(std::string* error=nullptr);
    // Owns the world tick only while active; paused/won/lost states deliberately freeze it.
    void step(float dt);
    void look(float yawRadians,float pitchRadians);
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] PrototypeCamera camera() const;
    [[nodiscard]] const PrototypeHud& hud() const noexcept;
    // After a GameWorld save restore, recover kit state and rebind saved trigger IDs.
    [[nodiscard]] bool after_restore(std::string* error=nullptr);
private: struct Impl;std::unique_ptr<Impl> impl_;
};
// Tiny dependency-free HUD renderer used by CPU player readbacks and screenshots.
void draw_prototype_hud(std::span<std::uint8_t> rgba,unsigned width,unsigned height,const PrototypeHud&);
}
