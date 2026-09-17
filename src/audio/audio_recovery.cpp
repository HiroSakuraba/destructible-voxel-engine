#include "dve/audio/audio_recovery.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <vector>

namespace dve::audio {
namespace {

constexpr std::array<char, 8> kJournalMagic{'D','V','E','A','J','R','N','1'};
constexpr std::uint64_t kMaximumSnapshotBytes = 1ULL << 34U;
constexpr std::uint32_t kMaximumLabelBytes = 1U << 20U;

std::uint64_t hash_bytes(const std::vector<char>& bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

template<class T>
bool write_value(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

template<class T>
bool read_value(std::istream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

std::filesystem::path temporary_sibling(const std::filesystem::path& path, std::string_view suffix) {
    return path.parent_path() / (path.filename().string() + std::string(suffix));
}

bool read_file(const std::filesystem::path& path, std::vector<char>& bytes, std::string* error) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { if (error) *error = "unable to open temporary audio session"; return false; }
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumSnapshotBytes) {
        if (error) *error = "audio recovery snapshot is too large";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!bytes.empty()) stream.read(bytes.data(), size);
    if (!stream) { if (error) *error = "unable to read temporary audio session"; return false; }
    return true;
}

} // namespace

bool atomic_write_audio_edit_session(const std::filesystem::path& path,
                                     const AudioEditSession& session,
                                     std::string* error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) { if (error) *error = filesystemError.message(); return false; }
    const auto temporary = temporary_sibling(path, ".tmp");
    if (!write_audio_edit_session(temporary, session, error)) return false;
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(path, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(temporary, path, filesystemError);
    }
    if (filesystemError) {
        std::filesystem::remove(temporary);
        if (error) *error = "unable to atomically replace audio session: " + filesystemError.message();
        return false;
    }
    return true;
}

AudioEditRecoveryJournal::AudioEditRecoveryJournal(std::filesystem::path path)
    : path_(std::move(path)) {
    AudioRecoveryInfo info;
    std::string ignored;
    if (recover_latest(&info, &ignored)) nextSequence_ = info.sequence + 1U;
}

bool AudioEditRecoveryJournal::append_snapshot(const AudioEditSession& session, std::string label,
                                                std::string* error) {
    if (label.size() > kMaximumLabelBytes) { if (error) *error = "audio recovery label is too large"; return false; }
    std::error_code filesystemError;
    std::filesystem::create_directories(path_.parent_path(), filesystemError);
    if (filesystemError) { if (error) *error = filesystemError.message(); return false; }
    const auto temporarySession = temporary_sibling(path_, ".snapshot.tmp");
    if (!write_audio_edit_session(temporarySession, session, error)) return false;
    std::vector<char> bytes;
    if (!read_file(temporarySession, bytes, error)) { std::filesystem::remove(temporarySession); return false; }
    std::filesystem::remove(temporarySession);

    const bool newFile = !std::filesystem::exists(path_);
    std::ofstream stream(path_, std::ios::binary | std::ios::app);
    if (!stream) { if (error) *error = "unable to open audio recovery journal"; return false; }
    if (newFile) stream.write(kJournalMagic.data(), static_cast<std::streamsize>(kJournalMagic.size()));
    const std::uint64_t sequence = nextSequence_++;
    const std::uint64_t size = bytes.size();
    const std::uint64_t hash = hash_bytes(bytes);
    const std::uint32_t labelSize = static_cast<std::uint32_t>(label.size());
    if (!write_value(stream, sequence) || !write_value(stream, size) || !write_value(stream, hash) ||
        !write_value(stream, labelSize) ||
        (labelSize != 0U && !static_cast<bool>(stream.write(label.data(), labelSize))) ||
        (size != 0U && !static_cast<bool>(stream.write(bytes.data(), static_cast<std::streamsize>(size))))) {
        if (error) *error = "unable to append audio recovery snapshot";
        return false;
    }
    stream.flush();
    return static_cast<bool>(stream);
}

std::optional<AudioEditSession> AudioEditRecoveryJournal::recover_latest(
    AudioRecoveryInfo* info, std::string* error) const {
    if (info != nullptr) *info = {};
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) return std::nullopt;
    std::array<char, 8> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != kJournalMagic) { if (error) *error = "unsupported audio recovery journal"; return std::nullopt; }
    std::vector<char> latest;
    std::uint64_t latestSequence{};
    std::string latestLabel;
    std::uint64_t validRecords{};
    bool truncated{};
    while (true) {
        std::uint64_t sequence{};
        std::uint64_t size{};
        std::uint64_t expectedHash{};
        std::uint32_t labelSize{};
        if (!read_value(stream, sequence)) {
            if (stream.eof()) break;
            truncated = true;
            break;
        }
        if (!read_value(stream, size) || !read_value(stream, expectedHash) || !read_value(stream, labelSize) ||
            size > kMaximumSnapshotBytes || labelSize > kMaximumLabelBytes) {
            truncated = true;
            break;
        }
        std::string label(labelSize, '\0');
        if (labelSize != 0U) stream.read(label.data(), labelSize);
        std::vector<char> bytes(static_cast<std::size_t>(size));
        if (size != 0U) stream.read(bytes.data(), static_cast<std::streamsize>(size));
        if (!stream || hash_bytes(bytes) != expectedHash) { truncated = true; break; }
        latest = std::move(bytes);
        latestSequence = sequence;
        latestLabel = std::move(label);
        ++validRecords;
    }
    if (latest.empty()) { if (error && truncated) *error = "audio recovery journal has no valid snapshot"; return std::nullopt; }
    const auto temporarySession = temporary_sibling(path_, ".recover.tmp");
    {
        std::ofstream output(temporarySession, std::ios::binary | std::ios::trunc);
        output.write(latest.data(), static_cast<std::streamsize>(latest.size()));
        if (!output) { if (error) *error = "unable to materialize recovered session"; return std::nullopt; }
    }
    auto recovered = read_audio_edit_session(temporarySession, error);
    std::filesystem::remove(temporarySession);
    if (recovered && info != nullptr) {
        info->sequence = latestSequence;
        info->label = std::move(latestLabel);
        info->validRecords = validRecords;
        info->ignoredTruncatedTail = truncated;
    }
    return recovered;
}

bool AudioEditRecoveryJournal::compact(const AudioEditSession& session, std::string label,
                                       std::string* error) {
    const auto oldPath = path_;
    const auto compacted = temporary_sibling(path_, ".compact.tmp");
    AudioEditRecoveryJournal replacement(compacted);
    if (!replacement.append_snapshot(session, std::move(label), error)) return false;
    std::error_code filesystemError;
    std::filesystem::rename(compacted, oldPath, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(oldPath, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(compacted, oldPath, filesystemError);
    }
    if (filesystemError) { if (error) *error = filesystemError.message(); return false; }
    nextSequence_ = 2U;
    return true;
}

bool AudioEditRecoveryJournal::clear(std::string* error) const {
    std::error_code filesystemError;
    const bool removed = std::filesystem::remove(path_, filesystemError);
    if (filesystemError) { if (error) *error = filesystemError.message(); return false; }
    return removed || !std::filesystem::exists(path_);
}

} // namespace dve::audio
