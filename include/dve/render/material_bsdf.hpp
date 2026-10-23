#pragma once

#include "dve/asset_cooker.hpp"
#include "dve/render/surface_shading.hpp"

namespace dve::render {
// Unit-incident-radiance BSDF multiplied by abs(n.wi), and its solid-angle PDF.
// Both directions point away from the interface; view is on the normal's side.
struct MaterialBsdfEvaluation {
    ShadingLobes value;
    float pdf{};
};
struct MaterialBsdfSample {
    Float3 direction{};
    Float3 weight{};       // f * abs(n.wi) / PDF; radiance transport includes eta_i^2/eta_t^2
    Float3 diffuseShare{}; // first-lobe AOV decomposition
    float pdf{};
    bool delta{};
    bool transmitted{};
    bool valid{};
};
// Transmission mixes an exact dielectric interface with the existing opaque GGX/coat BSDF.
// Rough dielectric reflection/transmission use GGX visible-normal sampling. Zero roughness
// is a true delta interface; a smooth full metal is a delta conductor with authored RGB F0.
[[nodiscard]] MaterialBsdfEvaluation evaluate_material_bsdf(const VoxelMaterialDefinition &material,
                                                            Float3 normal, Float3 view, Float3 incoming,
                                                            float etaIncident = 1,
                                                            float etaTransmitted = 1.5F) noexcept;
[[nodiscard]] MaterialBsdfSample sample_material_bsdf(const VoxelMaterialDefinition &material, Float3 normal,
                                                      Float3 view, float etaIncident, float etaTransmitted,
                                                      float selectLobe, float u1, float u2,
                                                      float selectFresnel) noexcept;
} // namespace dve::render
