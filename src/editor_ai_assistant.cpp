#include "dve/editor_ai_assistant.hpp"

#include <algorithm>
#include <stdexcept>

namespace dve::editor {

EditorAiAssistantPanel::EditorAiAssistantPanel(EditorAiAssistantPanel&& other) {
    std::lock_guard lock(other.completionMutex_);
    if (other.busy_)
        throw std::logic_error("cannot move an active AI assistant panel");
    client_ = std::move(other.client_);
    bridge_ = other.bridge_;
    tasks_ = other.tasks_;
    messages_ = std::move(other.messages_);
    pending_ = std::move(other.pending_);
    previousResponseId_ = std::move(other.previousResponseId_);
    lastError_ = std::move(other.lastError_);
    activeTask_ = other.activeTask_;
    completedResult_ = std::move(other.completedResult_);
    activeContinuation_ = other.activeContinuation_;
    lastUsage_ = other.lastUsage_;
    lastModel_ = std::move(other.lastModel_);
    other.bridge_ = nullptr;
    other.tasks_ = nullptr;
    other.activeTask_.reset();
    other.completedResult_.reset();
    other.activeContinuation_ = false;
}

EditorAiAssistantPanel& EditorAiAssistantPanel::operator=(EditorAiAssistantPanel&& other) {
    if (this == &other) return *this;
    std::scoped_lock lock(completionMutex_, other.completionMutex_);
    if (busy_ || other.busy_)
        throw std::logic_error("cannot move-assign an active AI assistant panel");
    client_ = std::move(other.client_);
    bridge_ = other.bridge_;
    tasks_ = other.tasks_;
    messages_ = std::move(other.messages_);
    pending_ = std::move(other.pending_);
    previousResponseId_ = std::move(other.previousResponseId_);
    lastError_ = std::move(other.lastError_);
    activeTask_ = other.activeTask_;
    completedResult_ = std::move(other.completedResult_);
    activeContinuation_ = other.activeContinuation_;
    lastUsage_ = other.lastUsage_;
    lastModel_ = std::move(other.lastModel_);
    other.bridge_ = nullptr;
    other.tasks_ = nullptr;
    other.activeTask_.reset();
    other.completedResult_.reset();
    other.activeContinuation_ = false;
    return *this;
}

void EditorAiAssistantPanel::attach(std::unique_ptr<ai::OpenAiResponsesClient> client,
                                    ai::DveAiBridge* bridge, EditorTaskManager* tasks) {
    if (busy_) throw std::logic_error("cannot reattach an active AI assistant panel");
    client_ = std::move(client);
    bridge_ = bridge;
    tasks_ = tasks;
    lastError_.clear();
    pending_.clear();
    lastUsage_ = {};
    lastModel_.clear();
    activeTask_.reset();
    busy_ = false;
    std::lock_guard lock(completionMutex_);
    completedResult_.reset();
}

void EditorAiAssistantPanel::accept_turn_result(const ai::OpenAiTurnResult& result) {
    if (!result.responseId.empty()) previousResponseId_ = result.responseId;
    lastUsage_ = result.usage;
    lastModel_ = result.model;
    if (!result.text.empty()) messages_.push_back({AiPanelRole::Assistant, result.text});
    pending_ = result.pendingApprovals;
    if (!pending_.empty()) {
        messages_.push_back({AiPanelRole::System, "Approval required for " + pending_.front().tool});
    }
}

bool EditorAiAssistantPanel::schedule_send(std::string text,
                                           std::optional<std::string> previousResponseId,
                                           bool continuation) {
    if (!tasks_ || !client_) return false;
    {
        std::lock_guard lock(completionMutex_);
        completedResult_.reset();
    }
    std::string error;
    auto task = tasks_->submit(continuation ? "Continue AI assistant request" : "AI assistant request",
        [this, text = std::move(text), previousResponseId = std::move(previousResponseId)](EditorTaskContext& context) {
            context.report(0.05F, "Preparing request");
            if (context.cancelled()) return;
            context.report(0.15F, "Waiting for assistant");
            const auto result = client_->send(
                text, previousResponseId ? std::optional<std::string_view>(*previousResponseId) : std::nullopt);
            if (context.cancelled()) return;
            {
                std::lock_guard lock(completionMutex_);
                completedResult_ = result;
            }
            context.report(0.95F, "Response received");
        }, &error);
    if (!task) {
        lastError_ = std::move(error);
        return false;
    }
    activeTask_ = *task;
    activeContinuation_ = continuation;
    busy_ = true;
    return true;
}

bool EditorAiAssistantPanel::send(std::string text) {
    if (!client_ || busy_) {
        lastError_ = client_ ? "assistant is busy" : "assistant is not configured";
        return false;
    }
    if (text.empty()) {
        lastError_ = "assistant prompt is empty";
        return false;
    }
    messages_.push_back({AiPanelRole::User, text});
    if (tasks_) {
        const bool scheduled = schedule_send(std::move(text),
            previousResponseId_.empty() ? std::nullopt : std::optional<std::string>(previousResponseId_), false);
        if (!scheduled) messages_.push_back({AiPanelRole::System, "Error: " + lastError_});
        return scheduled;
    }
    busy_ = true;
    const auto result = client_->send(
        text, previousResponseId_.empty() ? std::nullopt
                                         : std::optional<std::string_view>(previousResponseId_));
    busy_ = false;
    if (!result.ok) {
        lastError_ = result.error;
        messages_.push_back({AiPanelRole::System, "Error: " + result.error});
        return false;
    }
    lastError_.clear();
    accept_turn_result(result);
    return true;
}

bool EditorAiAssistantPanel::continue_after_decision(std::string instruction) {
    if (!client_ || previousResponseId_.empty() || busy_) return true;
    if (tasks_) {
        const bool scheduled=schedule_send(std::move(instruction),previousResponseId_,true);
        if(!scheduled) messages_.push_back({AiPanelRole::System,"Error continuing approved request: "+lastError_});
        return scheduled;
    }
    busy_ = true;
    const auto result = client_->send(instruction, previousResponseId_);
    busy_ = false;
    if (!result.ok) {
        lastError_ = result.error;
        messages_.push_back({AiPanelRole::System, "Error continuing approved request: " + result.error});
        return false;
    }
    lastError_.clear();
    accept_turn_result(result);
    return true;
}


void EditorAiAssistantPanel::refresh_pending_approvals() {
    if (!bridge_) {
        pending_.clear();
        return;
    }
    std::vector<ai::AiPendingApproval> unresolved;
    for (const auto& request : bridge_->registry().approvals().pending())
        if (!request.resolved) unresolved.push_back(request);
    pending_ = std::move(unresolved);
}

bool EditorAiAssistantPanel::approve(std::string_view id) {
    refresh_pending_approvals();
    const auto match = std::find_if(pending_.begin(), pending_.end(),
                                    [id](const ai::AiPendingApproval& item) { return item.id == id; });
    if (!bridge_ || match == pending_.end()) return false;
    const std::string tool = match->tool;
    const std::string actor = match->actor;
    if (!bridge_->registry().approvals().approve(id)) return false;
    refresh_pending_approvals();
    messages_.push_back({AiPanelRole::System, "Approved " + std::string(id) + " for " + actor});
    if (!actor.starts_with("openai-session-")) return true;
    return continue_after_decision(
        "The user approved the pending " + tool +
        " action. Repeat that tool call with exactly the same arguments, then continue the task.");
}

bool EditorAiAssistantPanel::deny(std::string_view id) {
    refresh_pending_approvals();
    const auto match = std::find_if(pending_.begin(), pending_.end(),
                                    [id](const ai::AiPendingApproval& item) { return item.id == id; });
    if (!bridge_ || match == pending_.end()) return false;
    const std::string tool = match->tool;
    const std::string actor = match->actor;
    if (!bridge_->registry().approvals().deny(id)) return false;
    refresh_pending_approvals();
    messages_.push_back({AiPanelRole::System, "Denied " + std::string(id) + " for " + actor});
    if (!actor.starts_with("openai-session-")) return true;
    return continue_after_decision(
        "The user denied the pending " + tool +
        " action. Do not repeat it. Explain the consequence and continue with safe read-only work if useful.");
}

bool EditorAiAssistantPanel::cancel() {
    if (!tasks_ || !activeTask_) return false;
    if (!tasks_->cancel(*activeTask_)) return false;
    messages_.push_back({AiPanelRole::System, "Cancellation requested. The active network operation may finish before the worker exits."});
    return true;
}

bool EditorAiAssistantPanel::poll() {
    if (!tasks_ || !activeTask_) return false;
    const auto snapshot=tasks_->snapshot(*activeTask_);
    if (!snapshot || snapshot->state==EditorTaskState::Queued || snapshot->state==EditorTaskState::Running) return false;
    std::optional<ai::OpenAiTurnResult> result;
    {
        std::lock_guard lock(completionMutex_);
        result=std::move(completedResult_);
        completedResult_.reset();
    }
    const bool continuation=activeContinuation_;
    activeTask_.reset();busy_=false;activeContinuation_=false;
    if(snapshot->state==EditorTaskState::Cancelled){lastError_="assistant request cancelled";messages_.push_back({AiPanelRole::System,"Assistant request cancelled."});return true;}
    if(snapshot->state==EditorTaskState::Failed){lastError_=snapshot->error.empty()?"assistant worker failed":snapshot->error;messages_.push_back({AiPanelRole::System,"Error: "+lastError_});return true;}
    if(!result){lastError_="assistant worker completed without a result";messages_.push_back({AiPanelRole::System,"Error: "+lastError_});return true;}
    if(!result->ok){lastError_=result->error;messages_.push_back({AiPanelRole::System,(continuation?"Error continuing approved request: ":"Error: ")+result->error});return true;}
    lastError_.clear();accept_turn_result(*result);return true;
}

void EditorAiAssistantPanel::clear() {
    if (busy_) (void)cancel();
    messages_.clear();
    pending_.clear();
    previousResponseId_.clear();
    lastError_.clear();
    if(!busy_) activeTask_.reset();
}

} // namespace dve::editor
