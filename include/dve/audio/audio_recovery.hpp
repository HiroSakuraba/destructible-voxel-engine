#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "dve/audio/audio_edit.hpp"

namespace dve::audio {

[[nodiscard]] bool atomic_write_audio_edit_session(const std::filesystem::path& path,
                                                    const AudioEditSession& session,
                                                    std::string* error = nullptr);

struct AudioRecoveryInfo {
    std::uint64_t sequence{};
    std::string label;
    std::uint64_t validRecords{};
    bool ignoredTruncatedTail{};
};

class AudioEditRecoveryJournal {
public:
    explicit AudioEditRecoveryJournal(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    bool append_snapshot(const AudioEditSession& session, std::string label = {},
                         std::string* error = nullptr);
    [[nodiscard]] std::optional<AudioEditSession> recover_latest(
        AudioRecoveryInfo* info = nullptr, std::string* error = nullptr) const;
    bool compact(const AudioEditSession& session, std::string label = {},
                 std::string* error = nullptr);
    bool clear(std::string* error = nullptr) const;
private:
    std::filesystem::path path_;
    std::uint64_t nextSequence_{1};
};

} // namespace dve::audio
