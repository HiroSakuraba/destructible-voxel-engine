#include "dve/audio/audio_asset.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>

namespace dve::audio {
#ifdef DVE_HAVE_SNDFILE
std::optional<DecodedAudioAsset> import_audio_with_sndfile(const std::filesystem::path& path,
                                                            const AudioImportOptions& options,
                                                            std::string* error);
#endif
#ifdef DVE_HAVE_FFMPEG_CLI
std::optional<DecodedAudioAsset> import_audio_with_ffmpeg_cli(const std::filesystem::path& path,
                                                               const AudioImportOptions& options,
                                                               std::string* error);
#endif
namespace {

constexpr std::array<char, 8> kCookedMagic{'D','V','E','S','A','M','P','1'};
constexpr std::uint32_t kCookedVersion = 1;
constexpr std::size_t kMaximumNameBytes = 4096;
constexpr std::size_t kMaximumCueCount = 4096;
constexpr std::uint64_t kMaximumFrames = 0x7fffffffULL;

template<class T>
bool read_value(std::istream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

template<class T>
bool write_value(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(stream);
}

std::uint16_t u16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(p[0])) |
           static_cast<std::uint16_t>(std::to_integer<unsigned>(p[1]) << 8U);
}
std::uint32_t u32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[3])) << 24U);
}

float sanitize(float value) noexcept {
    if (!std::isfinite(value)) return 0.0F;
    return std::clamp(value, -8.0F, 8.0F);
}

void update_metadata(DecodedAudioAsset& asset) {
    auto& metadata = asset.metadata;
    metadata.frameCount = metadata.channels == 0U ? 0U : asset.samples.size() / metadata.channels;
    metadata.durationSeconds = metadata.sampleRate == 0U ? 0.0 :
        static_cast<double>(metadata.frameCount) / static_cast<double>(metadata.sampleRate);
    double sumSquares{};
    float peak{};
    for (float& sample : asset.samples) {
        sample = sanitize(sample);
        peak = std::max(peak, std::abs(sample));
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    metadata.peakLinear = peak;
    metadata.rmsLinear = asset.samples.empty() ? 0.0F :
        static_cast<float>(std::sqrt(sumSquares / static_cast<double>(asset.samples.size())));
    metadata.approximateLoudnessDbfs = metadata.rmsLinear > 1.0e-9F
        ? 20.0F * std::log10(metadata.rmsLinear) : -120.0F;
    if (metadata.loop.enabled && (metadata.loop.beginFrame >= metadata.loop.endFrame ||
                                  metadata.loop.endFrame > metadata.frameCount)) {
        metadata.loop = {};
    }
    metadata.contentHash = audio_content_hash(asset.samples, metadata);
}

std::vector<float> linear_resample(std::span<const float> input, std::uint8_t channels,
                                   std::uint32_t sourceRate, std::uint32_t targetRate) {
    if (sourceRate == targetRate || input.empty()) return {input.begin(), input.end()};
    const std::size_t sourceFrames = input.size() / channels;
    const std::size_t targetFrames = static_cast<std::size_t>(std::llround(
        static_cast<long double>(sourceFrames) * targetRate / sourceRate));
    std::vector<float> output(targetFrames * channels);
    const double scale = static_cast<double>(sourceRate) / static_cast<double>(targetRate);
    for (std::size_t frame = 0; frame < targetFrames; ++frame) {
        const double position = std::min(static_cast<double>(sourceFrames - 1U), static_cast<double>(frame) * scale);
        const std::size_t i0 = static_cast<std::size_t>(position);
        const std::size_t i1 = std::min(i0 + 1U, sourceFrames - 1U);
        const float fraction = static_cast<float>(position - static_cast<double>(i0));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const float a = input[i0 * channels + channel];
            const float b = input[i1 * channels + channel];
            output[frame * channels + channel] = a + (b - a) * fraction;
        }
    }
    return output;
}

struct CookedHeader {
    std::array<char, 8> magic{};
    std::uint32_t version{};
    std::uint32_t headerBytes{};
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint64_t frameCount{};
    std::uint32_t storagePolicy{};
    std::uint32_t nameBytes{};
    std::uint32_t cueCount{};
    std::uint32_t flags{};
    std::uint64_t loopBegin{};
    std::uint64_t loopEnd{};
    float peak{};
    float rms{};
    float loudness{};
    std::uint32_t reserved{};
    std::uint64_t contentHash{};
    std::uint64_t sampleDataOffset{};
};
static_assert(std::is_trivially_copyable_v<CookedHeader>);

bool read_header(std::istream& stream, CookedHeader& header, AudioAssetMetadata& metadata,
                 std::uint64_t& sampleOffset, std::string* error) {
    if (!read_value(stream, header) || header.magic != kCookedMagic || header.version != kCookedVersion) {
        if (error) *error = "not a supported DVE cooked audio asset";
        return false;
    }
    if (header.channels < 1U || header.channels > 2U || header.sampleRate < 8000U ||
        header.sampleRate > 384000U || header.frameCount > kMaximumFrames ||
        header.nameBytes > kMaximumNameBytes || header.cueCount > kMaximumCueCount) {
        if (error) *error = "cooked audio header contains invalid dimensions";
        return false;
    }
    metadata = {};
    metadata.version = header.version;
    metadata.sampleRate = header.sampleRate;
    metadata.channels = static_cast<std::uint8_t>(header.channels);
    metadata.frameCount = header.frameCount;
    metadata.durationSeconds = static_cast<double>(header.frameCount) / header.sampleRate;
    metadata.storagePolicy = header.storagePolicy == 1U ? AudioStoragePolicy::Streamed : AudioStoragePolicy::Resident;
    metadata.loop = {header.loopBegin, header.loopEnd, (header.flags & 1U) != 0U};
    metadata.peakLinear = header.peak;
    metadata.rmsLinear = header.rms;
    metadata.approximateLoudnessDbfs = header.loudness;
    metadata.contentHash = header.contentHash;
    metadata.name.resize(header.nameBytes);
    if (header.nameBytes != 0U) stream.read(metadata.name.data(), static_cast<std::streamsize>(header.nameBytes));
    if (!stream) { if (error) *error = "truncated cooked audio name"; return false; }
    metadata.cues.reserve(header.cueCount);
    for (std::uint32_t i = 0; i < header.cueCount; ++i) {
        AudioCueMarker cue;
        std::uint32_t bytes{};
        if (!read_value(stream, cue.frame) || !read_value(stream, bytes) || bytes > kMaximumNameBytes) {
            if (error) *error = "invalid cooked audio cue";
            return false;
        }
        cue.name.resize(bytes);
        if (bytes != 0U) stream.read(cue.name.data(), static_cast<std::streamsize>(bytes));
        if (!stream || cue.frame > metadata.frameCount) {
            if (error) *error = "truncated or out-of-range cooked audio cue";
            return false;
        }
        metadata.cues.push_back(std::move(cue));
    }
    sampleOffset = header.sampleDataOffset;
    stream.seekg(static_cast<std::streamoff>(sampleOffset), std::ios::beg);
    if (!stream) { if (error) *error = "invalid cooked audio sample offset"; return false; }
    return true;
}

} // namespace

std::uint64_t audio_content_hash(std::span<const float> samples,
                                 const AudioAssetMetadata& metadata) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    auto add = [&](const void* data, std::size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) { hash ^= p[i]; hash *= 1099511628211ULL; }
    };
    add(&metadata.sampleRate, sizeof(metadata.sampleRate));
    add(&metadata.channels, sizeof(metadata.channels));
    add(&metadata.frameCount, sizeof(metadata.frameCount));
    add(metadata.name.data(), metadata.name.size());
    add(samples.data(), samples.size_bytes());
    return hash;
}

AudioImportCapabilities audio_import_capabilities() noexcept {
    AudioImportCapabilities result;
    result.nativeWav = true;
#ifdef DVE_HAVE_SNDFILE
    result.sndfile = true;
#endif
#ifdef DVE_HAVE_FFMPEG_CLI
    result.ffmpeg = true;
#endif
    return result;
}

std::optional<DecodedAudioAsset> import_audio_file(const std::filesystem::path& path,
                                                    const AudioImportOptions& options,
                                                    std::string* error) {
    if (error) error->clear();
    const std::string extension = path.extension().string();
    if (extension == ".wav" || extension == ".WAV" || extension == ".wave" || extension == ".WAVE") {
        auto native = import_wav_file(path, options, error);
        if (native) return native;
    }
    std::string sndfileError;
#ifdef DVE_HAVE_SNDFILE
    if (auto decoded = import_audio_with_sndfile(path, options, &sndfileError)) return decoded;
#endif
    std::string ffmpegError;
#ifdef DVE_HAVE_FFMPEG_CLI
    if (auto decoded = import_audio_with_ffmpeg_cli(path, options, &ffmpegError)) return decoded;
#endif
    if (error) {
        *error = "no audio decoder accepted the source";
        if (!sndfileError.empty()) *error += "; " + sndfileError;
        if (!ffmpegError.empty()) *error += "; " + ffmpegError;
#ifndef DVE_HAVE_SNDFILE
        *error += "; libsndfile support is not built";
#endif
#ifndef DVE_HAVE_FFMPEG_CLI
        *error += "; FFmpeg authoring support is not built";
#endif
    }
    return std::nullopt;
}

std::optional<DecodedAudioAsset> import_wav_file(const std::filesystem::path& path,
                                                  const AudioImportOptions& options,
                                                  std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { if (error) *error = "unable to open WAV file"; return std::nullopt; }
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    if (length < 44 || length > static_cast<std::streamoff>(2ULL * 1024ULL * 1024ULL * 1024ULL)) {
        if (error) *error = "WAV file has invalid size";
        return std::nullopt;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!stream || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        if (error) *error = "file is not RIFF/WAVE";
        return std::nullopt;
    }

    std::uint16_t format{};
    std::uint16_t channels{};
    std::uint32_t sampleRate{};
    std::uint16_t blockAlign{};
    std::uint16_t bits{};
    const std::byte* sampleData{};
    std::size_t sampleBytes{};
    AudioLoopRegion loop{};
    std::vector<AudioCueMarker> cues;

    std::size_t offset = 12U;
    while (offset + 8U <= bytes.size()) {
        const char* id = reinterpret_cast<const char*>(bytes.data() + offset);
        const std::uint32_t size = u32(bytes.data() + offset + 4U);
        const std::size_t dataOffset = offset + 8U;
        if (dataOffset + size > bytes.size()) { if (error) *error = "truncated WAV chunk"; return std::nullopt; }
        const std::byte* chunk = bytes.data() + dataOffset;
        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16U) {
            format = u16(chunk);
            channels = u16(chunk + 2U);
            sampleRate = u32(chunk + 4U);
            blockAlign = u16(chunk + 12U);
            bits = u16(chunk + 14U);
            if (format == 0xfffeU && size >= 40U) format = u16(chunk + 24U); // extensible sub-format tag
        } else if (std::memcmp(id, "data", 4) == 0) {
            sampleData = chunk;
            sampleBytes = size;
        } else if (std::memcmp(id, "cue ", 4) == 0 && size >= 4U) {
            const std::uint32_t count = std::min<std::uint32_t>(u32(chunk), 4096U);
            for (std::uint32_t i = 0; i < count && 4U + (i + 1U) * 24U <= size; ++i) {
                const std::byte* point = chunk + 4U + i * 24U;
                cues.push_back({u32(point + 20U), "cue_" + std::to_string(i)});
            }
        } else if (std::memcmp(id, "smpl", 4) == 0 && size >= 60U && u32(chunk + 28U) > 0U) {
            loop.beginFrame = u32(chunk + 44U);
            loop.endFrame = static_cast<std::uint64_t>(u32(chunk + 48U)) + 1U;
            loop.enabled = loop.endFrame > loop.beginFrame;
        }
        offset = dataOffset + size + (size & 1U);
    }

    if ((format != 1U && format != 3U) || channels == 0U || channels > 2U || sampleRate < 8000U ||
        sampleRate > 384000U || blockAlign == 0U || sampleData == nullptr || sampleBytes < blockAlign) {
        if (error) *error = "WAV format is unsupported or incomplete";
        return std::nullopt;
    }
    const std::size_t frames = sampleBytes / blockAlign;
    std::vector<float> samples(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const std::byte* p = sampleData + frame * blockAlign + channel * (bits / 8U);
            float value{};
            if (format == 3U && bits == 32U) {
                std::uint32_t raw = u32(p);
                value = std::bit_cast<float>(raw);
            } else if (format == 1U && bits == 8U) {
                value = (static_cast<float>(std::to_integer<unsigned>(*p)) - 128.0F) / 128.0F;
            } else if (format == 1U && bits == 16U) {
                value = static_cast<float>(static_cast<std::int16_t>(u16(p))) / 32768.0F;
            } else if (format == 1U && bits == 24U) {
                std::int32_t raw = static_cast<std::int32_t>(std::to_integer<unsigned>(p[0])) |
                    (static_cast<std::int32_t>(std::to_integer<unsigned>(p[1])) << 8) |
                    (static_cast<std::int32_t>(std::to_integer<unsigned>(p[2])) << 16);
                if ((raw & 0x00800000) != 0) raw |= static_cast<std::int32_t>(0xff000000U);
                value = static_cast<float>(raw) / 8388608.0F;
            } else if (format == 1U && bits == 32U) {
                value = static_cast<float>(static_cast<std::int32_t>(u32(p))) / 2147483648.0F;
            } else {
                if (error) *error = "WAV sample encoding is unsupported";
                return std::nullopt;
            }
            samples[frame * channels + channel] = sanitize(value);
        }
    }

    const std::uint32_t targetRate = std::clamp(options.targetSampleRate, 8000U, 384000U);
    if (targetRate != sampleRate) {
        const double ratio = static_cast<double>(targetRate) / sampleRate;
        samples = linear_resample(samples, static_cast<std::uint8_t>(channels), sampleRate, targetRate);
        if (loop.enabled) {
            loop.beginFrame = static_cast<std::uint64_t>(std::llround(static_cast<double>(loop.beginFrame) * ratio));
            loop.endFrame = static_cast<std::uint64_t>(std::llround(static_cast<double>(loop.endFrame) * ratio));
        }
        for (auto& cue : cues) cue.frame = static_cast<std::uint64_t>(std::llround(static_cast<double>(cue.frame) * ratio));
        sampleRate = targetRate;
    }

    if (options.normalize && !samples.empty()) {
        float peak{};
        for (const float value : samples) peak = std::max(peak, std::abs(value));
        if (peak > 1.0e-8F) {
            const float scale = std::clamp(options.normalizePeak, 0.01F, 1.0F) / peak;
            for (float& value : samples) value *= scale;
        }
    }

    DecodedAudioAsset result;
    result.metadata.name = path.stem().string();
    result.metadata.sampleRate = sampleRate;
    result.metadata.channels = static_cast<std::uint8_t>(channels);
    result.metadata.storagePolicy = options.storagePolicy;
    result.metadata.loop = loop;
    result.metadata.cues = std::move(cues);
    result.samples = std::move(samples);
    update_metadata(result);
    return result;
}

bool write_wav_file(const std::filesystem::path& path, const DecodedAudioAsset& asset,
                    WavSampleEncoding encoding, std::string* error) {
    const auto channels = asset.metadata.channels;
    const auto sampleRate = asset.metadata.sampleRate;
    if ((channels != 1U && channels != 2U) || sampleRate < 8000U || sampleRate > 384000U ||
        asset.samples.empty() || asset.samples.size() % channels != 0U) {
        if (error) *error = "invalid audio asset for WAV export";
        return false;
    }
    const std::uint64_t frames = asset.samples.size() / channels;
    const std::uint16_t bits = encoding == WavSampleEncoding::Float32 ? 32U : 16U;
    const std::uint16_t format = encoding == WavSampleEncoding::Float32 ? 3U : 1U;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bits / 8U));
    const std::uint64_t dataBytes64 = frames * blockAlign;
    if (dataBytes64 > std::numeric_limits<std::uint32_t>::max() - 36U) {
        if (error) *error = "WAV export exceeds RIFF size limits";
        return false;
    }
    const auto dataBytes = static_cast<std::uint32_t>(dataBytes64);
    const std::uint32_t riffBytes = 36U + dataBytes;
    const std::uint32_t byteRate = sampleRate * blockAlign;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { if (error) *error = "unable to create WAV file"; return false; }
    stream.write("RIFF", 4);
    write_value(stream, riffBytes);
    stream.write("WAVEfmt ", 8);
    const std::uint32_t fmtSize = 16U;
    write_value(stream, fmtSize);
    write_value(stream, format);
    const std::uint16_t wavChannels = channels;
    write_value(stream, wavChannels);
    write_value(stream, sampleRate);
    write_value(stream, byteRate);
    write_value(stream, blockAlign);
    write_value(stream, bits);
    stream.write("data", 4);
    write_value(stream, dataBytes);
    if (encoding == WavSampleEncoding::Float32) {
        for (const float sample : asset.samples) {
            const float value = sanitize(sample);
            write_value(stream, value);
        }
    } else {
        for (const float sample : asset.samples) {
            const float clamped = std::clamp(sanitize(sample), -1.0F, 1.0F);
            const auto value = static_cast<std::int16_t>(std::lrint(clamped * 32767.0F));
            write_value(stream, value);
        }
    }
    if (!stream) { if (error) *error = "failed while writing WAV file"; return false; }
    return true;
}

bool write_cooked_audio_asset(const std::filesystem::path& path, const DecodedAudioAsset& source,
                              std::string* error) {
    DecodedAudioAsset asset = source;
    if (asset.metadata.channels < 1U || asset.metadata.channels > 2U ||
        asset.metadata.sampleRate < 8000U || asset.metadata.sampleRate > 384000U ||
        asset.samples.empty() || asset.samples.size() % asset.metadata.channels != 0U ||
        asset.metadata.name.size() > kMaximumNameBytes || asset.metadata.cues.size() > kMaximumCueCount ||
        std::any_of(asset.metadata.cues.begin(), asset.metadata.cues.end(), [](const AudioCueMarker& cue) {
            return cue.name.size() > kMaximumNameBytes;
        })) {
        if (error) *error = "audio asset is invalid";
        return false;
    }
    update_metadata(asset);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { if (error) *error = "unable to create cooked audio asset"; return false; }

    CookedHeader header;
    header.magic = kCookedMagic;
    header.version = kCookedVersion;
    header.headerBytes = sizeof(CookedHeader);
    header.sampleRate = asset.metadata.sampleRate;
    header.channels = asset.metadata.channels;
    header.frameCount = asset.metadata.frameCount;
    header.storagePolicy = asset.metadata.storagePolicy == AudioStoragePolicy::Streamed ? 1U : 0U;
    header.nameBytes = static_cast<std::uint32_t>(asset.metadata.name.size());
    header.cueCount = static_cast<std::uint32_t>(asset.metadata.cues.size());
    header.flags = asset.metadata.loop.enabled ? 1U : 0U;
    header.loopBegin = asset.metadata.loop.beginFrame;
    header.loopEnd = asset.metadata.loop.endFrame;
    header.peak = asset.metadata.peakLinear;
    header.rms = asset.metadata.rmsLinear;
    header.loudness = asset.metadata.approximateLoudnessDbfs;
    header.contentHash = asset.metadata.contentHash;
    std::uint64_t variableBytes = asset.metadata.name.size();
    for (const auto& cue : asset.metadata.cues) variableBytes += sizeof(cue.frame) + sizeof(std::uint32_t) + cue.name.size();
    header.sampleDataOffset = sizeof(CookedHeader) + variableBytes;

    if (!write_value(stream, header)) { if (error) *error = "unable to write cooked audio header"; return false; }
    stream.write(asset.metadata.name.data(), static_cast<std::streamsize>(asset.metadata.name.size()));
    for (const auto& cue : asset.metadata.cues) {
        const std::uint32_t bytes = static_cast<std::uint32_t>(cue.name.size());
        if (!write_value(stream, cue.frame) || !write_value(stream, bytes)) return false;
        stream.write(cue.name.data(), static_cast<std::streamsize>(cue.name.size()));
    }
    stream.write(reinterpret_cast<const char*>(asset.samples.data()),
                 static_cast<std::streamsize>(asset.samples.size() * sizeof(float)));
    if (!stream) { if (error) *error = "unable to write cooked audio samples"; return false; }
    return true;
}

std::optional<DecodedAudioAsset> read_cooked_audio_asset(const std::filesystem::path& path,
                                                          std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { if (error) *error = "unable to open cooked audio asset"; return std::nullopt; }
    CookedHeader header;
    DecodedAudioAsset result;
    std::uint64_t offset{};
    if (!read_header(stream, header, result.metadata, offset, error)) return std::nullopt;
    const std::uint64_t sampleCount = header.frameCount * header.channels;
    if (sampleCount > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        if (error) *error = "cooked audio sample count is too large";
        return std::nullopt;
    }
    result.samples.resize(static_cast<std::size_t>(sampleCount));
    stream.read(reinterpret_cast<char*>(result.samples.data()),
                static_cast<std::streamsize>(result.samples.size() * sizeof(float)));
    if (!stream) { if (error) *error = "truncated cooked audio sample data"; return std::nullopt; }
    if (audio_content_hash(result.samples, result.metadata) != result.metadata.contentHash) {
        if (error) *error = "cooked audio content hash mismatch";
        return std::nullopt;
    }
    return result;
}

std::optional<AudioAssetMetadata> read_cooked_audio_metadata(const std::filesystem::path& path,
                                                              std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { if (error) *error = "unable to open cooked audio asset"; return std::nullopt; }
    CookedHeader header;
    AudioAssetMetadata metadata;
    std::uint64_t offset{};
    if (!read_header(stream, header, metadata, offset, error)) return std::nullopt;
    return metadata;
}

AudioStreamRing::AudioStreamRing(std::uint8_t channels, std::size_t capacityFrames)
    : channels_(std::clamp<std::uint8_t>(channels, 1U, 2U)),
      capacityFrames_(std::max<std::size_t>(2U, capacityFrames)),
      data_(capacityFrames_ * channels_) {}

std::size_t AudioStreamRing::available_read_frames() const noexcept {
    const auto write = writeFrame_.load(std::memory_order_acquire);
    const auto read = readFrame_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}
std::size_t AudioStreamRing::available_write_frames() const noexcept {
    return capacityFrames_ - std::min(capacityFrames_, available_read_frames());
}
std::size_t AudioStreamRing::write(std::span<const float> interleaved) noexcept {
    const std::size_t requested = interleaved.size() / channels_;
    const std::uint64_t read = readFrame_.load(std::memory_order_acquire);
    const std::uint64_t write = writeFrame_.load(std::memory_order_relaxed);
    const std::size_t free = capacityFrames_ - std::min<std::size_t>(capacityFrames_, static_cast<std::size_t>(write - read));
    const std::size_t frames = std::min(requested, free);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t destination = static_cast<std::size_t>((write + frame) % capacityFrames_) * channels_;
        std::copy_n(interleaved.data() + frame * channels_, channels_, data_.data() + destination);
    }
    writeFrame_.store(write + frames, std::memory_order_release);
    return frames;
}
std::size_t AudioStreamRing::read(std::span<float> interleaved) noexcept {
    const std::size_t requested = interleaved.size() / channels_;
    const std::uint64_t read = readFrame_.load(std::memory_order_relaxed);
    const std::uint64_t write = writeFrame_.load(std::memory_order_acquire);
    const std::size_t frames = std::min<std::size_t>(requested, static_cast<std::size_t>(write - read));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t source = static_cast<std::size_t>((read + frame) % capacityFrames_) * channels_;
        std::copy_n(data_.data() + source, channels_, interleaved.data() + frame * channels_);
    }
    readFrame_.store(read + frames, std::memory_order_release);
    return frames;
}
void AudioStreamRing::reset() noexcept {
    const std::uint64_t write = writeFrame_.load(std::memory_order_acquire);
    readFrame_.store(write, std::memory_order_release);
}

struct CookedAudioStream::Impl {
    explicit Impl(std::size_t capacity) : capacityFrames(capacity) {}
    std::filesystem::path path;
    AudioAssetMetadata metadata;
    std::uint64_t sampleOffset{};
    std::size_t capacityFrames{};
    std::unique_ptr<AudioStreamRing> ring;
    std::jthread worker;
    std::atomic_bool valid{};
    std::atomic_bool running{true};
    std::atomic_bool looping{};
    std::atomic_bool rewindRequested{};
    std::atomic_bool endOfStream{};
    std::atomic<std::uint64_t> produced{};
    std::atomic<std::uint64_t> consumed{};
    std::atomic<std::uint64_t> underruns{};
    std::atomic<std::uint64_t> loops{};

    void run(std::stop_token stop) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) { valid.store(false, std::memory_order_release); return; }
        std::uint64_t cursor{};
        constexpr std::size_t chunkFrames = 1024;
        std::vector<float> chunk(chunkFrames * metadata.channels);
        while (!stop.stop_requested()) {
            if (rewindRequested.exchange(false, std::memory_order_acq_rel)) {
                cursor = metadata.loop.enabled ? metadata.loop.beginFrame : 0U;
                ring->reset();
                endOfStream.store(false, std::memory_order_release);
            }
            if (!running.load(std::memory_order_acquire) || ring->available_write_frames() < 64U) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const std::uint64_t loopBegin = metadata.loop.enabled ? metadata.loop.beginFrame : 0U;
            const std::uint64_t loopEnd = metadata.loop.enabled ? metadata.loop.endFrame : metadata.frameCount;
            if (cursor >= loopEnd) {
                if (looping.load(std::memory_order_acquire)) {
                    cursor = loopBegin;
                    loops.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    endOfStream.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
            }
            const std::size_t writable = std::min<std::size_t>(chunkFrames, ring->available_write_frames());
            const std::size_t frames = static_cast<std::size_t>(std::min<std::uint64_t>(writable, loopEnd - cursor));
            if (frames == 0U) continue;
            stream.clear();
            const std::uint64_t byteOffset = sampleOffset + cursor * metadata.channels * sizeof(float);
            stream.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
            stream.read(reinterpret_cast<char*>(chunk.data()),
                        static_cast<std::streamsize>(frames * metadata.channels * sizeof(float)));
            const std::size_t readSamples = static_cast<std::size_t>(stream.gcount()) / sizeof(float);
            const std::size_t readFrames = readSamples / metadata.channels;
            if (readFrames == 0U) { endOfStream.store(true, std::memory_order_release); continue; }
            const std::size_t written = ring->write(std::span<const float>(chunk.data(), readFrames * metadata.channels));
            cursor += written;
            produced.fetch_add(written, std::memory_order_relaxed);
        }
    }
};

CookedAudioStream::CookedAudioStream(const std::filesystem::path& path, std::size_t capacity,
                                     std::string* error)
    : impl_(std::make_unique<Impl>(std::max<std::size_t>(1024U, capacity))) {
    impl_->path = path;
    std::ifstream stream(path, std::ios::binary);
    CookedHeader header;
    if (!stream || !read_header(stream, header, impl_->metadata, impl_->sampleOffset, error) ||
        impl_->metadata.storagePolicy != AudioStoragePolicy::Streamed) {
        if (error && error->empty()) *error = "asset is not marked for streaming";
        return;
    }
    impl_->ring = std::make_unique<AudioStreamRing>(impl_->metadata.channels, impl_->capacityFrames);
    impl_->looping.store(impl_->metadata.loop.enabled, std::memory_order_relaxed);
    impl_->valid.store(true, std::memory_order_release);
    impl_->worker = std::jthread([this](std::stop_token stop) { impl_->run(stop); });
}
CookedAudioStream::~CookedAudioStream() = default;
bool CookedAudioStream::valid() const noexcept { return impl_ && impl_->valid.load(std::memory_order_acquire); }
const AudioAssetMetadata& CookedAudioStream::metadata() const noexcept { return impl_->metadata; }
void CookedAudioStream::set_looping(bool enabled) noexcept { impl_->looping.store(enabled, std::memory_order_release); }
void CookedAudioStream::request_rewind() noexcept { impl_->rewindRequested.store(true, std::memory_order_release); }
void CookedAudioStream::set_running(bool running) noexcept { impl_->running.store(running, std::memory_order_release); }
std::size_t CookedAudioStream::read(float* output, std::size_t frameCount) noexcept {
    if (!valid() || output == nullptr || frameCount == 0U) return 0U;
    const std::size_t samples = frameCount * impl_->metadata.channels;
    std::fill_n(output, samples, 0.0F);
    const std::size_t readFrames = impl_->ring->read(std::span<float>(output, samples));
    impl_->consumed.fetch_add(readFrames, std::memory_order_relaxed);
    if (readFrames < frameCount) impl_->underruns.fetch_add(frameCount - readFrames, std::memory_order_relaxed);
    return readFrames;
}
AudioStreamTelemetry CookedAudioStream::telemetry() const noexcept {
    AudioStreamTelemetry result;
    if (!impl_) return result;
    result.producedFrames = impl_->produced.load(std::memory_order_relaxed);
    result.consumedFrames = impl_->consumed.load(std::memory_order_relaxed);
    result.underrunFrames = impl_->underruns.load(std::memory_order_relaxed);
    result.loopCount = impl_->loops.load(std::memory_order_relaxed);
    result.bufferedFrames = impl_->ring ? impl_->ring->available_read_frames() : 0U;
    result.capacityFrames = impl_->capacityFrames;
    result.endOfStream = impl_->endOfStream.load(std::memory_order_relaxed);
    result.running = impl_->running.load(std::memory_order_relaxed);
    return result;
}

} // namespace dve::audio
