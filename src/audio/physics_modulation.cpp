// Physics modulation implementation.
#include "dve/audio/physics_modulation.hpp"

#include <algorithm>
#include <cmath>

namespace dve::audio {

float SpringModulator::step(float dt) noexcept {
    // Semi-implicit Euler (stable).
    const float accel = -stiffness * position - damping * velocity;
    velocity += accel * dt;
    position += velocity * dt;
    // Soft clamp to prevent blowup.
    position = std::clamp(position, -2.0F, 2.0F);
    return std::clamp(position, -1.0F, 1.0F);
}

float PendulumModulator::step(float dt) noexcept {
    // Nonlinear pendulum: theta'' = -(g/L)*sin(theta) - damping*theta'
    const float accel = -(gravity / length) * std::sin(angle) - damping * angularVelocity;
    angularVelocity += accel * dt;
    angle += angularVelocity * dt;
    // Wrap angle to [-pi, pi] for output stability.
    while (angle > 3.14159265F) angle -= 6.2831853F;
    while (angle < -3.14159265F) angle += 6.2831853F;
    return std::sin(angle);
}

float OrbiterModulator::step(float dt) noexcept {
    // Central force: a = -M * r / |r|^3
    const float r2 = x*x + y*y + 1e-6F;
    const float r = std::sqrt(r2);
    const float force = -centralMass / (r2 * r);
    vx += force * x * dt - damping * vx * dt;
    vy += force * y * dt - damping * vy * dt;
    x += vx * dt;
    y += vy * dt;
    // Normalize output by initial radius.
    return std::clamp(x * 0.5F, -1.0F, 1.0F);
}

float LorenzModulator::step(float dt) noexcept {
    const float sdt = dt * speed;
    // RK2 for stability.
    const float dx1 = sigma * (y - x);
    const float dy1 = x * (rho - z) - y;
    const float dz1 = x * y - beta * z;
    const float mx = x + dx1 * sdt * 0.5F;
    const float my = y + dy1 * sdt * 0.5F;
    const float mz = z + dz1 * sdt * 0.5F;
    x += sigma * (my - mx) * sdt;
    y += (mx * (rho - mz) - my) * sdt;
    z += (mx * my - beta * mz) * sdt;
    // Lorenz x ranges roughly +/-20; normalize.
    return std::clamp(x / 20.0F, -1.0F, 1.0F);
}

void PhysicsModulationBank::step(float dt) noexcept {
    spring.step(dt);
    pendulum.step(dt);
    orbiter.step(dt);
    lorenz.step(dt);
}

void PhysicsModulationBank::note_on(float velocity) noexcept {
    const float v = std::clamp(velocity, 0.0F, 1.0F);
    spring.excite(v * 8.0F);
    pendulum.excite(v * 6.0F);
    orbiter.excite(v * 0.5F, v * 0.3F);
    // Lorenz runs continuously (no excitation needed).
}

void PhysicsModulationBank::reset() noexcept {
    spring.reset();
    pendulum.reset();
    orbiter.reset();
    lorenz.reset();
}

} // namespace dve::audio
