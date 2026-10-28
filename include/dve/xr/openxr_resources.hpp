#pragma once
#include <openxr/openxr.h>
#include <cstdint>
#include <string>

namespace dve::xr {
// Borrowed function pointers; objects created by these wrappers must be explicitly
// closed before destroying the instance/session or unloading the runtime.
struct SessionFunctions {
    PFN_xrCreateSession create{};
    PFN_xrDestroySession destroy{};
};
class SessionHandle {
public:
    SessionHandle() = default;
    SessionHandle(const SessionHandle&) = delete;
    SessionHandle& operator=(const SessionHandle&) = delete;
    bool create(XrInstance instance, XrSystemId system, const void* graphicsBinding,
                SessionFunctions functions, std::string& error);
    // Caller must first stop all frame work and destroy child spaces/swapchains.
    bool close(std::string& error);
    XrSession get() const noexcept { return session_; }
private:
    XrSession session_{XR_NULL_HANDLE};
    PFN_xrDestroySession destroy_{};
};
struct SwapchainFunctions {
    PFN_xrCreateSwapchain create{};
    PFN_xrDestroySwapchain destroy{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
};
enum class WaitResult { Ready, Timeout, Failed };
class SwapchainHandle {
public:
    SwapchainHandle() = default;
    SwapchainHandle(const SwapchainHandle&) = delete;
    SwapchainHandle& operator=(const SwapchainHandle&) = delete;
    bool create(XrSession session, const XrSwapchainCreateInfo& info,
                SwapchainFunctions functions, std::string& error);
    bool acquire(std::uint32_t& index, std::string& error);
    WaitResult wait(XrDuration timeout, std::string& error);
    bool release(std::string& error);
    // Refuses to destroy an acquired image. After a timeout, wait again before release.
    bool close(std::string& error);
    XrSwapchain get() const noexcept { return swapchain_; }
    bool acquired() const noexcept { return state_!=State::Available; }
private:
    enum class State { Available, Acquired, Ready };
    XrSwapchain swapchain_{XR_NULL_HANDLE};
    SwapchainFunctions fn_{};
    State state_{State::Available};
};
}
