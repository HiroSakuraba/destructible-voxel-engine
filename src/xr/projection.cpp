#include "dve/xr/projection.hpp"
#include <cmath>
namespace dve::xr {
bool vulkan_projection(const XrFovf& fov,float nearPlane,float farPlane,
                       std::array<float,16>& out,std::string& error) {
    out={}; error.clear();
    constexpr float halfPi=1.5707963267948966f;
    const auto valid=[](float a) { return std::isfinite(a) && a>-halfPi && a<halfPi; };
    if (!valid(fov.angleLeft) || !valid(fov.angleRight) || !valid(fov.angleUp) || !valid(fov.angleDown) ||
        !std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane<=0 || farPlane<=nearPlane) {
        error="Invalid OpenXR field of view or clipping planes"; return false;
    }
    const float l=std::tan(fov.angleLeft),r=std::tan(fov.angleRight);
    const float b=std::tan(fov.angleDown),t=std::tan(fov.angleUp);
    const float width=r-l,height=t-b,depth=farPlane-nearPlane;
    if (!(width>1e-6f) || !(height>1e-6f) || !(depth>1e-6f)) {
        error="Degenerate OpenXR projection frustum"; return false;
    }
    out[0]=2.0f/width; out[5]=-2.0f/height;
    out[8]=(r+l)/width; out[9]=-(t+b)/height;
    out[10]=-farPlane/depth; out[11]=-1.0f; out[14]=-(farPlane/depth)*nearPlane;
    for (float v : out) if (!std::isfinite(v)) { out={}; error="OpenXR projection overflow"; return false; }
    return true;
}
}
