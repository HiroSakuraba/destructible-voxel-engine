#pragma once
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace dve::audio {
[[nodiscard]] inline std::uint64_t audio_host_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
struct AudioClockAnchor {
    std::uint64_t hostNanoseconds{};
    std::uint64_t generatedFrame{};
    std::uint64_t deviceGeneration{};
    std::uint32_t sampleRate{};
    std::uint32_t queuedFrames{}; // estimated engine-rate frames before new audio is heard
};
struct GameMusicClockAnchor {
    std::uint64_t sampleFrame{};
    double beat{};
    float tempoBpm{120.0F};
    bool running{true};
    std::uint64_t transportGeneration{};
    std::uint64_t revision{};
};
class GameMusicClock {
  public:
    void publish(GameMusicClockAnchor a) noexcept {
        sequence_.fetch_add(1, std::memory_order_seq_cst);
        frame_.store(a.sampleFrame, std::memory_order_seq_cst);
        beat_.store(std::bit_cast<std::uint64_t>(a.beat), std::memory_order_seq_cst);
        tempo_.store(std::bit_cast<std::uint32_t>(a.tempoBpm), std::memory_order_seq_cst);
        running_.store(a.running, std::memory_order_seq_cst);
        generation_.store(a.transportGeneration, std::memory_order_seq_cst);
        sequence_.fetch_add(1, std::memory_order_seq_cst);
    }
    [[nodiscard]] std::optional<GameMusicClockAnchor> anchor() const noexcept {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto sequence = sequence_.load(std::memory_order_seq_cst);
            if (!sequence || (sequence & 1U))
                continue;
            GameMusicClockAnchor a{frame_.load(std::memory_order_seq_cst),
                                   std::bit_cast<double>(beat_.load(std::memory_order_seq_cst)),
                                   std::bit_cast<float>(tempo_.load(std::memory_order_seq_cst)),
                                   running_.load(std::memory_order_seq_cst),
                                   generation_.load(std::memory_order_seq_cst),
                                   sequence};
            if (sequence == sequence_.load(std::memory_order_seq_cst))
                return a;
        }
        return std::nullopt;
    }

  private:
    std::atomic<std::uint64_t> sequence_{}, frame_{}, beat_{}, generation_{};
    std::atomic<std::uint32_t> tempo_{};
    std::atomic<bool> running_{true};
};

struct AudioFrameTarget {
    std::uint64_t frame{};
    bool late{};
    bool stale{};
};
// Single audio-thread publisher; producers read a bounded atomic snapshot.
// Every field is atomic, avoiding the data race of a seqlock over ordinary C++
// storage.
class AudioClock {
  public:
    void publish(AudioClockAnchor a) noexcept {
        sequence_.fetch_add(1, std::memory_order_seq_cst);
        host_.store(a.hostNanoseconds, std::memory_order_seq_cst);
        frame_.store(a.generatedFrame, std::memory_order_seq_cst);
        generation_.store(a.deviceGeneration, std::memory_order_seq_cst);
        rate_.store(a.sampleRate, std::memory_order_seq_cst);
        queued_.store(a.queuedFrames, std::memory_order_seq_cst);
        sequence_.fetch_add(1, std::memory_order_seq_cst);
    }
    [[nodiscard]] std::optional<AudioClockAnchor> anchor() const noexcept {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto before = sequence_.load(std::memory_order_seq_cst);
            if (before & 1U)
                continue;
            AudioClockAnchor a{
                host_.load(std::memory_order_seq_cst), frame_.load(std::memory_order_seq_cst),
                generation_.load(std::memory_order_seq_cst), rate_.load(std::memory_order_seq_cst),
                queued_.load(std::memory_order_seq_cst)};
            if (before == sequence_.load(std::memory_order_seq_cst) && a.sampleRate)
                return a;
        }
        return std::nullopt;
    }
    // Schedule against estimated audible time. Subtract before rounding in signed
    // space; old timestamps cannot underflow an unsigned frame index. Late events
    // run at the earliest safe frame. Generation mismatch is explicit, never
    // silently remapped.
    [[nodiscard]] AudioFrameTarget target(std::uint64_t host, std::uint64_t earliest,
                                          std::uint64_t generation = 0) const noexcept {
        const auto a = anchor();
        if (!a || (generation && generation != a->deviceGeneration))
            return {earliest, false, true};
        const long double elapsed = host >= a->hostNanoseconds
                                        ? static_cast<long double>(host - a->hostNanoseconds)
                                        : -static_cast<long double>(a->hostNanoseconds - host);
        const long double desired = static_cast<long double>(a->generatedFrame) +
                                    elapsed * a->sampleRate / 1000000000.0L - a->queuedFrames;
        if (desired < static_cast<long double>(earliest))
            return {earliest, true, false};
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        return {desired >= static_cast<long double>(maximum)
                    ? maximum
                    : static_cast<std::uint64_t>(std::round(desired)),
                false, false};
    }

  private:
    std::atomic<std::uint64_t> sequence_{}, host_{}, frame_{}, generation_{};
    std::atomic<std::uint32_t> rate_{}, queued_{};
};
} // namespace dve::audio
