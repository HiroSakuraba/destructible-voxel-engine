#include "dve/xr/session_loop.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
using namespace dve::xr;
struct Fake : Driver {
    std::vector<std::string> calls;
    std::string fail;
    bool shouldRender{true};
    bool call(const char* name, std::string& e) { calls.emplace_back(name); if(fail == name) { e=std::string(name)+" failed"; return false; } return true; }
    bool begin_session(std::string& e) override { return call("begin_session",e); }
    bool end_session(std::string& e) override { return call("end_session",e); }
    bool wait_frame(FrameTiming& t,std::string& e) override { t={1234,shouldRender}; return call("wait",e); }
    bool begin_frame(std::string& e) override { return call("begin",e); }
    bool render_frame(const FrameTiming& t,std::string& e) override { assert(t.predictedDisplayTime==1234); return call("render",e); }
    bool end_frame(const FrameTiming& t,bool layers,std::string& e) override { assert(t.predictedDisplayTime==1234); return call(layers?"end_layers":"end_empty",e); }
};
int main() {
    std::string error;
    Fake d; SessionLoop loop(d);
    assert(loop.frame(error)==FrameOutcome::NotRunning && d.calls.empty());
    assert(loop.on_state(SessionState::Ready,error) && loop.running());
    assert(loop.on_state(SessionState::Ready,error) && d.calls.size()==1);
    assert(loop.on_state(SessionState::Focused,error) && loop.focused());
    d.calls.clear(); assert(loop.frame(error)==FrameOutcome::Submitted);
    assert((d.calls==std::vector<std::string>{"wait","begin","render","end_layers"}));
    d.calls.clear(); d.shouldRender=false; assert(loop.frame(error)==FrameOutcome::Empty);
    assert((d.calls==std::vector<std::string>{"wait","begin","end_empty"}));
    d.calls.clear(); d.shouldRender=true; d.fail="render"; assert(loop.frame(error)==FrameOutcome::Failed);
    assert(error=="render failed"); assert((d.calls==std::vector<std::string>{"wait","begin","render","end_empty"}));
    d.calls.clear(); d.fail="wait"; assert(loop.frame(error)==FrameOutcome::Failed);
    assert((d.calls==std::vector<std::string>{"wait"}));
    d.calls.clear(); d.fail="begin"; assert(loop.frame(error)==FrameOutcome::Failed);
    assert((d.calls==std::vector<std::string>{"wait","begin"}));
    d.calls.clear(); d.fail="end_layers"; assert(loop.frame(error)==FrameOutcome::Failed && error=="end_layers failed");
    d.calls.clear(); d.fail="end_session"; assert(!loop.on_state(SessionState::Stopping,error) && loop.running());
    d.fail=""; assert(loop.on_state(SessionState::Stopping,error) && !loop.running());
    assert(loop.frame(error)==FrameOutcome::NotRunning);
    assert(loop.on_state(SessionState::Ready,error) && loop.running());
    assert(loop.on_state(SessionState::LossPending,error) && loop.terminal() && !loop.running());
    assert(!loop.on_state(SessionState::Ready,error));
    Fake d2; d2.fail="begin_session"; SessionLoop second(d2);
    assert(!second.on_state(SessionState::Ready,error) && !second.running());
    std::cout << "XR session loop tests passed\n";
}
