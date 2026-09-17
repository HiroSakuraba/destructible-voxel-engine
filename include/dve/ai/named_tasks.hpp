#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "dve/ai/json.hpp"

namespace dve::ai {

struct AiNamedTaskDescriptor {
    std::string name;
    std::string title;
    std::string description;
    std::string executable;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory{"."};
    std::chrono::seconds timeout{300};
    std::size_t maximumOutputBytes{256U << 10U};
    bool writesArtifacts{true};
};

struct AiNamedTaskResult {
    bool launched{};
    bool succeeded{};
    bool timedOut{};
    int exitCode{-1};
    std::chrono::milliseconds duration{};
    std::string output;
    bool outputTruncated{};
    std::string error;
};

class AiNamedTaskRunner {
public:
    AiNamedTaskRunner(std::filesystem::path projectRoot, std::vector<AiNamedTaskDescriptor> tasks);
    [[nodiscard]] const std::vector<AiNamedTaskDescriptor>& tasks() const noexcept { return tasks_; }
    [[nodiscard]] const AiNamedTaskDescriptor* find(std::string_view name) const noexcept;
    [[nodiscard]] JsonValue list_json() const;
    [[nodiscard]] AiNamedTaskResult run(std::string_view name) const;
private:
    std::filesystem::path root_;
    std::vector<AiNamedTaskDescriptor> tasks_;
};

[[nodiscard]] std::vector<AiNamedTaskDescriptor> default_dve_validation_tasks();

} // namespace dve::ai
