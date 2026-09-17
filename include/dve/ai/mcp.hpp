#pragma once

#include <string>
#include <string_view>

#include "dve/ai/assistant.hpp"

namespace dve::ai {

class DveMcpProtocol {
public:
    explicit DveMcpProtocol(DveAiBridge& bridge, AiApprovalPolicy policy = AiApprovalPolicy::AskForChanges);
    [[nodiscard]] std::string handle(std::string_view jsonRpcMessage);
private:
    DveAiBridge& bridge_;
    AiApprovalPolicy policy_;
    std::string actorId_;
    bool initializeReceived_{};
    bool initialized_{};
    [[nodiscard]] JsonValue result_for(const JsonValue& request);
};

} // namespace dve::ai
