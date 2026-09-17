#pragma once

#include "dve/physics2d.hpp"

#include <memory>

namespace dve {

// Internal factory separated from the public backend-neutral factory so Box2D headers do not leak
// into engine or game code. This symbol exists only when DVE_HAVE_BOX2D is defined.
[[nodiscard]] std::unique_ptr<Physics2DWorld> create_box2d_physics_world(
    const Physics2DWorldSettings& settings, std::string* error = nullptr);

} // namespace dve
