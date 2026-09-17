#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "dve/ai/live_editor_mcp.hpp"

int main(int argc, char** argv) {
    dve::ai::LiveEditorMcpProxyOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--project-root" && index + 1 < argc) options.projectRoot = argv[++index];
        else if (argument == "--runtime-dir" && index + 1 < argc) options.runtimeDirectory = argv[++index];
        else if (argument == "--descriptor" && index + 1 < argc) options.descriptorPath = argv[++index];
        else if (argument == "--connect-timeout-ms" && index + 1 < argc)
            options.connectTimeout = std::chrono::milliseconds(std::stoll(argv[++index]));
        else if (argument == "--response-timeout-ms" && index + 1 < argc)
            options.responseTimeout = std::chrono::milliseconds(std::stoll(argv[++index]));
        else if (argument == "--help") {
            std::cerr << "dve_live_editor_mcp_proxy [--project-root PATH] [--runtime-dir PATH] "
                         "[--descriptor FILE] [--connect-timeout-ms N] [--response-timeout-ms N]\n";
            return 0;
        } else {
            std::cerr << "dve_live_editor_mcp_proxy: unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }
    return dve::ai::run_live_editor_mcp_proxy(options, std::cin, std::cout, std::cerr);
}
