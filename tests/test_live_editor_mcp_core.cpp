#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "dve/ai/assistant.hpp"
#include "dve/ai/live_editor_mcp.hpp"

namespace {
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::filesystem::path make_temp() {
    const auto path = std::filesystem::temp_directory_path() /
        ("dve-live-mcp-core-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path / "project");
    std::filesystem::create_directories(path / "runtime");
    return path;
}
}

int main() {
#if !defined(__unix__) && !defined(__APPLE__)
    std::cout << "live editor MCP core test skipped: private IPC not implemented on this platform\n";
    return 0;
#else
    try {
        const auto temporary = make_temp();
        dve::ai::DveAiBridge bridge({temporary / "project"});
        dve::ai::LiveEditorMcpHostOptions hostOptions;
        hostOptions.projectRoot = temporary / "project";
        hostOptions.runtimeDirectory = temporary / "runtime";
        hostOptions.requestTimeout = 2s;
        dve::ai::LiveEditorMcpHost host(bridge, hostOptions);
        std::string error;
        require(host.start(&error), error);

        dve::ai::LiveEditorMcpProxyOptions proxyOptions;
        proxyOptions.projectRoot = temporary / "project";
        proxyOptions.runtimeDirectory = temporary / "runtime";
        std::istringstream input(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"core-test\",\"version\":\"1\"}}}\n"
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/read\",\"params\":{\"uri\":\"dve://project/summary\"}}\n");
        std::ostringstream output;
        std::ostringstream diagnostics;
        auto future = std::async(std::launch::async, [&] {
            return dve::ai::run_live_editor_mcp_proxy(proxyOptions, input, output, diagnostics);
        });
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (future.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < deadline) {
            (void)host.pump(8);
            std::this_thread::sleep_for(2ms);
        }
        require(future.wait_for(0ms) == std::future_status::ready, "proxy request did not finish");
        require(future.get() == 0, diagnostics.str());
        require(output.str().find("dve-engine") != std::string::npos, "initialize response missing");
        require(output.str().find("project_root") != std::string::npos, "project resource missing");

        host.stop();
        require(!std::filesystem::exists(host.status().descriptorPath), "descriptor survived shutdown");
        std::filesystem::remove_all(temporary);
        std::cout << "all live editor MCP core tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "live editor MCP core test failed: " << exception.what() << '\n';
        return 1;
    }
#endif
}
