#include "dve/xr/openxr_events.hpp"
#include "dve/xr/openxr_views.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
using namespace dve::xr;
static std::deque<XrEventDataBuffer> queue;
template<class T> void push(T e) { XrEventDataBuffer b{}; static_assert(sizeof(e)<=sizeof(b)); std::memcpy(&b,&e,sizeof(e)); queue.push_back(b); }
static XrResult XRAPI_PTR poll(XrInstance, XrEventDataBuffer* e) {
    assert(e->type==XR_TYPE_EVENT_DATA_BUFFER && e->next==nullptr);
    if(queue.empty()) return XR_EVENT_UNAVAILABLE;
    *e=queue.front(); queue.pop_front(); return XR_SUCCESS;
}
struct Fake : Driver {
    int begin{},end{};
    bool begin_session(std::string&) override {++begin; return true;}
    bool end_session(std::string&) override {++end; return true;}
    bool wait_frame(FrameTiming&,std::string&) override {return false;}
    bool begin_frame(std::string&) override {return false;}
    bool render_frame(const FrameTiming&,std::string&) override {return false;}
    bool end_frame(const FrameTiming&,bool,std::string&) override {return false;}
};
static bool tracked=true; static bool failLocate=false; static uint32_t viewCount=2;
static XrResult XRAPI_PTR locate(XrSession,const XrViewLocateInfo* info,XrViewState* state,uint32_t capacity,uint32_t* count,XrView* views) {
    assert(info->displayTime==123 && info->viewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && capacity==2);
    assert(views[0].type==XR_TYPE_VIEW && views[1].type==XR_TYPE_VIEW);
    if(failLocate) return XR_ERROR_RUNTIME_FAILURE;
    *count=viewCount; state->viewStateFlags=tracked?(XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT):0;
    views[0].pose.position.x=-0.03f; views[1].pose.position.x=0.03f;
    return XR_SUCCESS;
}
int main() {
    auto session=reinterpret_cast<XrSession>(1); auto instance=reinterpret_cast<XrInstance>(2);
    auto space=reinterpret_cast<XrSpace>(3); Fake d; SessionLoop loop(d); EventReport r; std::string error;
    XrEventDataSessionStateChanged e{}; e.type=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED; e.session=session; e.state=XR_SESSION_STATE_READY; push(e);
    e.session=reinterpret_cast<XrSession>(9); e.state=XR_SESSION_STATE_STOPPING; push(e);
    XrEventDataEventsLost lost{}; lost.type=XR_TYPE_EVENT_DATA_EVENTS_LOST; lost.lostEventCount=3; push(lost);
    XrEventDataReferenceSpaceChangePending change{}; change.type=XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING; change.session=session; change.changeTime=400; change.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; change.poseValid=XR_TRUE; change.poseInPreviousSpace.position.x=2; push(change);
    assert(pump_openxr_events(instance,session,poll,loop,2,r,error));
    assert(r.budgetExhausted && r.processed==2 && d.begin==1 && d.end==0 && loop.running());
    assert(pump_openxr_events(instance,session,poll,loop,5,r,error));
    assert(!r.budgetExhausted && r.eventsLost==3 && r.referenceSpaceChanges.size()==1);
    assert(r.referenceSpaceChanges[0].changeTime==400 && r.referenceSpaceChanges[0].poseInPreviousSpace.position.x==2);
    e.session=session; e.state=XR_SESSION_STATE_FOCUSED; push(e);
    assert(pump_openxr_events(instance,session,poll,loop,5,r,error) && loop.focused());
    e.state=XR_SESSION_STATE_STOPPING; push(e);
    assert(pump_openxr_events(instance,session,poll,loop,5,r,error) && !loop.running() && d.end==1);
    XrEventDataInstanceLossPending loss{}; loss.type=XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING; push(loss);
    assert(pump_openxr_events(instance,session,poll,loop,5,r,error) && loop.terminal());
    assert(!pump_openxr_events(instance,session,poll,loop,0,r,error));
    StereoViews output;
    assert(locate_stereo_views(session,space,123,locate,output,error)==LocateResult::Valid);
    assert(output.views[0].pose.position.x<0 && output.views[1].pose.position.x>0);
    tracked=false; assert(locate_stereo_views(session,space,123,locate,output,error)==LocateResult::Untracked && output.views[0].pose.position.x==0);
    tracked=true; viewCount=1; assert(locate_stereo_views(session,space,123,locate,output,error)==LocateResult::Failed);
    viewCount=2; failLocate=true; assert(locate_stereo_views(session,space,123,locate,output,error)==LocateResult::Failed);
    assert(locate_stereo_views(session,space,0,locate,output,error)==LocateResult::Failed);
    std::cout << "OpenXR events and stereo views tests passed\n";
}
