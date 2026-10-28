#include "dve/xr/projection.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
using namespace dve::xr;
static float ndc(const std::array<float,16>& m, int row,float x,float y,float z) {
    float c=m[row]*x+m[4+row]*y+m[8+row]*z+m[12+row];
    float w=m[3]*x+m[7]*y+m[11]*z+m[15]; return c/w;
}
static bool near(float a,float b) { return std::abs(a-b)<1e-5f; }
int main() {
    XrFovf f{}; f.angleLeft=-0.6f; f.angleRight=0.8f; f.angleDown=-0.7f; f.angleUp=0.5f;
    std::array<float,16> m; std::string error;
    assert(vulkan_projection(f,0.1f,100.0f,m,error));
    assert(near(ndc(m,0,std::tan(f.angleLeft),0,-1),-1));
    assert(near(ndc(m,0,std::tan(f.angleRight),0,-1),1));
    assert(near(ndc(m,1,0,std::tan(f.angleUp),-1),-1));
    assert(near(ndc(m,1,0,std::tan(f.angleDown),-1),1));
    assert(near(ndc(m,2,0,0,-0.1f),0)); assert(near(ndc(m,2,0,0,-100),1));
    assert(!vulkan_projection(f,0,100,m,error));
    assert(!vulkan_projection(f,1,1,m,error));
    f.angleRight=f.angleLeft; assert(!vulkan_projection(f,0.1f,100,m,error));
    f.angleRight=std::numeric_limits<float>::quiet_NaN(); assert(!vulkan_projection(f,0.1f,100,m,error));
    for (float x : m) assert(x==0);
    std::cout << "OpenXR Vulkan projection tests passed\n";
}
