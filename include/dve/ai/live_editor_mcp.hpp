#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>

#include "dve/ai/assistant.hpp"

namespace dve::ai {

inline constexpr std::string_view kLiveEditorMcpDescriptorSchema = "dve.live-editor-mcp/1";

struct LiveEditorMcpHostOptions {
    std::filesystem::path projectRoot;
    // Optional override used by tests and managed installations. When empty, DVE uses
    // $XDG_RUNTIME_DIR/dve/live-editor-mcp on Unix and the platform temporary directory otherwise.
    std::filesystem::path runtimeDirectory;
    AiApprovalPolicy approvalPolicy{AiApprovalPolicy::AskForChanges};
    std::chrono::milliseconds requestTimeout{30'000};
    std::chrono::milliseconds authenticationTimeout{5'000};
    std::size_t maximumMessageBytes{4U << 20U};
    std::size_t maximumPendingRequests{64};
    std::size_t maximumClients{8};
};

struct LiveEditorMcpHostStatus {
    bool supported{};
    bool running{};
    std::string instanceId;
    std::filesystem::path descriptorPath;
    std::filesystem::path endpointPath;
    std::size_t connectedClients{};
    std::size_t pendingRequests{};
    std::uint64_t acceptedConnections{};
    std::uint64_t completedRequests{};
    std::uint64_t rejectedConnections{};
    std::string lastError;
};

// Owns a private local IPC endpoint for the running editor. Socket threads never invoke DVE tools
// directly. pump() must be called by the editor thread; it is the only function that executes MCP
// protocol requests against the bridge and therefore against live EditorWorkspace state.
class LiveEditorMcpHost {
public:
    LiveEditorMcpHost(DveAiBridge& bridge, LiveEditorMcpHostOptions options);
    ~LiveEditorMcpHost();
    LiveEditorMcpHost(const LiveEditorMcpHost&) = delete;
    LiveEditorMcpHost& operator=(const LiveEditorMcpHost&) = delete;

    [[nodiscard]] bool start(std::string* error = nullptr);
    void stop() noexcept;
    [[nodiscard]] std::size_t pump(std::size_t maximumRequests = 16);
    [[nodiscard]] LiveEditorMcpHostStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct LiveEditorMcpProxyOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path runtimeDirectory;
    std::filesystem::path descriptorPath;
    std::chrono::milliseconds connectTimeout{5'000};
    std::chrono::milliseconds responseTimeout{35'000};
    std::size_t maximumMessageBytes{4U << 20U};
};

// Thin stdio-to-private-IPC adapter intended for Claude Code/Desktop and other stdio MCP clients.
// It emits only MCP responses on output; diagnostics are written to diagnostics.
[[nodiscard]] int run_live_editor_mcp_proxy(const LiveEditorMcpProxyOptions& options,
                                             std::istream& input,
                                             std::ostream& output,
                                             std::ostream& diagnostics);

[[nodiscard]] std::filesystem::path live_editor_mcp_runtime_directory(
    const std::filesystem::path& overrideDirectory = {});
[[nodiscard]] std::filesystem::path live_editor_mcp_descriptor_path(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& runtimeDirectory = {});

} // namespace dve::ai
