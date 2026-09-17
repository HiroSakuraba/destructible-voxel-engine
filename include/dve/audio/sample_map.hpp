#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace dve::audio {

inline constexpr std::uint32_t kSampleMapFormatVersion = 2;
inline constexpr std::size_t kSampleMapMaxSources = 8;
inline constexpr std::size_t kSampleMapMaxZones = 32;
inline constexpr std::size_t kSampleMapMaxResidentFrames = 131072;
inline constexpr std::size_t kSampleStreamPageFrames = 2048;
inline constexpr std::size_t kSampleStreamPageCount = 32;
inline constexpr std::uint8_t kInvalidSampleZone = 0xFFU;

enum class SamplePreloadPolicy : std::uint8_t { Resident, Streamed, Hybrid };
enum class SampleTrigger : std::uint8_t { Attack, Release };
enum class SampleLoopMode : std::uint8_t { Disabled, Forward };

struct SampleMapSource {
    std::string name{"Sample"};
    std::string streamPath{};
    SamplePreloadPolicy preload{SamplePreloadPolicy::Resident};
    std::uint32_t sampleRate{48000};
    std::uint32_t totalFrameCount{};
    std::uint32_t residentSourceStartFrame{};
    std::uint32_t residentFrameOffset{};
    std::uint32_t residentFrameCount{};
    std::uint64_t contentHash{};
};

// Frame intervals are half-open: [startFrame, endFrame) and [loopStartFrame, loopEndFrame).
struct SampleMapZone {
    bool enabled{true};
    SampleTrigger trigger{SampleTrigger::Attack};
    std::uint8_t sourceIndex{};
    std::uint8_t keyLow{};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    std::uint8_t rootNote{60};
    std::uint8_t roundRobinGroup{}; // 0 disables round robin; valid groups are 1..16.
    std::uint8_t roundRobinIndex{};
    std::uint8_t roundRobinCount{1};
    float tuningCents{};
    float gain{1.0F};
    float pan{};
    std::uint32_t startFrame{};
    std::uint32_t endFrame{};
    SampleLoopMode loopMode{SampleLoopMode::Disabled};
    std::uint32_t loopStartFrame{};
    std::uint32_t loopEndFrame{};
    std::uint32_t loopCrossfadeFrames{};
    bool reverse{};
};

struct SynthSampleMap {
    std::string name{"Untitled Sample Map"};
    std::uint32_t sourceCount{};
    std::uint32_t zoneCount{};
    std::uint32_t residentFrameCount{};
    std::array<SampleMapSource, kSampleMapMaxSources> sources{};
    std::array<SampleMapZone, kSampleMapMaxZones> zones{};
    std::array<float, kSampleMapMaxResidentFrames> residentSamples{};
    std::uint64_t contentHash{};
    std::uint64_t runtimeGeneration{}; // Runtime-only; not part of the cooked hash.

    [[nodiscard]] bool enabled() const noexcept { return sourceCount > 0U && zoneCount > 0U; }
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] std::uint64_t calculate_content_hash() const noexcept;
};

[[nodiscard]] bool save_sample_map(const std::filesystem::path& path,
                                   const SynthSampleMap& map,
                                   std::string* error = nullptr);
[[nodiscard]] bool load_sample_map(const std::filesystem::path& path,
                                   SynthSampleMap& map,
                                   std::string* error = nullptr);

struct SampleStreamMetrics {
    std::uint64_t pagesPublished{};
    std::uint64_t pageReplacements{};
    std::uint64_t readHits{};
    std::uint64_t readMisses{};
};

// Direct-mapped, generation-tagged page cache. A background worker publishes complete,
// aligned pages; the audio callback performs bounded lock-free reads and never accesses files.
// One publisher thread and one or more readers are supported.
class SampleStreamCache {
public:
    SampleStreamCache() noexcept;

    [[nodiscard]] bool publish_page(std::uint64_t generation,
                                    std::uint8_t sourceIndex,
                                    std::uint32_t firstFrame,
                                    std::span<const float> monoFrames) noexcept;
    [[nodiscard]] bool read_frame_value(std::uint64_t generation,
                                         std::uint8_t sourceIndex,
                                         std::uint32_t frame,
                                         float& value) const noexcept;
    [[nodiscard]] bool read_linear(std::uint64_t generation,
                                   std::uint8_t sourceIndex,
                                   float framePosition,
                                   float& value) const noexcept;
    [[nodiscard]] SampleStreamMetrics metrics() const noexcept;
    void reset_metrics() noexcept;

private:
    struct Page {
        std::atomic<std::uint64_t> sequence{};
        std::uint64_t generation{};
        std::uint32_t pageIndex{};
        std::uint16_t frameCount{};
        std::uint8_t sourceIndex{};
        bool valid{};
        std::array<float, kSampleStreamPageFrames> samples{};
    };

    [[nodiscard]] bool read_frame(std::uint64_t generation,
                                  std::uint8_t sourceIndex,
                                  std::uint32_t frame,
                                  float& value) const noexcept;

    std::array<Page, kSampleStreamPageCount> pages_{};
    mutable std::atomic<std::uint64_t> pagesPublished_{};
    mutable std::atomic<std::uint64_t> pageReplacements_{};
    mutable std::atomic<std::uint64_t> readHits_{};
    mutable std::atomic<std::uint64_t> readMisses_{};
};

} // namespace dve::audio
