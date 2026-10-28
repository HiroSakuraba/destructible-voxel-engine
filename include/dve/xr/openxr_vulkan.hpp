#pragma once
#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <string>
#include <vector>
namespace dve::xr {
// Requires XR_KHR_vulkan_enable2 to have been enabled on the caller's instance.
// All output Vulkan handles are owned by the caller, not by these helpers.
bool vulkan_graphics_binding(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
    uint32_t queueFamily, uint32_t queueIndex, XrGraphicsBindingVulkan2KHR& output, std::string& error);
bool vulkan_swapchain_images(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages enumerate,
    std::vector<XrSwapchainImageVulkan2KHR>& output, std::string& error);
struct VulkanFunctions {
    PFN_xrGetVulkanGraphicsRequirements2KHR requirements{};
    PFN_xrCreateVulkanInstanceKHR createInstance{};
    PFN_xrGetVulkanGraphicsDevice2KHR selectDevice{};
    PFN_xrCreateVulkanDeviceKHR createDevice{};
};
bool vulkan_requirements(XrInstance instance, XrSystemId system, const VulkanFunctions& fn,
                         XrGraphicsRequirementsVulkan2KHR& requirements, std::string& error);
bool vulkan_create_instance(XrInstance instance, XrSystemId system, const VulkanFunctions& fn,
    PFN_vkGetInstanceProcAddr getProc, const VkInstanceCreateInfo& info,
    const VkAllocationCallbacks* allocator, VkInstance& output, std::string& error);
bool vulkan_select_device(XrInstance instance, XrSystemId system, const VulkanFunctions& fn,
    VkInstance vkInstance, VkPhysicalDevice& output, std::string& error);
bool vulkan_create_device(XrInstance instance, XrSystemId system, const VulkanFunctions& fn,
    PFN_vkGetInstanceProcAddr getProc, VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo& info, const VkAllocationCallbacks* allocator,
    VkDevice& output, std::string& error);
}
