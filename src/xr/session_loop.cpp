#include "dve/xr/session_loop.hpp"

namespace dve::xr {
bool SessionLoop::on_state(SessionState state, std::string& error) {
    error.clear();
    if (terminal_) { error = "XR session is terminal"; return false; }
    // Failed begin/end leaves the prior state intact so the caller can report it
    // and choose whether to tear down or retry. Never mark a failed begin running.
    if (state == SessionState::Ready && !running_) {
        if (!driver_.begin_session(error)) return false;
        running_ = true;
    } else if (state == SessionState::Stopping && running_) {
        if (!driver_.end_session(error)) return false;
        running_ = false;
    }
    state_ = state;
    if (state == SessionState::LossPending || state == SessionState::Exiting) {
        terminal_ = true;
        running_ = false;
    }
    return true;
}
FrameOutcome SessionLoop::frame(std::string& error) {
    error.clear();
    if (!running_ || terminal_) return FrameOutcome::NotRunning;
    FrameTiming timing{};
    if (!driver_.wait_frame(timing, error)) return FrameOutcome::Failed;
    if (!driver_.begin_frame(error)) return FrameOutcome::Failed;
    bool rendered = false;
    std::string renderError;
    if (timing.shouldRender) rendered = driver_.render_frame(timing, renderError);
    std::string endError;
    const bool ended = driver_.end_frame(timing, rendered, endError);
    if (timing.shouldRender && !rendered) {
        error = renderError.empty() ? "XR rendering failed" : renderError;
        if (!ended) error += "; end frame failed: " + endError;
        return FrameOutcome::Failed;
    }
    if (!ended) { error = endError.empty() ? "XR end frame failed" : endError; return FrameOutcome::Failed; }
    return rendered ? FrameOutcome::Submitted : FrameOutcome::Empty;
}
}
