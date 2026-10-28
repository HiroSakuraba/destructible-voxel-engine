#include "dve/xr/openxr_resources.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace dve::xr;
static int creates=0,destroys=0,acquires=0,waits=0,releases=0;
static XrResult waitResult=XR_SUCCESS, releaseResult=XR_SUCCESS, destroyResult=XR_SUCCESS;
static XrResult XRAPI_PTR createSession(XrInstance,const XrSessionCreateInfo* i,XrSession* s) {
    assert(i->type==XR_TYPE_SESSION_CREATE_INFO && i->systemId==10 && i->next!=nullptr); ++creates; *s=reinterpret_cast<XrSession>(3); return XR_SUCCESS;
}
static XrResult XRAPI_PTR destroySession(XrSession s) { assert(s==reinterpret_cast<XrSession>(3)); ++destroys; return destroyResult; }
static XrResult XRAPI_PTR createChain(XrSession,const XrSwapchainCreateInfo* i,XrSwapchain* s) {assert(i->type==XR_TYPE_SWAPCHAIN_CREATE_INFO); *s=reinterpret_cast<XrSwapchain>(4);return XR_SUCCESS;}
static XrResult XRAPI_PTR destroyChain(XrSwapchain) {++destroys;return destroyResult;}
static XrResult XRAPI_PTR acquire(XrSwapchain,const XrSwapchainImageAcquireInfo* i,uint32_t* index) {assert(i->type==XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);++acquires;*index=2;return XR_SUCCESS;}
static XrResult XRAPI_PTR wait(XrSwapchain,const XrSwapchainImageWaitInfo* i) {assert(i->type==XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);++waits;return waitResult;}
static XrResult XRAPI_PTR release(XrSwapchain,const XrSwapchainImageReleaseInfo* i) {assert(i->type==XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);++releases;return releaseResult;}
int main() {
    std::string error; SessionHandle session; int binding=1;
    assert(!session.create(XR_NULL_HANDLE,10,&binding,{createSession,destroySession},error));
    assert(session.create(reinterpret_cast<XrInstance>(2),10,&binding,{createSession,destroySession},error));
    assert(!session.create(reinterpret_cast<XrInstance>(2),10,&binding,{createSession,destroySession},error));
    XrSwapchainCreateInfo info{}; info.type=XR_TYPE_SWAPCHAIN_CREATE_INFO; SwapchainHandle chain;
    assert(chain.create(session.get(),info,{createChain,destroyChain,acquire,wait,release},error));
    uint32_t index=0; assert(chain.acquire(index,error) && index==2 && chain.acquired());
    assert(!chain.acquire(index,error)); assert(!chain.close(error)); assert(!chain.release(error));
    waitResult=XR_TIMEOUT_EXPIRED; assert(chain.wait(0,error)==WaitResult::Timeout && chain.acquired());
    assert(!chain.release(error)); assert(!chain.close(error));
    waitResult=XR_ERROR_RUNTIME_FAILURE; assert(chain.wait(0,error)==WaitResult::Failed && chain.acquired());
    waitResult=XR_SUCCESS; assert(chain.wait(XR_INFINITE_DURATION,error)==WaitResult::Ready);
    releaseResult=XR_ERROR_RUNTIME_FAILURE; assert(!chain.release(error) && chain.acquired());
    releaseResult=XR_SUCCESS; assert(chain.release(error) && !chain.acquired());
    destroyResult=XR_ERROR_RUNTIME_FAILURE; assert(!chain.close(error) && chain.get()!=XR_NULL_HANDLE);
    destroyResult=XR_SUCCESS; assert(chain.close(error) && chain.get()==XR_NULL_HANDLE); assert(chain.close(error));
    assert(session.close(error) && session.get()==XR_NULL_HANDLE); assert(session.close(error));
    assert(acquires==1 && waits==3 && releases==2 && creates==1);
    std::cout << "OpenXR resource lifecycle tests passed\n";
}
