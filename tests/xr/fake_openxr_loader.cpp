// Deterministic loader shim for the discovery probe; never a headset/runtime test.
#include <openxr/openxr.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
namespace {
XrResult XRAPI_PTR enumerate(const char*, uint32_t capacity, uint32_t* count, XrExtensionProperties* out) {
    *count = 1;
    if (capacity) { std::strcpy(out[0].extensionName, "XR_KHR_vulkan_enable2"); out[0].extensionVersion = 1; }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR create(const XrInstanceCreateInfo* info, XrInstance* instance) {
    if (!info || info->type != XR_TYPE_INSTANCE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    *instance = reinterpret_cast<XrInstance>(1); return XR_SUCCESS;
}
XrResult XRAPI_PTR destroy(XrInstance) { std::fputs("fake destroy called\n", stderr); return XR_SUCCESS; }
XrResult XRAPI_PTR properties(XrInstance, XrInstanceProperties* p) {
    if (p->type != XR_TYPE_INSTANCE_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    std::strcpy(p->runtimeName, "DVE fake runtime"); p->runtimeVersion = XR_MAKE_VERSION(1, 2, 3); return XR_SUCCESS;
}
XrResult XRAPI_PTR fake_system(XrInstance, const XrSystemGetInfo*, XrSystemId* id) {
    if (std::getenv("DVE_TEST_NO_HEADSET")) return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    *id = 42; return XR_SUCCESS;
}
XrResult XRAPI_PTR system_properties(XrInstance, XrSystemId id, XrSystemProperties* p) {
    if (id != 42 || p->type != XR_TYPE_SYSTEM_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    std::strcpy(p->systemName, "DVE fake headset"); return XR_SUCCESS;
}
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* out) {
#define ENTRY(n, f) if (std::strcmp(name, n) == 0) { *out = reinterpret_cast<PFN_xrVoidFunction>(f); return XR_SUCCESS; }
    ENTRY("xrEnumerateInstanceExtensionProperties", enumerate)
    ENTRY("xrCreateInstance", create)
    ENTRY("xrDestroyInstance", destroy)
    ENTRY("xrGetInstanceProperties", properties)
    ENTRY("xrGetSystem", fake_system)
    ENTRY("xrGetSystemProperties", system_properties)
#undef ENTRY
    *out = nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
