#include "dve/ai/live_editor_mcp.hpp"

#include "dve/ai/json.hpp"
#include "dve/ai/mcp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace dve::ai {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view kAuthSchema = "dve.live-editor-mcp-auth/1";
constexpr std::size_t kMaximumDescriptorBytes = 64U << 10U;

std::filesystem::path canonical_or_absolute(const std::filesystem::path& path) {
    std::error_code error;
    auto value = std::filesystem::weakly_canonical(path, error);
    if (!error) return value;
    value = std::filesystem::absolute(path, error);
    return error ? path : value;
}

std::string random_hex(std::size_t bytes) {
    std::random_device random;
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes; ++index) {
        const auto value = static_cast<unsigned>(random() & 0xFFU);
        stream << std::setw(2) << value;
    }
    return stream.str();
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    const std::size_t maximum = std::max(left.size(), right.size());
    unsigned difference = static_cast<unsigned>(left.size() ^ right.size());
    for (std::size_t index = 0; index < maximum; ++index) {
        const unsigned a = index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
        const unsigned b = index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= a ^ b;
    }
    return difference == 0U;
}

std::string json_rpc_error_for(std::string_view request, int code, std::string message) {
    JsonValue id(nullptr);
    const auto parsed = parse_json(request);
    if (parsed.value && parsed.value->is_object()) {
        if (const auto* candidate = parsed.value->find("id")) id = *candidate;
    }
    return stringify_json(JsonValue::Object{
        {"jsonrpc", "2.0"},
        {"id", std::move(id)},
        {"error", JsonValue::Object{{"code", code}, {"message", std::move(message)}}}
    }, false);
}

bool request_has_id(std::string_view request) {
    const auto parsed = parse_json(request);
    return parsed.value && parsed.value->is_object() && parsed.value->find("id") != nullptr;
}

std::optional<JsonValue> read_json_file(const std::filesystem::path& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error) *error = "could not open live-editor MCP descriptor: " + path.string();
        return std::nullopt;
    }
    std::string text;
    text.resize(kMaximumDescriptorBytes + 1U);
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto bytes = static_cast<std::size_t>(stream.gcount());
    if (!stream.eof() || bytes > kMaximumDescriptorBytes) {
        if (error) *error = "live-editor MCP descriptor exceeds the size limit";
        return std::nullopt;
    }
    text.resize(bytes);
    const auto parsed = parse_json(text);
    if (!parsed.value || !parsed.value->is_object()) {
        if (error) *error = "live-editor MCP descriptor is invalid JSON";
        return std::nullopt;
    }
    return std::move(parsed.value);
}

bool write_descriptor_atomic(const std::filesystem::path& path, const JsonValue& descriptor,
                             std::string* error) {
    const auto temporary = path.string() + ".tmp-" + random_hex(6);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error) *error = "could not create live-editor MCP descriptor";
            return false;
        }
        stream << stringify_json(descriptor, true) << '\n';
        stream.flush();
        if (!stream) {
            if (error) *error = "could not write live-editor MCP descriptor";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
#if defined(__unix__) || defined(__APPLE__)
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        if (error) *error = "could not restrict live-editor MCP descriptor permissions";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
#endif
    std::error_code filesystemError;
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(path, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(temporary, path, filesystemError);
    }
    if (filesystemError) {
        if (error) *error = "could not publish live-editor MCP descriptor: " + filesystemError.message();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::string project_key(const std::filesystem::path& projectRoot) {
    return to_hex(fnv1a64(canonical_or_absolute(projectRoot).generic_string()));
}

#if defined(__unix__) || defined(__APPLE__)

struct FileDescriptor {
    int value{-1};
    FileDescriptor() = default;
    explicit FileDescriptor(int descriptor) : value(descriptor) {}
    ~FileDescriptor() { reset(); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value(std::exchange(other.value, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value = std::exchange(other.value, -1);
        }
        return *this;
    }
    void reset(int descriptor = -1) noexcept {
        if (value >= 0) ::close(value);
        value = descriptor;
    }
    [[nodiscard]] int release() noexcept { return std::exchange(value, -1); }
    [[nodiscard]] explicit operator bool() const noexcept { return value >= 0; }
};

struct LineReadResult {
    bool ok{};
    bool eof{};
    bool timedOut{};
    bool tooLarge{};
    std::string line;
    std::string error;
};

class SocketLineReader {
public:
    explicit SocketLineReader(int socket) : socket_(socket) {}

    LineReadResult read(std::stop_token stopToken, std::size_t maximumBytes,
                        std::optional<Clock::time_point> deadline = std::nullopt) {
        LineReadResult result;
        for (;;) {
            if (const auto newline = buffer_.find('\n'); newline != std::string::npos) {
                if (newline > maximumBytes) {
                    result.tooLarge = true;
                    result.error = "message exceeds the size limit";
                    buffer_.erase(0, newline + 1U);
                    return result;
                }
                result.line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1U);
                if (!result.line.empty() && result.line.back() == '\r') result.line.pop_back();
                result.ok = true;
                return result;
            }
            if (buffer_.size() > maximumBytes) {
                result.tooLarge = true;
                result.error = "message exceeds the size limit";
                return result;
            }
            if (stopToken.stop_requested()) {
                result.error = "stopping";
                return result;
            }
            int timeout = 100;
            if (deadline) {
                const auto now = Clock::now();
                if (now >= *deadline) {
                    result.timedOut = true;
                    result.error = "deadline exceeded";
                    return result;
                }
                timeout = static_cast<int>(std::clamp<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count(), 1, 100));
            }
            pollfd descriptor{socket_, POLLIN, 0};
            const int polled = ::poll(&descriptor, 1, timeout);
            if (polled < 0) {
                if (errno == EINTR) continue;
                result.error = std::string("poll failed: ") + std::strerror(errno);
                return result;
            }
            if (polled == 0) continue;
            if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                result.error = "socket read failed";
                return result;
            }
            char incoming[4096];
            const ssize_t received = ::recv(socket_, incoming, sizeof(incoming), 0);
            if (received == 0) {
                result.eof = true;
                return result;
            }
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                result.error = std::string("recv failed: ") + std::strerror(errno);
                return result;
            }
            buffer_.append(incoming, static_cast<std::size_t>(received));
        }
    }

private:
    int socket_{};
    std::string buffer_;
};

bool write_socket_line(int socket, std::string_view line, std::chrono::milliseconds timeout,
                       std::string* error) {
    std::string framed(line);
    framed.push_back('\n');
    std::size_t offset = 0;
    const auto deadline = Clock::now() + timeout;
    while (offset < framed.size()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            if (error) *error = "socket write deadline exceeded";
            return false;
        }
        pollfd descriptor{socket, POLLOUT, 0};
        const int wait = static_cast<int>(std::clamp<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 1, 100));
        const int polled = ::poll(&descriptor, 1, wait);
        if (polled < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::string("poll failed: ") + std::strerror(errno);
            return false;
        }
        if (polled == 0) continue;
#ifdef MSG_NOSIGNAL
        const ssize_t written = ::send(socket, framed.data() + offset, framed.size() - offset, MSG_NOSIGNAL);
#else
        const ssize_t written = ::send(socket, framed.data() + offset, framed.size() - offset, 0);
#endif
        if (written < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (error) *error = std::string("send failed: ") + std::strerror(errno);
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool same_effective_user(int socket) noexcept {
#if defined(__linux__) && defined(SO_PEERCRED)
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) return false;
    return credentials.uid == ::geteuid();
#else
    (void)socket;
    return true;
#endif
}

std::optional<int> connect_unix_socket(const std::filesystem::path& endpoint,
                                       std::chrono::milliseconds timeout,
                                       std::string* error) {
    if (endpoint.native().size() >= sizeof(sockaddr_un::sun_path)) {
        if (error) *error = "live-editor MCP endpoint path is too long";
        return std::nullopt;
    }
    FileDescriptor socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socket) {
        if (error) *error = std::string("socket creation failed: ") + std::strerror(errno);
        return std::nullopt;
    }
    const int originalFlags = ::fcntl(socket.value, F_GETFL, 0);
    if (originalFlags >= 0) (void)::fcntl(socket.value, F_SETFL, originalFlags | O_NONBLOCK);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.native().size() + 1U);
    const int connected = ::connect(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected != 0 && errno != EINPROGRESS) {
        if (error) *error = std::string("could not connect to running DVE editor: ") + std::strerror(errno);
        return std::nullopt;
    }
    if (connected != 0) {
        pollfd descriptor{socket.value, POLLOUT, 0};
        const int polled = ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, timeout.count())));
        if (polled <= 0) {
            if (error) *error = polled == 0 ? "timed out connecting to running DVE editor"
                                            : std::string("connect poll failed: ") + std::strerror(errno);
            return std::nullopt;
        }
        int socketError = 0;
        socklen_t length = sizeof(socketError);
        if (::getsockopt(socket.value, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError != 0) {
            if (error) *error = std::string("could not connect to running DVE editor: ") +
                                std::strerror(socketError == 0 ? errno : socketError);
            return std::nullopt;
        }
    }
    if (originalFlags >= 0) (void)::fcntl(socket.value, F_SETFL, originalFlags);
    return socket.release();
}

#endif

} // namespace

std::filesystem::path live_editor_mcp_runtime_directory(const std::filesystem::path& overrideDirectory) {
    if (!overrideDirectory.empty()) return canonical_or_absolute(overrideDirectory);
#if defined(__unix__) || defined(__APPLE__)
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime != '\0')
        return std::filesystem::path(runtime) / "dve" / "live-editor-mcp";
    return std::filesystem::temp_directory_path() /
           ("dve-" + std::to_string(static_cast<unsigned long long>(::geteuid()))) /
           "live-editor-mcp";
#else
    return std::filesystem::temp_directory_path() / "dve" / "live-editor-mcp";
#endif
}

std::filesystem::path live_editor_mcp_descriptor_path(const std::filesystem::path& projectRoot,
                                                       const std::filesystem::path& runtimeDirectory) {
    return live_editor_mcp_runtime_directory(runtimeDirectory) / (project_key(projectRoot) + ".json");
}

class LiveEditorMcpHost::Impl {
public:
    Impl(DveAiBridge& bridgeValue, LiveEditorMcpHostOptions hostOptions)
        : bridge(bridgeValue), options(std::move(hostOptions)) {
        if (options.projectRoot.empty()) options.projectRoot = bridge.project_root();
        options.projectRoot = canonical_or_absolute(options.projectRoot);
        options.maximumMessageBytes = std::max<std::size_t>(1024, options.maximumMessageBytes);
        options.maximumPendingRequests = std::max<std::size_t>(1, options.maximumPendingRequests);
        options.maximumClients = std::max<std::size_t>(1, options.maximumClients);
        options.requestTimeout = std::max(options.requestTimeout, std::chrono::milliseconds(100));
        options.authenticationTimeout = std::max(options.authenticationTimeout, std::chrono::milliseconds(100));
    }

    ~Impl() { stop(); }

    struct Session {
        explicit Session(DveAiBridge& bridge, AiApprovalPolicy policy) : protocol(bridge, policy) {}
        DveMcpProtocol protocol;
    };

    struct Request {
        std::shared_ptr<Session> session;
        std::string message;
        std::promise<std::string> promise;
        std::atomic_bool completed{};
        std::atomic_bool cancelled{};

        void fulfill(std::string response) noexcept {
            bool expected = false;
            if (!completed.compare_exchange_strong(expected, true)) return;
            try { promise.set_value(std::move(response)); } catch (...) {}
        }
    };

    bool start(std::string* error) {
#if !defined(__unix__) && !defined(__APPLE__)
        const std::string message = "live-editor MCP host is not implemented on this platform";
        set_error(message);
        if (error) *error = message;
        return false;
#else
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) return true;
        stopping.store(false);
        ownerThread = std::this_thread::get_id();
        instanceId = random_hex(16);
        authToken = random_hex(32);
        runtimeDirectory = live_editor_mcp_runtime_directory(options.runtimeDirectory);
        descriptorPath = live_editor_mcp_descriptor_path(options.projectRoot, options.runtimeDirectory);

        std::error_code filesystemError;
        std::filesystem::create_directories(runtimeDirectory, filesystemError);
        if (filesystemError) return fail_start("could not create live-editor MCP runtime directory: " + filesystemError.message(), error);
        if (::chmod(runtimeDirectory.c_str(), S_IRWXU) != 0)
            return fail_start("could not restrict live-editor MCP runtime directory permissions", error);

        const std::string key = project_key(options.projectRoot);
        endpointPath = runtimeDirectory /
            (key.substr(0, std::min<std::size_t>(12, key.size())) + "-" +
             std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
             instanceId.substr(0, 8) + ".sock");
        if (endpointPath.native().size() >= sizeof(sockaddr_un::sun_path)) {
            const auto fallback = std::filesystem::temp_directory_path() /
                ("dve-" + std::to_string(static_cast<unsigned long long>(::geteuid())));
            std::filesystem::create_directories(fallback, filesystemError);
            if (filesystemError) return fail_start("could not create compact MCP endpoint directory", error);
            if (::chmod(fallback.c_str(), S_IRWXU) != 0)
                return fail_start("could not restrict compact MCP endpoint directory", error);
            endpointPath = fallback / ("mcp-" + key.substr(0, 10) + "-" + instanceId.substr(0, 8) + ".sock");
        }

        FileDescriptor listener(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!listener) return fail_start(std::string("MCP socket creation failed: ") + std::strerror(errno), error);
        (void)::fcntl(listener.value, F_SETFD, FD_CLOEXEC);
        std::filesystem::remove(endpointPath, filesystemError);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, endpointPath.c_str(), endpointPath.native().size() + 1U);
        if (::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            return fail_start(std::string("MCP socket bind failed: ") + std::strerror(errno), error);
        if (::chmod(endpointPath.c_str(), S_IRUSR | S_IWUSR) != 0)
            return fail_start("could not restrict live-editor MCP socket permissions", error);
        if (::listen(listener.value, static_cast<int>(options.maximumClients)) != 0)
            return fail_start(std::string("MCP socket listen failed: ") + std::strerror(errno), error);
        listenSocket = listener.release();

        const JsonValue descriptor = JsonValue::Object{
            {"schema", std::string(kLiveEditorMcpDescriptorSchema)},
            {"instance_id", instanceId},
            {"pid", static_cast<double>(::getpid())},
            {"project_root", options.projectRoot.generic_string()},
            {"endpoint_kind", "unix-domain-socket"},
            {"endpoint", endpointPath.generic_string()},
            {"auth_token", authToken},
            {"protocol_version", "2025-11-25"},
            {"maximum_message_bytes", static_cast<double>(options.maximumMessageBytes)}
        };
        std::string descriptorError;
        if (!write_descriptor_atomic(descriptorPath, descriptor, &descriptorError))
            return fail_start(descriptorError, error);

        acceptThread = std::jthread([this](std::stop_token token) { accept_loop(token); });
        clear_error();
        return true;
#endif
    }

    void stop() noexcept {
        if (!running.exchange(false)) return;
        stopping.store(true);
#if defined(__unix__) || defined(__APPLE__)
        if (listenSocket >= 0) {
            (void)::shutdown(listenSocket, SHUT_RDWR);
            ::close(listenSocket);
            listenSocket = -1;
        }
        if (acceptThread.joinable()) {
            acceptThread.request_stop();
            acceptThread.join();
        }
        {
            std::lock_guard lock(clientMutex);
            for (const int descriptor : clientSockets) (void)::shutdown(descriptor, SHUT_RDWR);
            for (auto& thread : clientThreads) thread.request_stop();
        }
        std::vector<std::jthread> threads;
        {
            std::lock_guard lock(clientMutex);
            threads = std::move(clientThreads);
        }
        for (auto& thread : threads) if (thread.joinable()) thread.join();
#endif
        std::deque<std::shared_ptr<Request>> abandoned;
        {
            std::lock_guard lock(queueMutex);
            abandoned.swap(pending);
        }
        for (auto& request : abandoned) {
            request->cancelled.store(true);
            request->fulfill(json_rpc_error_for(request->message, -32004, "DVE live-editor MCP host stopped"));
        }
        cleanup_endpoint();
        connectedClients.store(0);
    }

    std::size_t pump(std::size_t maximumRequests) {
        if (!running.load() || maximumRequests == 0) return 0;
        if (ownerThread != std::this_thread::get_id()) {
            set_error("live-editor MCP pump called from a non-owner thread");
            return 0;
        }
        std::size_t processed = 0;
        while (processed < maximumRequests) {
            std::shared_ptr<Request> request;
            {
                std::lock_guard lock(queueMutex);
                if (pending.empty()) break;
                request = std::move(pending.front());
                pending.pop_front();
            }
            if (!request->cancelled.load()) {
                try {
                    request->fulfill(request->session->protocol.handle(request->message));
                } catch (const std::exception& exception) {
                    request->fulfill(json_rpc_error_for(request->message, -32603, exception.what()));
                } catch (...) {
                    request->fulfill(json_rpc_error_for(request->message, -32603, "unknown live-editor MCP failure"));
                }
                completedRequests.fetch_add(1);
            } else {
                request->fulfill({});
            }
            ++processed;
        }
        return processed;
    }

    LiveEditorMcpHostStatus status() const {
        LiveEditorMcpHostStatus value;
#if defined(__unix__) || defined(__APPLE__)
        value.supported = true;
#endif
        value.running = running.load();
        value.instanceId = instanceId;
        value.descriptorPath = descriptorPath;
        value.endpointPath = endpointPath;
        value.connectedClients = connectedClients.load();
        {
            std::lock_guard lock(queueMutex);
            value.pendingRequests = pending.size();
        }
        value.acceptedConnections = acceptedConnections.load();
        value.completedRequests = completedRequests.load();
        value.rejectedConnections = rejectedConnections.load();
        {
            std::lock_guard lock(stateMutex);
            value.lastError = lastError;
        }
        return value;
    }

private:
#if defined(__unix__) || defined(__APPLE__)
    void accept_loop(std::stop_token stopToken) {
        while (!stopToken.stop_requested() && !stopping.load()) {
            pollfd descriptor{listenSocket, POLLIN, 0};
            const int polled = ::poll(&descriptor, 1, 100);
            if (polled < 0) {
                if (errno == EINTR || stopping.load()) continue;
                set_error(std::string("live-editor MCP accept poll failed: ") + std::strerror(errno));
                break;
            }
            if (polled == 0) continue;
            const int client = ::accept(listenSocket, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR || stopping.load()) continue;
                set_error(std::string("live-editor MCP accept failed: ") + std::strerror(errno));
                continue;
            }
            (void)::fcntl(client, F_SETFD, FD_CLOEXEC);
            if (!same_effective_user(client) || connectedClients.load() >= options.maximumClients) {
                rejectedConnections.fetch_add(1);
                ::close(client);
                continue;
            }
            connectedClients.fetch_add(1);
            acceptedConnections.fetch_add(1);
            std::lock_guard lock(clientMutex);
            clientSockets.insert(client);
            clientThreads.emplace_back([this, client](std::stop_token token) { client_loop(client, token); });
        }
    }

    void client_loop(int client, std::stop_token stopToken) noexcept {
        FileDescriptor socket(client);
        const auto finish = [&] {
            std::lock_guard lock(clientMutex);
            clientSockets.erase(client);
            connectedClients.fetch_sub(1);
        };
        try {
            SocketLineReader reader(client);
            const auto auth = reader.read(stopToken, 8192,
                Clock::now() + options.authenticationTimeout);
            if (!auth.ok) {
                rejectedConnections.fetch_add(1);
                finish();
                return;
            }
            const auto parsed = parse_json(auth.line);
            const auto* schema = parsed.value && parsed.value->is_object() ? parsed.value->find("schema") : nullptr;
            const auto* token = parsed.value && parsed.value->is_object() ? parsed.value->find("token") : nullptr;
            if (!schema || !schema->is_string() || schema->as_string() != kAuthSchema ||
                !token || !token->is_string() || !constant_time_equal(token->as_string(), authToken)) {
                std::string ignored;
                (void)write_socket_line(client,
                    stringify_json(JsonValue::Object{{"ok", false}, {"error", "authentication failed"}}, false),
                    std::chrono::milliseconds(500), &ignored);
                rejectedConnections.fetch_add(1);
                finish();
                return;
            }
            std::string writeError;
            if (!write_socket_line(client,
                    stringify_json(JsonValue::Object{{"ok", true}, {"instance_id", instanceId}}, false),
                    std::chrono::milliseconds(500), &writeError)) {
                finish();
                return;
            }

            auto session = std::make_shared<Session>(bridge, options.approvalPolicy);
            while (!stopToken.stop_requested() && !stopping.load()) {
                const auto incoming = reader.read(stopToken, options.maximumMessageBytes);
                if (incoming.eof || stopToken.stop_requested() || stopping.load()) break;
                if (!incoming.ok) {
                    if (incoming.tooLarge) {
                        (void)write_socket_line(client,
                            json_rpc_error_for({}, -32005, "MCP message exceeds the configured size limit"),
                            std::chrono::milliseconds(500), &writeError);
                    }
                    break;
                }
                auto request = std::make_shared<Request>();
                request->session = session;
                request->message = incoming.line;
                auto future = request->promise.get_future();
                {
                    std::lock_guard lock(queueMutex);
                    if (pending.size() >= options.maximumPendingRequests) {
                        rejectedConnections.fetch_add(1);
                        (void)write_socket_line(client,
                            json_rpc_error_for(incoming.line, -32006, "DVE live-editor MCP request queue is full"),
                            std::chrono::milliseconds(500), &writeError);
                        break;
                    }
                    pending.push_back(request);
                }
                if (future.wait_for(options.requestTimeout) != std::future_status::ready) {
                    request->cancelled.store(true);
                    if (request_has_id(incoming.line)) {
                        (void)write_socket_line(client,
                            json_rpc_error_for(incoming.line, -32001, "DVE editor-thread request deadline exceeded"),
                            std::chrono::milliseconds(500), &writeError);
                    }
                    break;
                }
                const std::string response = future.get();
                if (!response.empty() && !write_socket_line(client, response,
                        std::chrono::milliseconds(2'000), &writeError)) break;
            }
        } catch (...) {}
        finish();
    }
#endif

    bool fail_start(std::string message, std::string* error) {
        set_error(message);
        if (error) *error = std::move(message);
#if defined(__unix__) || defined(__APPLE__)
        if (listenSocket >= 0) {
            ::close(listenSocket);
            listenSocket = -1;
        }
#endif
        cleanup_endpoint();
        running.store(false);
        return false;
    }

    void cleanup_endpoint() noexcept {
        try {
            std::error_code ignored;
            if (!endpointPath.empty()) std::filesystem::remove(endpointPath, ignored);
            if (!descriptorPath.empty()) {
                std::string descriptorError;
                const auto descriptor = read_json_file(descriptorPath, &descriptorError);
                const auto* owner = descriptor && descriptor->is_object() ? descriptor->find("instance_id") : nullptr;
                if (owner && owner->is_string() && owner->as_string() == instanceId)
                    std::filesystem::remove(descriptorPath, ignored);
            }
        } catch (...) {
            // Shutdown cleanup is best-effort and must never escape a noexcept destructor path.
        }
    }

    void set_error(std::string message) {
        std::lock_guard lock(stateMutex);
        lastError = std::move(message);
    }
    void clear_error() {
        std::lock_guard lock(stateMutex);
        lastError.clear();
    }

    DveAiBridge& bridge;
    LiveEditorMcpHostOptions options;
    std::thread::id ownerThread{};
    std::atomic_bool running{};
    std::atomic_bool stopping{};
    std::string instanceId;
    std::string authToken;
    std::filesystem::path runtimeDirectory;
    std::filesystem::path descriptorPath;
    std::filesystem::path endpointPath;
    mutable std::mutex stateMutex;
    std::string lastError;
    mutable std::mutex queueMutex;
    std::deque<std::shared_ptr<Request>> pending;
    std::atomic_size_t connectedClients{};
    std::atomic_uint64_t acceptedConnections{};
    std::atomic_uint64_t completedRequests{};
    std::atomic_uint64_t rejectedConnections{};
#if defined(__unix__) || defined(__APPLE__)
    int listenSocket{-1};
    std::jthread acceptThread;
    mutable std::mutex clientMutex;
    std::set<int> clientSockets;
    std::vector<std::jthread> clientThreads;
#endif
};

LiveEditorMcpHost::LiveEditorMcpHost(DveAiBridge& bridge, LiveEditorMcpHostOptions options)
    : impl_(std::make_unique<Impl>(bridge, std::move(options))) {}
LiveEditorMcpHost::~LiveEditorMcpHost() = default;
bool LiveEditorMcpHost::start(std::string* error) { return impl_->start(error); }
void LiveEditorMcpHost::stop() noexcept { impl_->stop(); }
std::size_t LiveEditorMcpHost::pump(std::size_t maximumRequests) { return impl_->pump(maximumRequests); }
LiveEditorMcpHostStatus LiveEditorMcpHost::status() const { return impl_->status(); }

int run_live_editor_mcp_proxy(const LiveEditorMcpProxyOptions& options,
                              std::istream& input,
                              std::ostream& output,
                              std::ostream& diagnostics) {
#if !defined(__unix__) && !defined(__APPLE__)
    (void)options; (void)input; (void)output;
    diagnostics << "dve_live_editor_mcp_proxy: private IPC is not implemented on this platform\n";
    return 2;
#else
    const auto projectRoot = options.projectRoot.empty() ? std::filesystem::current_path() : options.projectRoot;
    const auto descriptorPath = options.descriptorPath.empty()
        ? live_editor_mcp_descriptor_path(projectRoot, options.runtimeDirectory)
        : options.descriptorPath;
    std::string descriptorError;
    const auto descriptor = read_json_file(descriptorPath, &descriptorError);
    if (!descriptor) {
        diagnostics << "dve_live_editor_mcp_proxy: " << descriptorError << '\n';
        return 2;
    }
    const auto* schema = descriptor->find("schema");
    const auto* endpoint = descriptor->find("endpoint");
    const auto* token = descriptor->find("auth_token");
    const auto* describedRoot = descriptor->find("project_root");
    if (!schema || !schema->is_string() || schema->as_string() != kLiveEditorMcpDescriptorSchema ||
        !endpoint || !endpoint->is_string() || !token || !token->is_string() ||
        !describedRoot || !describedRoot->is_string()) {
        diagnostics << "dve_live_editor_mcp_proxy: descriptor is missing required fields\n";
        return 2;
    }
    if (canonical_or_absolute(describedRoot->as_string()) != canonical_or_absolute(projectRoot)) {
        diagnostics << "dve_live_editor_mcp_proxy: descriptor belongs to a different project root\n";
        return 2;
    }
    std::string connectError;
    const auto connected = connect_unix_socket(std::filesystem::path(endpoint->as_string()),
                                               options.connectTimeout, &connectError);
    if (!connected) {
        diagnostics << "dve_live_editor_mcp_proxy: " << connectError << '\n';
        return 2;
    }
    FileDescriptor socket(*connected);
    const JsonValue authentication = JsonValue::Object{
        {"schema", std::string(kAuthSchema)},
        {"token", std::string(token->as_string())},
        {"client", "dve-live-editor-mcp-proxy"},
        {"pid", static_cast<double>(::getpid())}
    };
    if (!write_socket_line(socket.value, stringify_json(authentication, false), options.connectTimeout, &connectError)) {
        diagnostics << "dve_live_editor_mcp_proxy: authentication write failed: " << connectError << '\n';
        return 2;
    }
    SocketLineReader reader(socket.value);
    const auto acknowledgement = reader.read({}, 8192, Clock::now() + options.connectTimeout);
    if (!acknowledgement.ok) {
        diagnostics << "dve_live_editor_mcp_proxy: editor did not acknowledge authentication\n";
        return 2;
    }
    const auto acknowledgementJson = parse_json(acknowledgement.line);
    const auto* ok = acknowledgementJson.value && acknowledgementJson.value->is_object()
        ? acknowledgementJson.value->find("ok") : nullptr;
    if (!ok || !ok->is_bool() || !ok->as_bool()) {
        diagnostics << "dve_live_editor_mcp_proxy: editor rejected authentication\n";
        return 2;
    }

    std::string message;
    while (std::getline(input, message)) {
        if (!message.empty() && message.back() == '\r') message.pop_back();
        if (message.empty()) continue;
        if (message.size() > options.maximumMessageBytes) {
            diagnostics << "dve_live_editor_mcp_proxy: MCP message exceeds the configured size limit\n";
            return 3;
        }
        const bool expectsResponse = request_has_id(message);
        if (!write_socket_line(socket.value, message, options.responseTimeout, &connectError)) {
            diagnostics << "dve_live_editor_mcp_proxy: request forwarding failed: " << connectError << '\n';
            return 3;
        }
        if (!expectsResponse) continue;
        const auto response = reader.read({}, options.maximumMessageBytes,
                                          Clock::now() + options.responseTimeout);
        if (!response.ok) {
            diagnostics << "dve_live_editor_mcp_proxy: response read failed"
                        << (response.error.empty() ? "" : ": " + response.error) << '\n';
            return 3;
        }
        output << response.line << '\n' << std::flush;
        if (!output) {
            diagnostics << "dve_live_editor_mcp_proxy: could not write MCP response\n";
            return 3;
        }
    }
    return 0;
#endif
}

} // namespace dve::ai
