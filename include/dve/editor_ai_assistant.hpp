#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dve/ai/openai_responses.hpp"
#include "dve/editor_tasks.hpp"

namespace dve::editor {

class EditorWorkspace;

enum class AiPanelRole : std::uint8_t { User, Assistant, Tool, System };
struct AiPanelMessage { AiPanelRole role{AiPanelRole::System}; std::string text; };

class EditorAiAssistantPanel {
public:
    EditorAiAssistantPanel() = default;
    EditorAiAssistantPanel(const EditorAiAssistantPanel&) = delete;
    EditorAiAssistantPanel& operator=(const EditorAiAssistantPanel&) = delete;
    EditorAiAssistantPanel(EditorAiAssistantPanel&& other);
    EditorAiAssistantPanel& operator=(EditorAiAssistantPanel&& other);
    void attach(std::unique_ptr<ai::OpenAiResponsesClient> client, ai::DveAiBridge* bridge,
                EditorTaskManager* tasks = nullptr);
    [[nodiscard]] bool attached() const noexcept { return client_ != nullptr; }
    [[nodiscard]] bool send(std::string text);
    bool approve(std::string_view approvalId);
    bool deny(std::string_view approvalId);
    [[nodiscard]] bool cancel();
    [[nodiscard]] bool poll();
    void refresh_pending_approvals();
    void clear();
    [[nodiscard]] const std::vector<AiPanelMessage>& messages() const noexcept { return messages_; }
    [[nodiscard]] const std::vector<ai::AiPendingApproval>& pending_approvals() const noexcept { return pending_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return lastError_; }
    [[nodiscard]] std::optional<EditorTaskId> active_task() const noexcept { return activeTask_; }
    [[nodiscard]] const ai::OpenAiUsage& last_usage() const noexcept { return lastUsage_; }
    [[nodiscard]] const std::string& last_model() const noexcept { return lastModel_; }
private:
    bool continue_after_decision(std::string instruction);
    bool schedule_send(std::string text, std::optional<std::string> previousResponseId,
                       bool continuation);
    void accept_turn_result(const ai::OpenAiTurnResult& result);
    std::unique_ptr<ai::OpenAiResponsesClient> client_;
    ai::DveAiBridge* bridge_{};
    EditorTaskManager* tasks_{};
    std::vector<AiPanelMessage> messages_;
    std::vector<ai::AiPendingApproval> pending_;
    std::string previousResponseId_;
    std::string lastError_;
    bool busy_{};
    std::optional<EditorTaskId> activeTask_;
    std::mutex completionMutex_;
    std::optional<ai::OpenAiTurnResult> completedResult_;
    bool activeContinuation_{};
    ai::OpenAiUsage lastUsage_{};
    std::string lastModel_;
};

void register_editor_ai_tools(ai::DveAiBridge& bridge, EditorWorkspace& workspace);

} // namespace dve::editor
