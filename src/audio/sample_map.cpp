#include "dve/audio/sample_map.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <type_traits>

namespace dve::audio {
namespace {

constexpr std::array<char, 8> kMagicV2{'D', 'V', 'E', 'S', 'M', 'A', 'P', '2'};
constexpr std::array<char, 8> kMagicV1{'D', 'V', 'E', 'S', 'M', 'A', 'P', '1'};
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool finite(float value) noexcept { return std::isfinite(value); }

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

template <class T>
void hash_value(std::uint64_t& hash, T value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= kFnvPrime;
    }
}

void hash_string(std::uint64_t& hash, const std::string& value) noexcept {
    hash_value(hash, static_cast<std::uint32_t>(value.size()));
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= kFnvPrime;
    }
}

void write_u8(std::ostream& out, std::uint8_t value) { out.put(static_cast<char>(value)); }
void write_u16(std::ostream& out, std::uint16_t value) {
    write_u8(out, static_cast<std::uint8_t>(value & 0xFFU));
    write_u8(out, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}
void write_u32(std::ostream& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        write_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}
void write_u64(std::ostream& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U)
        write_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}
void write_float(std::ostream& out, float value) { write_u32(out, std::bit_cast<std::uint32_t>(value)); }
void write_string(std::ostream& out, const std::string& value) {
    write_u16(out, static_cast<std::uint16_t>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

bool read_exact(std::istream& in, char* target, std::size_t bytes) {
    in.read(target, static_cast<std::streamsize>(bytes));
    return static_cast<std::size_t>(in.gcount()) == bytes;
}
bool read_u8(std::istream& in, std::uint8_t& value) {
    char byte{};
    if (!read_exact(in, &byte, 1U)) return false;
    value = static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
    return true;
}
bool read_u16(std::istream& in, std::uint16_t& value) {
    std::uint8_t a{}, b{};
    if (!read_u8(in, a) || !read_u8(in, b)) return false;
    value = static_cast<std::uint16_t>(a | static_cast<std::uint16_t>(b << 8U));
    return true;
}
bool read_u32(std::istream& in, std::uint32_t& value) {
    value = 0U;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        std::uint8_t byte{};
        if (!read_u8(in, byte)) return false;
        value |= static_cast<std::uint32_t>(byte) << shift;
    }
    return true;
}
bool read_u64(std::istream& in, std::uint64_t& value) {
    value = 0U;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        std::uint8_t byte{};
        if (!read_u8(in, byte)) return false;
        value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return true;
}
bool read_float(std::istream& in, float& value) {
    std::uint32_t bits{};
    if (!read_u32(in, bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}
bool read_string(std::istream& in, std::string& value, std::size_t maximum) {
    std::uint16_t length{};
    if (!read_u16(in, length) || length > maximum) return false;
    value.resize(length);
    return length == 0U || read_exact(in, value.data(), length);
}

} // namespace

bool SynthSampleMap::validate(std::string* error) const {
    const auto fail = [&](std::string message) {
        set_error(error, std::move(message));
        return false;
    };
    if (name.empty() || name.size() > 128U) return fail("sample-map name must contain 1 to 128 characters");
    if (sourceCount == 0U || sourceCount > kSampleMapMaxSources)
        return fail("sample-map source count is out of range");
    if (zoneCount == 0U || zoneCount > kSampleMapMaxZones)
        return fail("sample-map zone count is out of range");
    if (residentFrameCount > kSampleMapMaxResidentFrames)
        return fail("sample-map resident frame count exceeds the fixed callback-safe pool");

    for (std::size_t i = 0; i < sourceCount; ++i) {
        const SampleMapSource& source = sources[i];
        if (source.name.empty() || source.name.size() > 128U || source.streamPath.size() > 1024U)
            return fail("sample-map source text metadata is out of range");
        if (source.sampleRate < 8000U || source.sampleRate > 384000U || source.totalFrameCount < 2U)
            return fail("sample-map source rate or frame count is invalid");
        if (source.residentFrameOffset > residentFrameCount ||
            source.residentFrameCount > residentFrameCount - source.residentFrameOffset)
            return fail("sample-map source resident pool range is invalid");
        if (source.residentSourceStartFrame > source.totalFrameCount ||
            source.residentFrameCount > source.totalFrameCount - source.residentSourceStartFrame)
            return fail("sample-map source resident file range is invalid");
        if (source.preload == SamplePreloadPolicy::Resident &&
            (source.residentSourceStartFrame != 0U || source.residentFrameCount != source.totalFrameCount))
            return fail("resident sample-map source must place the complete source in the resident pool");
        if (source.preload != SamplePreloadPolicy::Resident && source.streamPath.empty())
            return fail("streamed and hybrid sample-map sources require a stream path");
    }
    for (std::size_t i = 0; i < residentFrameCount; ++i) {
        if (!finite(residentSamples[i]) || std::abs(residentSamples[i]) > 8.0F)
            return fail("sample-map resident PCM contains a non-finite or excessive sample");
    }

    for (std::size_t i = 0; i < zoneCount; ++i) {
        const SampleMapZone& zone = zones[i];
        if (!zone.enabled) continue;
        if (zone.sourceIndex >= sourceCount) return fail("sample-map zone references an absent source");
        const SampleMapSource& source = sources[zone.sourceIndex];
        if (zone.keyLow > zone.keyHigh || zone.velocityLow > zone.velocityHigh || zone.velocityHigh > 127U)
            return fail("sample-map key or velocity zone is invalid");
        if (zone.rootNote > 127U || !finite(zone.tuningCents) || !finite(zone.gain) || !finite(zone.pan) ||
            zone.tuningCents < -2400.0F || zone.tuningCents > 2400.0F ||
            zone.gain < 0.0F || zone.gain > 8.0F || zone.pan < -1.0F || zone.pan > 1.0F)
            return fail("sample-map zone tuning, gain, or pan is invalid");
        if (zone.startFrame >= zone.endFrame || zone.endFrame > source.totalFrameCount)
            return fail("sample-map zone playback interval is invalid");
        if (zone.roundRobinGroup > 16U || zone.roundRobinCount == 0U ||
            zone.roundRobinIndex >= zone.roundRobinCount)
            return fail("sample-map round-robin metadata is invalid");
        if (zone.roundRobinGroup == 0U && (zone.roundRobinIndex != 0U || zone.roundRobinCount != 1U))
            return fail("sample-map non-round-robin zone must use index 0 of count 1");
        if (zone.loopMode != SampleLoopMode::Disabled) {
            if (zone.loopStartFrame < zone.startFrame || zone.loopStartFrame >= zone.loopEndFrame ||
                zone.loopEndFrame > zone.endFrame)
                return fail("sample-map loop interval is invalid");
            const std::uint32_t loopFrames = zone.loopEndFrame - zone.loopStartFrame;
            if (zone.loopCrossfadeFrames * 2U > loopFrames)
                return fail("sample-map loop crossfade exceeds half of the loop length");
        } else if (zone.loopCrossfadeFrames != 0U) {
            return fail("sample-map loop crossfade requires a loop");
        }
    }
    return true;
}

std::uint64_t SynthSampleMap::calculate_content_hash() const noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, name);
    hash_value(hash, sourceCount);
    hash_value(hash, zoneCount);
    hash_value(hash, residentFrameCount);
    for (std::size_t i = 0; i < sourceCount; ++i) {
        const SampleMapSource& source = sources[i];
        hash_string(hash, source.name);
        hash_string(hash, source.streamPath);
        hash_value(hash, static_cast<std::uint8_t>(source.preload));
        hash_value(hash, source.sampleRate);
        hash_value(hash, source.totalFrameCount);
        hash_value(hash, source.residentSourceStartFrame);
        hash_value(hash, source.residentFrameOffset);
        hash_value(hash, source.residentFrameCount);
        hash_value(hash, source.contentHash);
    }
    for (std::size_t i = 0; i < zoneCount; ++i) {
        const SampleMapZone& zone = zones[i];
        hash_value(hash, static_cast<std::uint8_t>(zone.enabled));
        hash_value(hash, static_cast<std::uint8_t>(zone.trigger));
        hash_value(hash, zone.sourceIndex);
        hash_value(hash, zone.keyLow);
        hash_value(hash, zone.keyHigh);
        hash_value(hash, zone.velocityLow);
        hash_value(hash, zone.velocityHigh);
        hash_value(hash, zone.rootNote);
        hash_value(hash, zone.roundRobinGroup);
        hash_value(hash, zone.roundRobinIndex);
        hash_value(hash, zone.roundRobinCount);
        hash_value(hash, zone.tuningCents);
        hash_value(hash, zone.gain);
        hash_value(hash, zone.pan);
        hash_value(hash, zone.startFrame);
        hash_value(hash, zone.endFrame);
        hash_value(hash, static_cast<std::uint8_t>(zone.loopMode));
        hash_value(hash, zone.loopStartFrame);
        hash_value(hash, zone.loopEndFrame);
        hash_value(hash, zone.loopCrossfadeFrames);
        hash_value(hash, static_cast<std::uint8_t>(zone.reverse));
    }
    for (std::size_t i = 0; i < residentFrameCount; ++i) hash_value(hash, residentSamples[i]);
    return hash;
}

bool save_sample_map(const std::filesystem::path& path, const SynthSampleMap& map, std::string* error) {
    if (!map.validate(error)) return false;
    if (path.extension() != ".dvesamplemap") {
        set_error(error, "sample maps must use the .dvesamplemap extension");
        return false;
    }
    if (map.name.size() > std::numeric_limits<std::uint16_t>::max()) {
        set_error(error, "sample-map name is too long");
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_error(error, "could not create cooked sample map");
        return false;
    }
    out.write(kMagicV2.data(), static_cast<std::streamsize>(kMagicV2.size()));
    write_u32(out, kSampleMapFormatVersion);
    write_u32(out, map.sourceCount);
    write_u32(out, map.zoneCount);
    write_u32(out, map.residentFrameCount);
    write_u64(out, map.calculate_content_hash());
    write_string(out, map.name);
    for (std::size_t i = 0; i < map.sourceCount; ++i) {
        const SampleMapSource& source = map.sources[i];
        write_string(out, source.name);
        write_string(out, source.streamPath);
        write_u8(out, static_cast<std::uint8_t>(source.preload));
        write_u32(out, source.sampleRate);
        write_u32(out, source.totalFrameCount);
        write_u32(out, source.residentSourceStartFrame);
        write_u32(out, source.residentFrameOffset);
        write_u32(out, source.residentFrameCount);
        write_u64(out, source.contentHash);
    }
    for (std::size_t i = 0; i < map.zoneCount; ++i) {
        const SampleMapZone& zone = map.zones[i];
        write_u8(out, static_cast<std::uint8_t>(zone.enabled));
        write_u8(out, static_cast<std::uint8_t>(zone.trigger));
        write_u8(out, zone.sourceIndex);
        write_u8(out, zone.keyLow);
        write_u8(out, zone.keyHigh);
        write_u8(out, zone.velocityLow);
        write_u8(out, zone.velocityHigh);
        write_u8(out, zone.rootNote);
        write_u8(out, zone.roundRobinGroup);
        write_u8(out, zone.roundRobinIndex);
        write_u8(out, zone.roundRobinCount);
        write_float(out, zone.tuningCents);
        write_float(out, zone.gain);
        write_float(out, zone.pan);
        write_u32(out, zone.startFrame);
        write_u32(out, zone.endFrame);
        write_u8(out, static_cast<std::uint8_t>(zone.loopMode));
        write_u32(out, zone.loopStartFrame);
        write_u32(out, zone.loopEndFrame);
        write_u32(out, zone.loopCrossfadeFrames);
        write_u8(out, static_cast<std::uint8_t>(zone.reverse));
    }
    for (std::size_t i = 0; i < map.residentFrameCount; ++i) write_float(out, map.residentSamples[i]);
    if (!out) {
        set_error(error, "could not finish cooked sample map");
        return false;
    }
    return true;
}

bool load_sample_map(const std::filesystem::path& path, SynthSampleMap& map, std::string* error) {
    std::error_code filesystemError;
    const std::uintmax_t bytes = std::filesystem::file_size(path, filesystemError);
    if (filesystemError || bytes < 32U || bytes > 64U * 1024U * 1024U) {
        set_error(error, "cooked sample-map file size is invalid");
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "could not open cooked sample map");
        return false;
    }
    std::array<char, 8> magic{};
    if (!read_exact(in, magic.data(), magic.size()) || (magic != kMagicV2 && magic != kMagicV1)) {
        set_error(error, "cooked sample-map magic is invalid");
        return false;
    }
    const bool legacyRuntimeLayout = magic == kMagicV1;
    std::uint32_t version{}, sourceCount{}, zoneCount{}, residentFrameCount{};
    std::uint64_t storedHash{};
    if (!read_u32(in, version) || !read_u32(in, sourceCount) || !read_u32(in, zoneCount) ||
        !read_u32(in, residentFrameCount) || !read_u64(in, storedHash)) {
        set_error(error, "cooked sample-map header is truncated");
        return false;
    }
    const std::uint32_t expectedVersion = legacyRuntimeLayout ? 1U : kSampleMapFormatVersion;
    if (version != expectedVersion || sourceCount > kSampleMapMaxSources ||
        zoneCount > kSampleMapMaxZones || residentFrameCount > kSampleMapMaxResidentFrames) {
        set_error(error, "cooked sample-map version or fixed-capacity counts are unsupported");
        return false;
    }
    SynthSampleMap candidate;
    candidate.sourceCount = sourceCount;
    candidate.zoneCount = zoneCount;
    candidate.residentFrameCount = residentFrameCount;
    if (!read_string(in, candidate.name, 128U)) {
        set_error(error, "cooked sample-map name is invalid");
        return false;
    }
    for (std::size_t i = 0; i < sourceCount; ++i) {
        SampleMapSource& source = candidate.sources[i];
        std::uint8_t preload{};
        if (!read_string(in, source.name, 128U) || !read_string(in, source.streamPath, 1024U) ||
            !read_u8(in, preload) || !read_u32(in, source.sampleRate) ||
            !read_u32(in, source.totalFrameCount) || !read_u32(in, source.residentSourceStartFrame) ||
            !read_u32(in, source.residentFrameOffset) || !read_u32(in, source.residentFrameCount) ||
            !read_u64(in, source.contentHash) || preload > static_cast<std::uint8_t>(SamplePreloadPolicy::Hybrid)) {
            set_error(error, "cooked sample-map source record is invalid or truncated");
            return false;
        }
        source.preload = static_cast<SamplePreloadPolicy>(preload);
    }
    for (std::size_t i = 0; i < zoneCount; ++i) {
        SampleMapZone& zone = candidate.zones[i];
        std::uint8_t enabled{}, trigger{}, loopMode{}, reverse{};
        if (!read_u8(in, enabled) || !read_u8(in, trigger) || !read_u8(in, zone.sourceIndex) ||
            !read_u8(in, zone.keyLow) || !read_u8(in, zone.keyHigh) ||
            !read_u8(in, zone.velocityLow) || !read_u8(in, zone.velocityHigh) ||
            !read_u8(in, zone.rootNote) || !read_u8(in, zone.roundRobinGroup) ||
            !read_u8(in, zone.roundRobinIndex) || !read_u8(in, zone.roundRobinCount) ||
            !read_float(in, zone.tuningCents) || !read_float(in, zone.gain) || !read_float(in, zone.pan) ||
            !read_u32(in, zone.startFrame) || !read_u32(in, zone.endFrame) || !read_u8(in, loopMode) ||
            !read_u32(in, zone.loopStartFrame) || !read_u32(in, zone.loopEndFrame) ||
            !read_u32(in, zone.loopCrossfadeFrames) || !read_u8(in, reverse) ||
            enabled > 1U || trigger > static_cast<std::uint8_t>(SampleTrigger::Release) ||
            loopMode > static_cast<std::uint8_t>(SampleLoopMode::Forward) || reverse > 1U) {
            set_error(error, "cooked sample-map zone record is invalid or truncated");
            return false;
        }
        zone.enabled = enabled != 0U;
        zone.trigger = static_cast<SampleTrigger>(trigger);
        zone.loopMode = static_cast<SampleLoopMode>(loopMode);
        zone.reverse = reverse != 0U;
    }
    for (std::size_t i = 0; i < residentFrameCount; ++i) {
        if (!read_float(in, candidate.residentSamples[i])) {
            set_error(error, "cooked sample-map PCM payload is truncated");
            return false;
        }
    }
    char trailing{};
    if (in.read(&trailing, 1)) {
        set_error(error, "cooked sample map contains trailing data");
        return false;
    }
    if (!candidate.validate(error)) return false;
    candidate.contentHash = candidate.calculate_content_hash();
    if (candidate.contentHash != storedHash) {
        set_error(error, "cooked sample-map content hash mismatch");
        return false;
    }
    map = std::move(candidate);
    return true;
}

SampleStreamCache::SampleStreamCache() noexcept = default;

bool SampleStreamCache::publish_page(std::uint64_t generation, std::uint8_t sourceIndex,
                                     std::uint32_t firstFrame,
                                     std::span<const float> monoFrames) noexcept {
    if (generation == 0U || sourceIndex >= kSampleMapMaxSources || monoFrames.empty() ||
        monoFrames.size() > kSampleStreamPageFrames || firstFrame % kSampleStreamPageFrames != 0U)
        return false;
    for (const float sample : monoFrames) if (!finite(sample) || std::abs(sample) > 8.0F) return false;
    const std::uint32_t pageIndex = firstFrame / static_cast<std::uint32_t>(kSampleStreamPageFrames);
    Page& page = pages_[pageIndex % kSampleStreamPageCount];
    const bool replacement = page.valid && (page.generation != generation || page.sourceIndex != sourceIndex ||
                                             page.pageIndex != pageIndex);
    page.sequence.fetch_add(1U, std::memory_order_acq_rel); // odd: writer owns the slot
    page.valid = false;
    page.generation = generation;
    page.sourceIndex = sourceIndex;
    page.pageIndex = pageIndex;
    page.frameCount = static_cast<std::uint16_t>(monoFrames.size());
    std::copy(monoFrames.begin(), monoFrames.end(), page.samples.begin());
    std::fill(page.samples.begin() + static_cast<std::ptrdiff_t>(monoFrames.size()), page.samples.end(), 0.0F);
    page.valid = true;
    page.sequence.fetch_add(1U, std::memory_order_release); // even: readers may consume
    pagesPublished_.fetch_add(1U, std::memory_order_relaxed);
    if (replacement) pageReplacements_.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool SampleStreamCache::read_frame(std::uint64_t generation, std::uint8_t sourceIndex,
                                   std::uint32_t frame, float& value) const noexcept {
    const std::uint32_t pageIndex = frame / static_cast<std::uint32_t>(kSampleStreamPageFrames);
    const std::uint32_t offset = frame % static_cast<std::uint32_t>(kSampleStreamPageFrames);
    const Page& page = pages_[pageIndex % kSampleStreamPageCount];
    for (unsigned attempt = 0; attempt < 2U; ++attempt) {
        const std::uint64_t before = page.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        if (!page.valid || page.generation != generation || page.sourceIndex != sourceIndex ||
            page.pageIndex != pageIndex || offset >= page.frameCount)
            return false;
        const float sample = page.samples[offset];
        const std::uint64_t after = page.sequence.load(std::memory_order_acquire);
        if (before == after) {
            value = sample;
            return true;
        }
    }
    return false;
}

bool SampleStreamCache::read_frame_value(std::uint64_t generation, std::uint8_t sourceIndex,
                                                std::uint32_t frame, float& value) const noexcept {
    if (!read_frame(generation, sourceIndex, frame, value)) {
        readMisses_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    readHits_.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool SampleStreamCache::read_linear(std::uint64_t generation, std::uint8_t sourceIndex,
                                    float framePosition, float& value) const noexcept {
    if (!finite(framePosition) || framePosition < 0.0F ||
        framePosition > static_cast<float>(std::numeric_limits<std::uint32_t>::max() - 1U)) {
        readMisses_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const std::uint32_t frame0 = static_cast<std::uint32_t>(framePosition);
    const std::uint32_t frame1 = frame0 + 1U;
    float a{}, b{};
    if (!read_frame(generation, sourceIndex, frame0, a) || !read_frame(generation, sourceIndex, frame1, b)) {
        readMisses_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    value = a + (b - a) * (framePosition - static_cast<float>(frame0));
    readHits_.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

SampleStreamMetrics SampleStreamCache::metrics() const noexcept {
    return {pagesPublished_.load(std::memory_order_relaxed),
            pageReplacements_.load(std::memory_order_relaxed),
            readHits_.load(std::memory_order_relaxed),
            readMisses_.load(std::memory_order_relaxed)};
}

void SampleStreamCache::reset_metrics() noexcept {
    pagesPublished_.store(0U, std::memory_order_relaxed);
    pageReplacements_.store(0U, std::memory_order_relaxed);
    readHits_.store(0U, std::memory_order_relaxed);
    readMisses_.store(0U, std::memory_order_relaxed);
}

} // namespace dve::audio
