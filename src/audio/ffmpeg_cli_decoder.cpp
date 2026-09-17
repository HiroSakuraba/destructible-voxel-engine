#include "dve/audio/audio_asset.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef DVE_FFMPEG_EXECUTABLE_PATH
#define DVE_FFMPEG_EXECUTABLE_PATH "ffmpeg"
#endif

namespace dve::audio {
namespace {

std::string shell_quote(const std::string& value) {
#ifdef _WIN32
    std::string result{"\""};
    for (const char c : value) {
        if (c == '\"') result += "\\\"";
        else result += c;
    }
    result += '\"';
    return result;
#else
    std::string result{"'"};
    for (const char c : value) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    result += '\'';
    return result;
#endif
}

class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() { std::error_code error; std::filesystem::remove(path_, error); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

} // namespace

std::optional<DecodedAudioAsset> import_audio_with_ffmpeg_cli(const std::filesystem::path& path,
                                                               const AudioImportOptions& options,
                                                               std::string* error) {
    static std::atomic<std::uint64_t> serial{};
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temporary = std::filesystem::temp_directory_path() /
        ("dve_ffmpeg_decode_" + std::to_string(now) + "_" +
         std::to_string(serial.fetch_add(1U, std::memory_order_relaxed)) + ".wav");
    TemporaryFile output(temporary);

    const std::uint32_t rate = std::clamp(options.targetSampleRate, 8000U, 384000U);
    const std::string command = shell_quote(DVE_FFMPEG_EXECUTABLE_PATH) +
        " -nostdin -hide_banner -loglevel error -y -i " + shell_quote(path.string()) +
        " -map 0:a:0 -vn -ac 2 -ar " + std::to_string(rate) +
        " -c:a pcm_f32le " + shell_quote(output.path().string());
    const int result = std::system(command.c_str());
    if (result != 0) {
        if (error) *error = "FFmpeg could not decode the first audio stream";
        return std::nullopt;
    }
    auto decoded = import_wav_file(output.path(), options, error);
    if (!decoded) return std::nullopt;
    decoded->metadata.name = path.stem().string();
    decoded->metadata.contentHash = audio_content_hash(decoded->samples, decoded->metadata);
    return decoded;
}

} // namespace dve::audio
