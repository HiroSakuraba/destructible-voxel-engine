#pragma once
#include "dve/xr/session_loop.hpp"
#include <openxr/openxr.h>
#include <vector>

namespace dve::xr {
// Function table is resolved by the caller for its instance. Session and layer data
// are borrowed: caller owns them and keeps them alive through end_frame.
struct OpenXrFunctions {
    PFN_xrBeginSession beginSession{};
    PFN_xrEndSession endSession{};
    PFN_xrWaitFrame waitFrame{};
    PFN_xrBeginFrame beginFrame{};
    PFN_xrEndFrame endFrame{};
};
struct OpenXrRenderer {
    virtual ~OpenXrRenderer() = default;
    // Release acquired images when possible; report unrecoverable lifecycle failure
    // so the application can stop and tear down. Returned layer pointers
    // and their nested data must remain valid until the subsequent end_frame call.
    virtual bool render(XrTime time, std::vector<const XrCompositionLayerBaseHeader*>& layers,
                        std::string& error) = 0;
};
class OpenXrDriver final : public Driver {
public:
    OpenXrDriver(XrSession session, OpenXrFunctions functions, XrViewConfigurationType viewType,
                 XrEnvironmentBlendMode blendMode, OpenXrRenderer& renderer);
    bool begin_session(std::string& error) override;
    bool end_session(std::string& error) override;
    bool wait_frame(FrameTiming& timing, std::string& error) override;
    bool begin_frame(std::string& error) override;
    bool render_frame(const FrameTiming& timing, std::string& error) override;
    bool end_frame(const FrameTiming& timing, bool submitLayers, std::string& error) override;
private:
    XrSession session_;
    OpenXrFunctions fn_;
    XrViewConfigurationType viewType_;
    XrEnvironmentBlendMode blendMode_;
    OpenXrRenderer& renderer_;
    std::vector<const XrCompositionLayerBaseHeader*> layers_;
};
// Returns false for UNKNOWN and future/unrecognized session states.
bool session_state_from_openxr(XrSessionState input, SessionState& output) noexcept;
}
