#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "dve/ai/json.hpp"
#include "dve/ai/live_editor_mcp.hpp"
#include "dve/editor_native.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::filesystem::path make_temp() {
    const auto path = std::filesystem::temp_directory_path() /
        ("dve-live-mcp-test-" + std::to_string(
#if defined(__unix__) || defined(__APPLE__)
            static_cast<unsigned long long>(::getpid())
#else
            1ULL
#endif
        ) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path / "project");
    std::filesystem::create_directories(path / "runtime");
    return path;
}

#if defined(__unix__) || defined(__APPLE__)

void write_line(int socket, std::string_view line) {
    std::string framed(line);
    framed.push_back('\n');
    std::size_t offset = 0;
    while (offset < framed.size()) {
        const ssize_t written = ::send(socket, framed.data() + offset, framed.size() - offset, 0);
        if (written <= 0) throw std::runtime_error("socket write failed");
        offset += static_cast<std::size_t>(written);
    }
}

std::string read_line(int socket, int timeoutMs = 3000) {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{socket, POLLIN, 0};
        const int polled = ::poll(&descriptor, 1, 20);
        if (polled < 0) throw std::runtime_error("socket poll failed");
        if (polled == 0) continue;
        char character{};
        const ssize_t received = ::recv(socket, &character, 1, 0);
        if (received <= 0) throw std::runtime_error("socket closed while reading");
        if (character == '\n') return line;
        if (character != '\r') line.push_back(character);
    }
    throw std::runtime_error("socket read deadline exceeded");
}

struct Connection {
    int socket{-1};
    ~Connection() { if (socket >= 0) ::close(socket); }
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept : socket(std::exchange(other.socket, -1)) {}
    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            if (socket >= 0) ::close(socket);
            socket = std::exchange(other.socket, -1);
        }
        return *this;
    }
};


Connection connect_without_auth(const std::filesystem::path& endpoint) {
    Connection connection;
    connection.socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(connection.socket >= 0, "could not create unauthenticated client socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(endpoint.native().size() < sizeof(address.sun_path), "test endpoint path is too long");
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.native().size() + 1U);
    require(::connect(connection.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "could not connect unauthenticated client to live editor host");
    return connection;
}

int error_code(std::string_view response) {
    const auto parsed = dve::ai::parse_json(response);
    const auto* error = parsed.value ? parsed.value->find("error") : nullptr;
    const auto* code = error && error->is_object() ? error->find("code") : nullptr;
    require(code && code->is_number(), "expected JSON-RPC error code");
    return static_cast<int>(code->as_number());
}

Connection connect_to(const std::filesystem::path& endpoint, std::string_view token) {
    Connection connection;
    connection.socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(connection.socket >= 0, "could not create client socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(endpoint.native().size() < sizeof(address.sun_path), "test endpoint path is too long");
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.native().size() + 1U);
    require(::connect(connection.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "could not connect to live editor host");
    dve::ai::JsonValue auth = dve::ai::JsonValue::Object{
        {"schema", "dve.live-editor-mcp-auth/1"}, {"token", std::string(token)}, {"client", "test"}};
    write_line(connection.socket, dve::ai::stringify_json(auth, false));
    const auto acknowledgement = dve::ai::parse_json(read_line(connection.socket));
    require(acknowledgement.value && acknowledgement.value->find("ok") &&
            acknowledgement.value->find("ok")->as_bool(), "live editor authentication failed");
    return connection;
}

template <typename Pump>
std::string request(Connection& connection, std::string message, Pump&& pump) {
    write_line(connection.socket, message);
    auto future = std::async(std::launch::async, [&connection] { return read_line(connection.socket); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (future.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < deadline) {
        pump();
        std::this_thread::sleep_for(2ms);
    }
    require(future.wait_for(0ms) == std::future_status::ready, "live MCP request did not complete");
    return future.get();
}

void pump_notification(Connection& connection, std::string message, dve::editor::NativeEditorController& controller) {
    write_line(connection.socket, message);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        controller.update(0.0F);
        if (controller.live_mcp_status().pendingRequests == 0) break;
        std::this_thread::sleep_for(2ms);
    }
}

void initialize(Connection& connection, dve::editor::NativeEditorController& controller, int id) {
    const std::string response = request(connection,
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}",
        [&] { controller.update(0.0F); });
    const auto parsed = dve::ai::parse_json(response);
    require(parsed.value && parsed.value->find("result"), "initialize failed");
    pump_notification(connection,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", controller);
}

#endif

} // namespace

int main() {
#if !defined(__unix__) && !defined(__APPLE__)
    std::cout << "live editor MCP test skipped: private IPC not implemented on this platform\n";
    return 0;
#else
    try {
        const auto temporary = make_temp();
        auto document = dve::editor::make_native_editor_demo_document();
        dve::editor::NativeEditorController controller{dve::editor::EditorWorkspace(std::move(document))};
        controller.configure_ai_assistant(temporary / "project");
        dve::ai::LiveEditorMcpHostOptions options;
        options.projectRoot = temporary / "project";
        options.runtimeDirectory = temporary / "runtime";
        options.requestTimeout = 2s;
        std::string error;
        require(controller.start_live_mcp_host(options, &error), error);
        const auto status = controller.live_mcp_status();
        require(status.running && std::filesystem::exists(status.descriptorPath), "host descriptor missing");
        struct stat descriptorStat{};
        require(::stat(status.descriptorPath.c_str(), &descriptorStat) == 0, "could not stat descriptor");
        require((descriptorStat.st_mode & 0777) == 0600, "descriptor permissions are not owner-only");

        std::ifstream descriptorStream(status.descriptorPath);
        std::stringstream descriptorText;
        descriptorText << descriptorStream.rdbuf();
        const auto descriptor = dve::ai::parse_json(descriptorText.str());
        require(descriptor.value.has_value(), "descriptor JSON could not be parsed");
        const std::string token(descriptor.value->find("auth_token")->as_string());
        const std::filesystem::path endpoint(descriptor.value->find("endpoint")->as_string());

        {
            Connection rejected = connect_without_auth(endpoint);
            write_line(rejected.socket, dve::ai::stringify_json(dve::ai::JsonValue::Object{
                {"schema", "dve.live-editor-mcp-auth/1"}, {"token", "wrong-token"}, {"client", "test"}}, false));
            const auto rejection = dve::ai::parse_json(read_line(rejected.socket));
            require(rejection.value && rejection.value->find("ok") &&
                    !rejection.value->find("ok")->as_bool(), "invalid bearer token was accepted");
        }

        {
            Connection lifecycle = connect_to(endpoint, token);
            const auto beforeInitialize = request(lifecycle,
                R"({"jsonrpc":"2.0","id":90,"method":"tools/list","params":{}})",
                [&] { controller.update(0.0F); });
            require(error_code(beforeInitialize) == -32002, "operation before initialize was not rejected");

            const auto unsupported = request(lifecycle,
                R"({"jsonrpc":"2.0","id":91,"method":"initialize","params":{"protocolVersion":"1900-01-01","capabilities":{},"clientInfo":{"name":"bad-version","version":"1"}}})",
                [&] { controller.update(0.0F); });
            require(error_code(unsupported) == -32602, "unsupported protocol version was not rejected");

            const auto validInitialize = request(lifecycle,
                R"({"jsonrpc":"2.0","id":92,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"lifecycle-test","version":"1"}}})",
                [&] { controller.update(0.0F); });
            const auto initializedResponse = dve::ai::parse_json(validInitialize);
            require(initializedResponse.value && initializedResponse.value->find("result"),
                    "valid initialize after version rejection failed");

            const auto beforeNotification = request(lifecycle,
                R"({"jsonrpc":"2.0","id":93,"method":"ping","params":{}})",
                [&] { controller.update(0.0F); });
            require(error_code(beforeNotification) == -32002,
                    "operation before initialized notification was not rejected");
            pump_notification(lifecycle,
                R"({"jsonrpc":"2.0","method":"notifications/initialized"})", controller);
            const auto afterNotification = request(lifecycle,
                R"({"jsonrpc":"2.0","id":94,"method":"ping","params":{}})",
                [&] { controller.update(0.0F); });
            const auto ping = dve::ai::parse_json(afterNotification);
            require(ping.value && ping.value->find("result"), "session did not become operational after initialization");
        }

        Connection clientA = connect_to(endpoint, token);
        initialize(clientA, controller, 1);
        const auto contextText = request(clientA,
            R"({"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"dve://editor/context"}})",
            [&] { controller.update(0.0F); });
        const auto context = dve::ai::parse_json(contextText);
        require(context.value && context.value->find("result"), "live editor context resource failed");

        const auto objectId = controller.workspace().document().objects().begin()->first;
        const std::string renameRequest =
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"dve.editor.rename_object\",\"arguments\":{\"id\":" +
            std::to_string(objectId) + ",\"name\":\"Renamed Through Live MCP\"}}}";
        const auto pendingText = request(clientA, renameRequest, [&] { controller.update(0.0F); });
        const auto pending = dve::ai::parse_json(pendingText);
        const auto* pendingResult = pending.value ? pending.value->find("result") : nullptr;
        const auto* meta = pendingResult ? pendingResult->find("_meta") : nullptr;
        require(meta && meta->find("dve/approvalId"), "mutation did not produce approval metadata");
        const std::string approvalId(meta->find("dve/approvalId")->as_string());
        controller.update(0.0F);
        const auto& visibleApprovals = controller.workspace().ai_assistant().pending_approvals();
        require(std::any_of(visibleApprovals.begin(), visibleApprovals.end(),
                            [&](const auto& item) { return item.id == approvalId; }),
                "external MCP proposal was not surfaced in the editor review queue");
        require(controller.workspace().ai_assistant().approve(approvalId),
                "editor review queue could not approve the external MCP proposal");

        Connection clientB = connect_to(endpoint, token);
        initialize(clientB, controller, 10);
        const std::string crossClientReplay =
            "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"dve.editor.rename_object\",\"arguments\":{\"id\":" +
            std::to_string(objectId) + ",\"name\":\"Renamed Through Live MCP\",\"_dve_approval_id\":\"" + approvalId + "\"}}}";
        const auto crossText = request(clientB, crossClientReplay, [&] { controller.update(0.0F); });
        const auto cross = dve::ai::parse_json(crossText);
        const auto* crossResult = cross.value ? cross.value->find("result") : nullptr;
        require(crossResult && crossResult->find("_meta"), "cross-client approval replay was not rejected");
        require(controller.workspace().document().find_object(objectId)->name != "Renamed Through Live MCP",
                "cross-client approval replay changed the live document");

        const std::string approvedReplay =
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"dve.editor.rename_object\",\"arguments\":{\"id\":" +
            std::to_string(objectId) + ",\"name\":\"Renamed Through Live MCP\",\"_dve_approval_id\":\"" + approvalId + "\"}}}";
        const auto appliedText = request(clientA, approvedReplay, [&] { controller.update(0.0F); });
        const auto applied = dve::ai::parse_json(appliedText);
        require(applied.value && applied.value->find("result") &&
                !applied.value->find("result")->find("isError")->as_bool(), "approved live mutation failed");
        require(controller.workspace().document().find_object(objectId)->name == "Renamed Through Live MCP",
                "approved live mutation did not reach the open document");
        require(controller.workspace().commands().can_undo(), "live mutation bypassed the editor undo stack");

        dve::ai::LiveEditorMcpProxyOptions proxyOptions;
        proxyOptions.projectRoot = temporary / "project";
        proxyOptions.runtimeDirectory = temporary / "runtime";
        std::istringstream proxyInput(
            "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"proxy-test\",\"version\":\"1\"}}}\n"
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"resources/read\",\"params\":{\"uri\":\"dve://editor/live-mcp\"}}\n");
        std::ostringstream proxyOutput;
        std::ostringstream proxyDiagnostics;
        auto proxyFuture = std::async(std::launch::async, [&] {
            return dve::ai::run_live_editor_mcp_proxy(proxyOptions, proxyInput, proxyOutput, proxyDiagnostics);
        });
        const auto proxyDeadline = std::chrono::steady_clock::now() + 4s;
        while (proxyFuture.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < proxyDeadline) {
            controller.update(0.0F);
            std::this_thread::sleep_for(2ms);
        }
        require(proxyFuture.wait_for(0ms) == std::future_status::ready, "stdio proxy did not complete");
        require(proxyFuture.get() == 0, proxyDiagnostics.str());
        require(proxyOutput.str().find("live-editor-mcp") != std::string::npos ||
                proxyOutput.str().find("running") != std::string::npos,
                "stdio proxy did not return live-host resource data");
        require(proxyOutput.str().find(token) == std::string::npos &&
                proxyDiagnostics.str().find(token) == std::string::npos,
                "stdio proxy exposed the private live-host token");

        controller.stop_live_mcp_host();
        require(!std::filesystem::exists(status.descriptorPath), "host descriptor survived clean shutdown");
        std::filesystem::remove_all(temporary);
        std::cout << "all live editor MCP tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "live editor MCP test failed: " << exception.what() << '\n';
        return 1;
    }
#endif
}
