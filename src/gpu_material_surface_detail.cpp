#include "dve/gpu_material_surface_detail.hpp"
#include "dve/transform.hpp"
namespace dve {
GpuMaterialSurfaceDetail pack_gpu_material_surface_detail(const MaterialSurfaceDetail& d) noexcept {
    GpuMaterialSurfaceDetail r;
    if(!material_surface_detail_valid(d))return r;
    r.pattern=static_cast<std::uint32_t>(d.pattern);r.space=static_cast<std::uint32_t>(d.space);r.seed=d.seed;
    r.periodMeters=d.periodMeters;
    const auto q=quaternion_from_euler_xyz(d.rotationRadians);
    r.rotationX=q.x;r.rotationY=q.y;r.rotationZ=q.z;r.rotationW=q.w;
    r.grainStretch=d.grainStretch;r.colorVariation=d.colorVariation;r.roughnessVariation=d.roughnessVariation;
    r.bumpHeightMeters=d.bumpHeightMeters;r.uvTileMeters=d.uvTileMeters;
    return r;
}
}
