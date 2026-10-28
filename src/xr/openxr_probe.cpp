// Optional OpenXR runtime discovery probe. Does not create a graphics session.
#include <openxr/openxr.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
struct Library {
#ifdef _WIN32
    HMODULE handle{};
    explicit Library(const char* path) : handle(LoadLibraryA(path)) {}
    ~Library() { if (handle) FreeLibrary(handle); }
    void* symbol(const char* name) const { return reinterpret_cast<void*>(GetProcAddress(handle, name)); }
#else
    void* handle{};
    explicit Library(const char* path) : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}
    ~Library() { if (handle) dlclose(handle); }
    void* symbol(const char* name) const { return dlsym(handle, name); }
#endif
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
};

template<class T> bool resolve(PFN_xrGetInstanceProcAddr get, XrInstance instance, const char* name, T& out) {
    PFN_xrVoidFunction fn = nullptr;
    const auto result = get(instance, name, &fn);
    if (XR_FAILED(result) || !fn) { std::cerr << "OpenXR: cannot resolve " << name << " (" << result << ")\n"; return false; }
    out = reinterpret_cast<T>(fn);
    return true;
}
struct Instance {
    XrInstance value{XR_NULL_HANDLE};
    PFN_xrDestroyInstance destroy{};
    ~Instance() { if (value != XR_NULL_HANDLE && destroy) destroy(value); }
};
}

int main(int argc, char** argv) {
#ifdef _WIN32
    const char* path = "openxr_loader.dll";
#else
    const char* path = "libopenxr_loader.so.1";
#endif
    if (argc == 3 && std::strcmp(argv[1], "--loader") == 0) path = argv[2];
    else if (argc != 1) { std::cerr << "Usage: dve_openxr_probe [--loader path]\n"; return 2; }
    Library lib(path);
    if (!lib.handle) { std::cerr << "OpenXR loader unavailable: " << path << ". Install an OpenXR runtime and loader, or specify --loader.\n"; return 1; }
    const auto get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(lib.symbol("xrGetInstanceProcAddr"));
    if (!get) { std::cerr << "OpenXR loader does not export xrGetInstanceProcAddr\n"; return 1; }
    PFN_xrEnumerateInstanceExtensionProperties enumerate{};
    PFN_xrCreateInstance create{};
    if (!resolve(get, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", enumerate) ||
        !resolve(get, XR_NULL_HANDLE, "xrCreateInstance", create)) return 1;
    uint32_t count = 0;
    auto result = enumerate(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) { std::cerr << "OpenXR extension enumeration failed (" << result << "); check active runtime configuration.\n"; return 1; }
    std::vector<XrExtensionProperties> extensions(count);
    for (auto& extension : extensions) { extension.type = XR_TYPE_EXTENSION_PROPERTIES; extension.next = nullptr; }
    if (count) {
        uint32_t written = count;
        result = enumerate(nullptr, count, &written, extensions.data());
        if (XR_FAILED(result) || written > count) { std::cerr << "OpenXR extension enumeration failed or changed (" << result << ")\n"; return 1; }
        extensions.resize(written);
    }
    bool vulkan = false;
    for (const auto& ext : extensions) {
        if (std::strcmp(ext.extensionName, "XR_KHR_vulkan_enable") == 0 ||
            std::strcmp(ext.extensionName, "XR_KHR_vulkan_enable2") == 0) vulkan = true;
    }
    XrInstanceCreateInfo info{}; info.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::strncpy(info.applicationInfo.applicationName, "DVE OpenXR probe", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "DVE", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    Instance instance;
    result = create(&info, &instance.value);
    if (XR_FAILED(result)) { std::cerr << "OpenXR instance creation failed (" << result << ")\n"; return 1; }
    if (!resolve(get, instance.value, "xrDestroyInstance", instance.destroy)) {
        // A conforming runtime provides xrDestroyInstance. Report the failure; no safe destroy is available.
        return 1;
    }
    PFN_xrGetInstanceProperties properties{};
    PFN_xrGetSystem system{};
    PFN_xrGetSystemProperties systemProperties{};
    if (!resolve(get, instance.value, "xrGetInstanceProperties", properties) ||
        !resolve(get, instance.value, "xrGetSystem", system) ||
        !resolve(get, instance.value, "xrGetSystemProperties", systemProperties)) return 1;
    XrInstanceProperties props{}; props.type = XR_TYPE_INSTANCE_PROPERTIES;
    result = properties(instance.value, &props);
    if (XR_FAILED(result)) { std::cerr << "OpenXR runtime properties failed (" << result << ")\n"; return 1; }
    std::cout << "Runtime: " << props.runtimeName << " (" << XR_VERSION_MAJOR(props.runtimeVersion) << '.'
              << XR_VERSION_MINOR(props.runtimeVersion) << '.' << XR_VERSION_PATCH(props.runtimeVersion) << ")\n";
    std::cout << "Vulkan extension advertised: " << (vulkan ? "yes" : "no") << '\n';
    XrSystemGetInfo systemInfo{}; systemInfo.type = XR_TYPE_SYSTEM_GET_INFO; systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId id = XR_NULL_SYSTEM_ID;
    result = system(instance.value, &systemInfo, &id);
    if (XR_FAILED(result)) { std::cerr << "OpenXR headset system unavailable (" << result << "); connect a headset and check the active runtime.\n"; return 1; }
    XrSystemProperties sys{}; sys.type = XR_TYPE_SYSTEM_PROPERTIES;
    result = systemProperties(instance.value, id, &sys);
    if (XR_FAILED(result)) { std::cerr << "OpenXR headset properties failed (" << result << ")\n"; return 1; }
    std::cout << "Headset system: " << sys.systemName << '\n';
    std::cout << "Discovery only: no VR session, stereo rendering, or headset display was tested.\n";
    return 0;
}
