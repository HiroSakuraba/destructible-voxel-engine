#include "dve/xr/openxr_resources.hpp"
namespace dve::xr {
namespace {
bool failed(XrResult result, const char* operation, std::string& error) {
    if (XR_SUCCEEDED(result)) return false;
    error=std::string(operation)+" failed (XrResult "+std::to_string(result)+")"; return true;
}
}
bool SessionHandle::create(XrInstance instance,XrSystemId system,const void* binding,SessionFunctions fn,std::string& error) {
    error.clear();
    if (session_!=XR_NULL_HANDLE || instance==XR_NULL_HANDLE || system==XR_NULL_SYSTEM_ID || !binding || !fn.create || !fn.destroy) {
        error="OpenXR session creation requires a new handle, instance, system, graphics binding and functions"; return false;
    }
    XrSessionCreateInfo info{}; info.type=XR_TYPE_SESSION_CREATE_INFO; info.systemId=system; info.next=binding;
    XrSession created=XR_NULL_HANDLE;
    if (failed(fn.create(instance,&info,&created),"xrCreateSession",error)) return false;
    if (created==XR_NULL_HANDLE) { error="xrCreateSession returned a null session"; return false; }
    session_=created; destroy_=fn.destroy; return true;
}
bool SessionHandle::close(std::string& error) {
    error.clear(); if (session_==XR_NULL_HANDLE) return true;
    if (failed(destroy_(session_),"xrDestroySession",error)) return false;
    session_=XR_NULL_HANDLE; destroy_=nullptr; return true;
}
bool SwapchainHandle::create(XrSession session,const XrSwapchainCreateInfo& info,SwapchainFunctions fn,std::string& error) {
    error.clear();
    if (swapchain_!=XR_NULL_HANDLE || session==XR_NULL_HANDLE || info.type!=XR_TYPE_SWAPCHAIN_CREATE_INFO ||
        !fn.create || !fn.destroy || !fn.acquire || !fn.wait || !fn.release) {
        error="OpenXR swapchain creation requires a new handle, session, create info and functions"; return false;
    }
    XrSwapchain created=XR_NULL_HANDLE;
    if (failed(fn.create(session,&info,&created),"xrCreateSwapchain",error)) return false;
    if (created==XR_NULL_HANDLE) { error="xrCreateSwapchain returned a null swapchain"; return false; }
    swapchain_=created; fn_=fn; state_=State::Available; return true;
}
bool SwapchainHandle::acquire(std::uint32_t& index,std::string& error) {
    error.clear(); index=0;
    if (swapchain_==XR_NULL_HANDLE || state_!=State::Available) { error="OpenXR swapchain image is not available to acquire"; return false; }
    XrSwapchainImageAcquireInfo info{}; info.type=XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (failed(fn_.acquire(swapchain_,&info,&index),"xrAcquireSwapchainImage",error)) return false;
    state_=State::Acquired; return true;
}
WaitResult SwapchainHandle::wait(XrDuration timeout,std::string& error) {
    error.clear();
    if (swapchain_==XR_NULL_HANDLE || state_!=State::Acquired || timeout<0) { error="OpenXR image must be acquired before waiting"; return WaitResult::Failed; }
    XrSwapchainImageWaitInfo info{}; info.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; info.timeout=timeout;
    const XrResult result=fn_.wait(swapchain_,&info);
    if (result==XR_TIMEOUT_EXPIRED) return WaitResult::Timeout;
    if (failed(result,"xrWaitSwapchainImage",error)) return WaitResult::Failed;
    state_=State::Ready; return WaitResult::Ready;
}
bool SwapchainHandle::release(std::string& error) {
    error.clear();
    if (swapchain_==XR_NULL_HANDLE || state_!=State::Ready) { error="OpenXR image must finish waiting before release"; return false; }
    XrSwapchainImageReleaseInfo info{}; info.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    if (failed(fn_.release(swapchain_,&info),"xrReleaseSwapchainImage",error)) return false;
    state_=State::Available; return true;
}
bool SwapchainHandle::close(std::string& error) {
    error.clear(); if (swapchain_==XR_NULL_HANDLE) return true;
    if (state_!=State::Available) { error="OpenXR swapchain still has an acquired image"; return false; }
    if (failed(fn_.destroy(swapchain_),"xrDestroySwapchain",error)) return false;
    swapchain_=XR_NULL_HANDLE; fn_={}; return true;
}
}
