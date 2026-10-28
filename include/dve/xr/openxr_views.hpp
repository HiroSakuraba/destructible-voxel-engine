#pragma once
#include <openxr/openxr.h>
#include <array>
#include <string>
namespace dve::xr {
struct StereoViews {
    std::array<XrView,2> views{};
    XrViewStateFlags flags{};
};
enum class LocateResult { Valid, Untracked, Failed };
// Caller selects a supported primary-stereo configuration and a valid reference space.
// Untracked is not an error and must never be submitted as a new projection layer.
LocateResult locate_stereo_views(XrSession session, XrSpace space, XrTime displayTime,
    PFN_xrLocateViews locate, StereoViews& output, std::string& error);
}
