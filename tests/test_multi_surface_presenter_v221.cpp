#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dve/render/multi_surface_presenter.hpp"
#include "dve/rhi/null_device.hpp"

namespace {
using namespace dve;
using namespace dve::presentation;
using namespace dve::render;
using namespace dve::rhi;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

PresentationSurfaceRequest surface(WindowId id, std::uint32_t width, std::uint32_t height) {
    PresentationSurfaceRequest result;
    result.window = id;
    result.nativeWindow = {platform::HostBackend::Headless, nullptr, nullptr, id};
    result.width = width;
    result.height = height;
    result.presentMode = id == 1U ? PresentMode::Fifo : PresentMode::Mailbox;
    result.debugName = id == 1U ? "main game window" : "spectator window";
    return result;
}

FenceHandle prepare_present(NullDevice& device, TextureHandle texture, const char* label,
                            std::string& error) {
    const auto commands = device.begin_commands(QueueKind::Graphics, label, &error);
    require(static_cast<bool>(commands), error.c_str());
    require(device.transition_texture(commands, texture, ResourceState::Present,
                                      ResourceState::RenderTarget, &error), error.c_str());
    require(device.transition_texture(commands, texture, ResourceState::RenderTarget,
                                      ResourceState::Present, &error), error.c_str());
    const auto fence = device.submit(commands, &error);
    require(fence && device.fence_complete(fence), error.c_str());
    return fence;
}

void test_multiple_swapchains_and_independent_recreation() {
    NullDevice device;
    MultiSurfacePresenter presenter;
    std::string error;
    std::vector<PresentationSurfaceRequest> requests{surface(1U, 128U, 72U), surface(2U, 96U, 54U)};
    require(presenter.reconcile(device, requests, &error), error.c_str());
    require(presenter.size() == 2U, "multi-surface presenter did not create two swapchains");
    auto statuses = presenter.statuses();
    require(statuses.size() == 2U && statuses[0].generation == 1U && statuses[1].generation == 1U,
            "initial surface generations are wrong");

    const auto acquired = presenter.acquire_all(device, &error);
    require(acquired.size() == 2U, error.c_str());
    std::vector<std::pair<WindowId, FenceHandle>> fences;
    for (const auto& frame : acquired)
        fences.emplace_back(frame.window, prepare_present(device, frame.texture, "multi-window frame", error));
    const auto report = presenter.present_all(device, fences, &error);
    require(report.presented == 2U && report.errors == 0U, error.c_str());

    requests[0].width = 160U;
    requests[0].height = 90U;
    require(presenter.reconcile(device, requests, &error), error.c_str());
    statuses = presenter.statuses();
    require(statuses[0].window == 1U && statuses[0].generation == 2U &&
            statuses[0].width == 160U && statuses[1].window == 2U &&
            statuses[1].generation == 1U,
            "resizing the main window incorrectly recreated the spectator surface");

    require(presenter.mark_out_of_date(2U), "could not mark spectator window out of date");
    require(presenter.reconcile(device, requests, &error), error.c_str());
    statuses = presenter.statuses();
    require(statuses[0].generation == 2U && statuses[1].generation == 2U,
            "out-of-date recovery did not recreate only the requested surface");

    requests.erase(requests.begin() + 1);
    require(presenter.reconcile(device, requests, &error), error.c_str());
    require(presenter.size() == 1U && presenter.statuses()[0].window == 1U,
            "removing a physical-window surface did not preserve the main surface");
    presenter.shutdown(device);
    require(presenter.size() == 0U, "multi-surface presenter did not shut down");
    const auto statistics = device.statistics();
    require(statistics.swapchainsCreated == 4U && statistics.swapchainsDestroyed == 4U &&
            statistics.presents == 2U,
            "multi-surface swapchain lifetime or present counters are wrong");
}

void test_invalid_reconciliation_is_transactional() {
    NullDevice device;
    MultiSurfacePresenter presenter;
    std::string error;
    std::vector<PresentationSurfaceRequest> valid{surface(1U, 64U, 64U)};
    require(presenter.reconcile(device, valid, &error), error.c_str());
    auto invalid = valid;
    invalid.push_back(valid.front());
    require(!presenter.reconcile(device, invalid, &error),
            "duplicate window identities were accepted");
    require(presenter.size() == 1U && presenter.statuses()[0].generation == 1U,
            "failed reconciliation modified the live surface set");
    presenter.shutdown(device);
}

} // namespace

int main() {
    try {
        test_multiple_swapchains_and_independent_recreation();
        test_invalid_reconciliation_is_transactional();
        std::cout << "dve_v221_multi_surface_presenter_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v221_multi_surface_presenter_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
