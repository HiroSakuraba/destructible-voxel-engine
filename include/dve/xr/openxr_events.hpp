#pragma once
#include "dve/xr/openxr_driver.hpp"
#include <cstdint>
namespace dve::xr {
struct EventReport {
    std::uint32_t processed{};
    std::uint64_t eventsLost{};
    bool budgetExhausted{}; // more events may remain; pump again before rendering
    // Copies retain the time, space, and pose required to handle recentering.
    std::vector<XrEventDataReferenceSpaceChangePending> referenceSpaceChanges;
};
// Polls at most budget events; only applies state events for the owned session.
// The instance, session and function pointer are borrowed and must remain valid.
bool pump_openxr_events(XrInstance instance, XrSession session, PFN_xrPollEvent poll,
                        SessionLoop& loop, std::uint32_t budget, EventReport& report,
                        std::string& error);
}
