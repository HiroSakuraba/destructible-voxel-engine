#pragma once

#include "dve/render/voxel_reference_renderer.hpp"
#include <functional>

namespace dve::render {
// Shared CPU integrator. World coordinates use metersPerUnit; authored lights
// use metres. Immutable after construction and safe for simultaneous pixel
// queries.
class SceneLighting {
public:
  SceneLighting(std::span<const VoxelReferenceInstance> voxels,
                std::span<const PolygonRenderInstance> polygons,
                const RenderEnvironment &environment, float metersPerUnit);
  ~SceneLighting();
  SceneLighting(const SceneLighting &) = delete;
  SceneLighting &operator=(const SceneLighting &) = delete;
  [[nodiscard]] float sun_visibility(Float3 point, Float3 normal,
                                     std::uint32_t seed) const;
  [[nodiscard]] ShadingLobes direct(Float3 point, Float3 normal, Float3 view,
                                    const VoxelMaterialDefinition &material,
                                    std::uint32_t seed) const;
  // Incoming multi-bounce transport scattered at the supplied surface. Includes
  // emission reached by continuation, explicit emitters, shadowed sun and
  // escaped environment.
  [[nodiscard]] ShadingLobes
  indirect_lobes(Float3 point, Float3 normal, Float3 view,
                 const VoxelMaterialDefinition &material,
                 std::uint32_t seed) const;
  [[nodiscard]] Float3 indirect(Float3 point, Float3 normal, Float3 view,
                                const VoxelMaterialDefinition &material,
                                std::uint32_t seed) const;
  // Single-scattering emitter estimate, with optional participating-medium
  // transmittance.
  [[nodiscard]] Float3 scattering(
      Float3 point, Float3 viewRay, float anisotropy, std::uint32_t seed,
      const std::function<float(Float3, float)> &transmittance = {}) const;
  [[nodiscard]] std::size_t emitter_count() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
[[nodiscard]] float candle_light_multiplier(const SceneLight &light,
                                            double timeSeconds) noexcept;
// Finite-radius separable HDR bloom, applied once before exposure/tonemap.
// Writes glow to the emission AOV, retaining light_sum == beauty. Returns false
// without mutation on bad input.
[[nodiscard]] bool apply_cpu_bloom(PolygonRenderTarget &target,
                                   const RenderEnvironment &environment);
} // namespace dve::render
