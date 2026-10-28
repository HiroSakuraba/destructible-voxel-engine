#include "dve/xr/openxr_events.hpp"
#include <cstring>
namespace dve::xr {
bool pump_openxr_events(XrInstance instance, XrSession session, PFN_xrPollEvent poll,
                        SessionLoop& loop, std::uint32_t budget, EventReport& report,
                        std::string& error) {
    error.clear(); report={};
    if (instance==XR_NULL_HANDLE || session==XR_NULL_HANDLE || !poll || budget==0) {
        error="OpenXR event pump requires an instance, session, poll function and positive budget"; return false;
    }
    for (std::uint32_t i=0; i<budget; ++i) {
        XrEventDataBuffer event{}; event.type=XR_TYPE_EVENT_DATA_BUFFER;
        const auto result=poll(instance,&event);
        if (result==XR_EVENT_UNAVAILABLE) return true;
        if (result!=XR_SUCCESS) { error="xrPollEvent failed (XrResult "+std::to_string(result)+")"; return false; }
        ++report.processed;
        if (event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged e{}; std::memcpy(&e,&event,sizeof(e));
            if (e.session!=session) continue;
            SessionState state;
            if (!session_state_from_openxr(e.state,state)) { error="Unknown OpenXR session state"; return false; }
            if (!loop.on_state(state,error)) return false;
            if (loop.terminal()) return true;
        } else if (event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            if (!loop.on_state(SessionState::LossPending,error)) return false;
            return true;
        } else if (event.type==XR_TYPE_EVENT_DATA_EVENTS_LOST) {
            XrEventDataEventsLost e{}; std::memcpy(&e,&event,sizeof(e));
            report.eventsLost+=e.lostEventCount;
        } else if (event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            XrEventDataReferenceSpaceChangePending e{}; std::memcpy(&e,&event,sizeof(e));
            if (e.session==session) { e.next=nullptr; report.referenceSpaceChanges.push_back(e); }
        }
    }
    report.budgetExhausted=true;
    return true;
}
}
