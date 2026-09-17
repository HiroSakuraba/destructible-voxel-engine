#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/ai/assistant.hpp"

namespace dve::ai {

struct AiHttpResponse { int statusCode{}; std::string body; std::string error; };
class IAiHttpTransport {
public:
    virtual ~IAiHttpTransport() = default;
    virtual AiHttpResponse post_json(std::string_view url, std::string_view bearerToken,
                                     std::string_view body) = 0;
};

class CurlAiHttpTransport final : public IAiHttpTransport {
public:
    explicit CurlAiHttpTransport(std::filesystem::path temporaryDirectory = {},
                                 std::chrono::seconds connectTimeout = std::chrono::seconds(10),
                                 std::chrono::seconds requestTimeout = std::chrono::seconds(120),
                                 std::size_t retryCount = 2);
    AiHttpResponse post_json(std::string_view url, std::string_view bearerToken,
                             std::string_view body) override;
private:
    std::filesystem::path temporaryDirectory_;
    std::chrono::seconds connectTimeout_;
    std::chrono::seconds requestTimeout_;
    std::size_t retryCount_{};
};

struct OpenAiResponsesConfig {
    std::string endpoint{"https://api.openai.com/v1/responses"};
    std::string model{"gpt-5.6"};
    std::string instructions{
        "You are working inside the Destructible Voxel Engine editor. Inspect before changing. "
        "Use the smallest safe tool call. Never claim an action succeeded unless its tool result says so."};
    std::size_t maximumToolTurns{8};
    AiApprovalPolicy approvalPolicy{AiApprovalPolicy::AskForChanges};
};

struct OpenAiUsage {
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    std::uint64_t totalTokens{};
};

struct OpenAiTurnResult {
    bool ok{};
    std::string responseId;
    std::string text;
    std::vector<AiPendingApproval> pendingApprovals;
    std::string error;
    OpenAiUsage usage;
    std::string model;
};

class OpenAiResponsesClient {
public:
    using ApiKeyProvider = std::function<std::string()>;
    OpenAiResponsesClient(DveAiBridge& bridge, std::unique_ptr<IAiHttpTransport> transport,
                          OpenAiResponsesConfig config = {}, ApiKeyProvider keyProvider = {});
    [[nodiscard]] OpenAiTurnResult send(std::string_view userText,
                                        std::optional<std::string_view> previousResponseId = std::nullopt);
    [[nodiscard]] JsonValue build_request(std::string_view userText,
                                          std::optional<std::string_view> previousResponseId) const;
private:
    DveAiBridge& bridge_;
    std::unique_ptr<IAiHttpTransport> transport_;
    OpenAiResponsesConfig config_;
    ApiKeyProvider keyProvider_;
    std::string actorId_;
};

} // namespace dve::ai
