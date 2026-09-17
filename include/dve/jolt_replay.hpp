#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dve/physics_jolt_backend.hpp"

namespace dve {

struct JoltReplayFrame {
    std::uint64_t simulationFrame{};
    JoltWorldSnapshot snapshot{};
};

struct JoltReplayReadResult {
    std::vector<JoltReplayFrame> frames{};
    std::string error{};
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

class JoltReplayTrack {
public:
    bool capture(const JoltRigidBodyWorld& world, std::uint64_t simulationFrame);
    bool restore(JoltRigidBodyWorld& world, std::size_t frameIndex) const;
    void clear() noexcept { frames_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return frames_.size(); }
    [[nodiscard]] const std::vector<JoltReplayFrame>& frames() const noexcept { return frames_; }
    [[nodiscard]] bool write(const std::filesystem::path& path, std::string* error = nullptr) const;
    [[nodiscard]] static JoltReplayReadResult read(
        const std::filesystem::path& path,
        std::uint64_t maximumBytes = 512ULL * 1024ULL * 1024ULL);

private:
    std::vector<JoltReplayFrame> frames_{};
};

} // namespace dve
