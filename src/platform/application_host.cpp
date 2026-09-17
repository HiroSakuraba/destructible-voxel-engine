#include "dve/platform/application_host.hpp"

namespace dve::platform {

std::string_view host_backend_name(HostBackend backend) noexcept {
    switch (backend) {
        case HostBackend::Headless: return "Headless";
        case HostBackend::SDL3: return "SDL3";
        case HostBackend::Win32: return "Win32";
        case HostBackend::X11: return "X11";
        case HostBackend::Cocoa: return "Cocoa";
    }
    return "Unknown";
}

} // namespace dve::platform
