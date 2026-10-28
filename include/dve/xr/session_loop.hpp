#pragma once
#include <cstdint>
#include <string>

namespace dve::xr {
// A runtime-independent controller, not an OpenXR runtime adapter. Events and frame
// operations must be supplied by the future adapter on a single owning thread.
enum class SessionState { Idle, Ready, Synchronized, Visible, Focused, Stopping, LossPending, Exiting };
struct FrameTiming { std::int64_t predictedDisplayTime{}; bool shouldRender{}; };
struct Driver {
    virtual ~Driver() = default;
    virtual bool begin_session(std::string& error) = 0;
    virtual bool end_session(std::string& error) = 0;
    virtual bool wait_frame(FrameTiming& timing, std::string& error) = 0;
    virtual bool begin_frame(std::string& error) = 0;
    // The renderer is responsible for view location and swapchain acquire/wait/release.
    // On failure the renderer must report any unrecoverable acquired-image state
    // to the application, which must stop rendering and tear down the session.
    virtual bool render_frame(const FrameTiming& timing, std::string& error) = 0;
    // submitLayers=false submits an empty frame. This must still be called after a
    // successful begin_frame if render_frame fails or shouldRender is false.
    virtual bool end_frame(const FrameTiming& timing, bool submitLayers, std::string& error) = 0;
};
enum class FrameOutcome { NotRunning, Submitted, Empty, Failed };
class SessionLoop {
public:
    explicit SessionLoop(Driver& driver) : driver_(driver) {}
    bool on_state(SessionState state, std::string& error);
    FrameOutcome frame(std::string& error);
    bool running() const noexcept { return running_; }
    bool focused() const noexcept { return state_ == SessionState::Focused && running_; }
    bool terminal() const noexcept { return terminal_; }
    SessionState state() const noexcept { return state_; }
private:
    Driver& driver_;
    SessionState state_{SessionState::Idle};
    bool running_{};
    bool terminal_{};
};
}
