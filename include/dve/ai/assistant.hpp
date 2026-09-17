#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/ai/json.hpp"
#include "dve/ai/named_tasks.hpp"

namespace dve { class GameWorld; }

namespace dve::ai {

enum class AiToolRisk : std::uint8_t { ReadOnly, Mutating, Destructive };
enum class AiApprovalPolicy : std::uint8_t { DenyAll, ReadOnlyOnly, AskForChanges, AllowChanges };

enum class AiCallStatus : std::uint8_t { Completed, ApprovalRequired, Denied, InvalidArguments, Failed };

struct AiToolDescriptor {
    std::string name;
    std::string title;
    std::string description;
    JsonValue inputSchema{JsonValue::Object{{"type", "object"}, {"additionalProperties", false}}};
    AiToolRisk risk{AiToolRisk::ReadOnly};
    bool idempotent{};
};

struct AiToolCallResult {
    AiCallStatus status{AiCallStatus::Failed};
    JsonValue content{JsonValue::Object{}};
    std::string message;
    std::string approvalId;
};

struct AiAuditEntry {
    std::uint64_t sequence{};
    std::string utcTimestamp;
    std::string actor;
    std::string tool;
    AiToolRisk risk{AiToolRisk::ReadOnly};
    AiCallStatus status{AiCallStatus::Failed};
    std::string summary;
};

class AiAuditLog {
public:
    explicit AiAuditLog(std::filesystem::path path = {});
    void append(AiAuditEntry entry);
    [[nodiscard]] const std::vector<AiAuditEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] JsonValue as_json() const;
private:
    std::filesystem::path path_;
    std::vector<AiAuditEntry> entries_;
    std::uint64_t nextSequence_{1};
};

struct AiPendingApproval {
    std::string id;
    std::string tool;
    std::string actor;
    AiToolRisk risk{AiToolRisk::Mutating};
    JsonValue arguments;
    std::string summary;
    bool approved{};
    bool resolved{};
};

class AiApprovalStore {
public:
    [[nodiscard]] std::string request(std::string tool, std::string actor, AiToolRisk risk, JsonValue arguments, std::string summary);
    bool approve(std::string_view id);
    bool deny(std::string_view id);
    [[nodiscard]] bool consume_approved(std::string_view id, std::string_view tool,
                                        const JsonValue& arguments, std::string_view actor);
    [[nodiscard]] bool consume_approved_for(std::string_view tool, const JsonValue& arguments,
                                            std::string_view actor);
    [[nodiscard]] const std::vector<AiPendingApproval>& pending() const noexcept { return requests_; }
private:
    std::vector<AiPendingApproval> requests_;
    std::uint64_t nextId_{1};
};

class AiToolRegistry {
public:
    using Handler = std::function<AiToolCallResult(const JsonValue&)>;
    bool add(AiToolDescriptor descriptor, Handler handler, std::string* error = nullptr);
    [[nodiscard]] const std::vector<AiToolDescriptor>& tools() const noexcept { return descriptors_; }
    [[nodiscard]] const AiToolDescriptor* find(std::string_view name) const noexcept;
    [[nodiscard]] AiToolCallResult call(std::string_view name, const JsonValue& arguments,
                                        AiApprovalPolicy policy, std::string_view approvalId,
                                        std::string_view actor = "assistant");
    [[nodiscard]] AiApprovalStore& approvals() noexcept { return approvals_; }
    [[nodiscard]] const AiApprovalStore& approvals() const noexcept { return approvals_; }
    [[nodiscard]] AiAuditLog& audit() noexcept { return audit_; }
    [[nodiscard]] const AiAuditLog& audit() const noexcept { return audit_; }
    void set_audit_path(std::filesystem::path path);
private:
    std::vector<AiToolDescriptor> descriptors_;
    std::map<std::string, Handler, std::less<>> handlers_;
    AiApprovalStore approvals_;
    AiAuditLog audit_;
};

struct DveAiBridgeOptions {
    std::filesystem::path projectRoot;
    std::size_t maximumReadBytes{1U << 20U};
    std::size_t maximumWriteBytes{1U << 20U};
    bool allowSourceWrites{true};
    bool enableDefaultValidationTasks{true};
    std::vector<AiNamedTaskDescriptor> validationTasks;
};

class DveAiBridge {
public:
    explicit DveAiBridge(DveAiBridgeOptions options, GameWorld* world = nullptr);
    [[nodiscard]] AiToolRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] const AiToolRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] const std::filesystem::path& project_root() const noexcept { return root_; }
    [[nodiscard]] JsonValue project_summary() const;
private:
    std::filesystem::path root_;
    DveAiBridgeOptions options_;
    GameWorld* world_{};
    AiToolRegistry registry_;
    std::unique_ptr<AiNamedTaskRunner> taskRunner_;
    void register_project_tools();
    void register_patch_tools();
    void register_task_tools();
    void register_scene_tools();
    void register_text3d_tools();
    void register_gabor_tools();
#if defined(DVE_ENABLE_FLUODDITY)
    void register_fluoddity_tools();
#endif
    [[nodiscard]] std::optional<std::filesystem::path> resolve_project_path(std::string_view relative,
                                                                           bool writing,
                                                                           std::string* error) const;
};

[[nodiscard]] std::uint64_t fnv1a64(std::string_view text) noexcept;
[[nodiscard]] std::string to_hex(std::uint64_t value);
[[nodiscard]] std::string_view to_string(AiToolRisk risk) noexcept;
[[nodiscard]] std::string_view to_string(AiCallStatus status) noexcept;

} // namespace dve::ai
