#include "dve/xr/openxr_configuration.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace dve::xr;
static bool bad=false;
static XrResult XRAPI_PTR views(XrInstance,XrSystemId,XrViewConfigurationType type,uint32_t capacity,uint32_t* count,XrViewConfigurationView* out) {
    assert(type==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && capacity==2);*count=2;
    for(uint32_t i=0;i<2;i++){assert(out[i].type==XR_TYPE_VIEW_CONFIGURATION_VIEW);out[i].recommendedImageRectWidth=100+i;out[i].recommendedImageRectHeight=80;out[i].maxImageRectWidth=bad?1:200;out[i].maxImageRectHeight=160;out[i].recommendedSwapchainSampleCount=1;out[i].maxSwapchainSampleCount=4;}return XR_SUCCESS;
}
static XrResult XRAPI_PTR formats(XrSession,uint32_t capacity,uint32_t* count,int64_t* out) {*count=2;if(capacity){assert(capacity==2);out[0]=4;out[1]=7;}return XR_SUCCESS;}
int main(){auto i=reinterpret_cast<XrInstance>(1);auto s=reinterpret_cast<XrSession>(2);std::string error;std::array<XrViewConfigurationView,2> out{};
    assert(query_stereo_view_configuration(i,5,views,out,error) && out[1].recommendedImageRectWidth==101);
    bad=true;assert(!query_stereo_view_configuration(i,5,views,out,error));bad=false;
    int64_t selected=0;assert(choose_swapchain_format(s,formats,{9,7,4},selected,error) && selected==7);
    assert(!choose_swapchain_format(s,formats,{9,8},selected,error) && selected==0);
    assert(!choose_swapchain_format(s,formats,{},selected,error));
    std::cout<<"OpenXR configuration tests passed\n";
}
