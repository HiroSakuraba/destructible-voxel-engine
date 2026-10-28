#include "dve/xr/openxr_vulkan.hpp"
#include <utility>
namespace dve::xr {
namespace {
bool xr_error(XrResult r,const char* name,std::string& error) {
    if (XR_SUCCEEDED(r)) return false;
    error=std::string(name)+" failed (XrResult "+std::to_string(r)+")"; return true;
}
bool vk_error(VkResult r,const char* name,std::string& error) {
    if (r==VK_SUCCESS) return false;
    error=std::string(name)+" returned VkResult "+std::to_string(r); return true;
}
}
bool vulkan_graphics_binding(VkInstance instance,VkPhysicalDevice physical,VkDevice device,
    uint32_t family,uint32_t queue,XrGraphicsBindingVulkan2KHR& out,std::string& error) {
    out={}; error.clear();
    if (instance==VK_NULL_HANDLE || physical==VK_NULL_HANDLE || device==VK_NULL_HANDLE) {
        error="OpenXR Vulkan graphics binding requires instance, physical device and device"; return false;
    }
    out.type=XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR; out.instance=instance; out.physicalDevice=physical;
    out.device=device; out.queueFamilyIndex=family; out.queueIndex=queue; return true;
}
bool vulkan_swapchain_images(XrSwapchain swapchain,PFN_xrEnumerateSwapchainImages enumerate,
    std::vector<XrSwapchainImageVulkan2KHR>& out,std::string& error) {
    out.clear(); error.clear();
    if (swapchain==XR_NULL_HANDLE || !enumerate) { error="OpenXR Vulkan image enumeration requires a swapchain and function"; return false; }
    uint32_t count=0;
    if (xr_error(enumerate(swapchain,0,&count,nullptr),"xrEnumerateSwapchainImages",error)) return false;
    if (!count || count>4096) {error="OpenXR returned an invalid swapchain image count";return false;}
    std::vector<XrSwapchainImageVulkan2KHR> images(count);
    for (auto& image:images) image.type=XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    uint32_t written=count;
    if (xr_error(enumerate(swapchain,count,&written,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"xrEnumerateSwapchainImages",error)) return false;
    if (!written || written>count) {error="OpenXR swapchain image count changed";return false;}
    images.resize(written);
    for (const auto& image:images) if (image.image==VK_NULL_HANDLE) {error="OpenXR returned a null Vulkan swapchain image";return false;}
    out=std::move(images); return true;
}
bool vulkan_requirements(XrInstance instance,XrSystemId system,const VulkanFunctions& fn,
                         XrGraphicsRequirementsVulkan2KHR& out,std::string& error) {
    out={}; error.clear();
    if (instance==XR_NULL_HANDLE || system==XR_NULL_SYSTEM_ID || !fn.requirements) { error="Missing OpenXR Vulkan requirements inputs"; return false; }
    out.type=XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
    if (xr_error(fn.requirements(instance,system,&out),"xrGetVulkanGraphicsRequirements2KHR",error)) { out={}; return false; }
    return true;
}
bool vulkan_create_instance(XrInstance instance,XrSystemId system,const VulkanFunctions& fn,
    PFN_vkGetInstanceProcAddr getProc,const VkInstanceCreateInfo& info,const VkAllocationCallbacks* allocator,
    VkInstance& out,std::string& error) {
    out=VK_NULL_HANDLE; error.clear();
    if (instance==XR_NULL_HANDLE || system==XR_NULL_SYSTEM_ID || !fn.createInstance || !getProc || info.sType!=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO) { error="Missing OpenXR Vulkan instance inputs"; return false; }
    XrVulkanInstanceCreateInfoKHR create{}; create.type=XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    create.systemId=system; create.pfnGetInstanceProcAddr=getProc; create.vulkanCreateInfo=&info; create.vulkanAllocator=allocator;
    VkResult vk=VK_SUCCESS; VkInstance created=VK_NULL_HANDLE;
    XrResult xr=fn.createInstance(instance,&create,&created,&vk);
    if (xr_error(xr,"xrCreateVulkanInstanceKHR",error) || vk_error(vk,"xrCreateVulkanInstanceKHR",error)) return false;
    if (created==VK_NULL_HANDLE) {error="xrCreateVulkanInstanceKHR returned a null instance";return false;}
    out=created; return true;
}
bool vulkan_select_device(XrInstance instance,XrSystemId system,const VulkanFunctions& fn,VkInstance vkInstance,VkPhysicalDevice& out,std::string& error) {
    out=VK_NULL_HANDLE; error.clear();
    if (instance==XR_NULL_HANDLE || system==XR_NULL_SYSTEM_ID || !fn.selectDevice || vkInstance==VK_NULL_HANDLE) { error="Missing OpenXR Vulkan physical-device inputs"; return false; }
    XrVulkanGraphicsDeviceGetInfoKHR info{}; info.type=XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR; info.systemId=system; info.vulkanInstance=vkInstance;
    VkPhysicalDevice selected=VK_NULL_HANDLE;
    if (xr_error(fn.selectDevice(instance,&info,&selected),"xrGetVulkanGraphicsDevice2KHR",error)) return false;
    if (selected==VK_NULL_HANDLE) {error="OpenXR returned a null physical device";return false;}
    out=selected; return true;
}
bool vulkan_create_device(XrInstance instance,XrSystemId system,const VulkanFunctions& fn,PFN_vkGetInstanceProcAddr getProc,
    VkPhysicalDevice physical,const VkDeviceCreateInfo& info,const VkAllocationCallbacks* allocator,VkDevice& out,std::string& error) {
    out=VK_NULL_HANDLE; error.clear();
    if (instance==XR_NULL_HANDLE || system==XR_NULL_SYSTEM_ID || !fn.createDevice || !getProc || physical==VK_NULL_HANDLE || info.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO) {error="Missing OpenXR Vulkan device inputs";return false;}
    XrVulkanDeviceCreateInfoKHR create{}; create.type=XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR; create.systemId=system;
    create.pfnGetInstanceProcAddr=getProc; create.vulkanPhysicalDevice=physical; create.vulkanCreateInfo=&info; create.vulkanAllocator=allocator;
    VkResult vk=VK_SUCCESS; VkDevice created=VK_NULL_HANDLE;
    XrResult xr=fn.createDevice(instance,&create,&created,&vk);
    if (xr_error(xr,"xrCreateVulkanDeviceKHR",error) || vk_error(vk,"xrCreateVulkanDeviceKHR",error)) return false;
    if (created==VK_NULL_HANDLE) {error="xrCreateVulkanDeviceKHR returned a null device";return false;}
    out=created; return true;
}
}
