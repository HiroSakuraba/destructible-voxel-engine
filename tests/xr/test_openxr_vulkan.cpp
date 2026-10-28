#include "dve/xr/openxr_vulkan.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace dve::xr;
static XrResult xrResult=XR_SUCCESS; static VkResult vkResult=VK_SUCCESS;
static XrResult XRAPI_PTR req(XrInstance,XrSystemId s,XrGraphicsRequirementsVulkanKHR* p) {assert(s==5 && p->type==XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);p->minApiVersionSupported=XR_MAKE_VERSION(1,0,0);return xrResult;}
static XrResult XRAPI_PTR instanceCreate(XrInstance,const XrVulkanInstanceCreateInfoKHR* info,VkInstance* v,VkResult* result) {assert(info->systemId==5 && info->type==XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR && info->vulkanCreateInfo->sType==VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);*result=vkResult;*v=reinterpret_cast<VkInstance>(1);return xrResult;}
static XrResult XRAPI_PTR select(XrInstance,const XrVulkanGraphicsDeviceGetInfoKHR* info,VkPhysicalDevice* p) {assert(info->systemId==5 && info->vulkanInstance==reinterpret_cast<VkInstance>(1));*p=reinterpret_cast<VkPhysicalDevice>(2);return xrResult;}
static XrResult XRAPI_PTR deviceCreate(XrInstance,const XrVulkanDeviceCreateInfoKHR* info,VkDevice* d,VkResult* result) {assert(info->systemId==5 && info->vulkanPhysicalDevice==reinterpret_cast<VkPhysicalDevice>(2) && info->vulkanCreateInfo->sType==VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);*result=vkResult;*d=reinterpret_cast<VkDevice>(3);return xrResult;}
static XrResult XRAPI_PTR images(XrSwapchain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* data) {
    *count=2; if (!capacity) return XR_SUCCESS; assert(capacity==2);
    auto* vk=reinterpret_cast<XrSwapchainImageVulkan2KHR*>(data);
    assert(vk[0].type==XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR && vk[1].type==XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR);
    vk[0].image=reinterpret_cast<VkImage>(4);vk[1].image=reinterpret_cast<VkImage>(5);return XR_SUCCESS;
}
static PFN_vkVoidFunction VKAPI_PTR getProc(VkInstance,const char*) {return nullptr;}
int main(){
    auto xr=reinterpret_cast<XrInstance>(8); VulkanFunctions fn{req,instanceCreate,select,deviceCreate}; std::string error;
    XrGraphicsRequirementsVulkan2KHR r{}; assert(vulkan_requirements(xr,5,fn,r,error) && XR_VERSION_MAJOR(r.minApiVersionSupported)==1);
    VkInstanceCreateInfo ii{}; ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; VkInstance vi{};
    assert(vulkan_create_instance(xr,5,fn,getProc,ii,nullptr,vi,error) && vi==reinterpret_cast<VkInstance>(1));
    VkPhysicalDevice gpu{}; assert(vulkan_select_device(xr,5,fn,vi,gpu,error) && gpu==reinterpret_cast<VkPhysicalDevice>(2));
    VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;VkDevice device{};
    assert(vulkan_create_device(xr,5,fn,getProc,gpu,di,nullptr,device,error) && device==reinterpret_cast<VkDevice>(3));
    XrGraphicsBindingVulkan2KHR binding{};
    assert(vulkan_graphics_binding(vi,gpu,device,3,1,binding,error));
    assert(binding.type==XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR && binding.queueFamilyIndex==3 && binding.queueIndex==1);
    std::vector<XrSwapchainImageVulkan2KHR> swapImages;
    assert(vulkan_swapchain_images(reinterpret_cast<XrSwapchain>(4),images,swapImages,error));
    assert(swapImages.size()==2 && swapImages[1].image==reinterpret_cast<VkImage>(5));
    assert(!vulkan_swapchain_images(XR_NULL_HANDLE,images,swapImages,error) && swapImages.empty());
    vkResult=VK_ERROR_INITIALIZATION_FAILED; device=reinterpret_cast<VkDevice>(3);
    assert(!vulkan_create_device(xr,5,fn,getProc,gpu,di,nullptr,device,error) && device==VK_NULL_HANDLE && error.find("VkResult")!=std::string::npos);
    vkResult=VK_SUCCESS; xrResult=XR_ERROR_RUNTIME_FAILURE; vi=reinterpret_cast<VkInstance>(1);
    assert(!vulkan_create_instance(xr,5,fn,getProc,ii,nullptr,vi,error) && vi==VK_NULL_HANDLE && error.find("XrResult")!=std::string::npos);
    gpu=reinterpret_cast<VkPhysicalDevice>(2); assert(!vulkan_select_device(xr,5,fn,reinterpret_cast<VkInstance>(1),gpu,error) && gpu==VK_NULL_HANDLE);
    std::cout << "OpenXR Vulkan bridge contract tests passed\n";
}
