#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/chiptune.hpp"
#include "dve/audio/mixer.hpp"

namespace dve::audio {

inline constexpr std::size_t kChiptuneAuthoringHistoryLimit = 128U;

enum class ChipTrackerField : std::uint8_t { Note, Instrument, Volume, Effect, EffectParameter };
enum class ChipEnvelopeLane : std::uint8_t { Volume, Release, Pitch, Duty, Arpeggio };
enum class ChipWavetableShape : std::uint8_t { Sine, Triangle, Sawtooth, Pulse, Silence };

struct ChipTrackerPosition {
    std::uint32_t order{};
    std::uint32_t row{};
    std::uint32_t channel{};
    ChipTrackerField field{ChipTrackerField::Note};
    auto operator<=>(const ChipTrackerPosition&) const = default;
};

struct ChipTrackerSelection {
    ChipTrackerPosition anchor{};
    ChipTrackerPosition cursor{};
    [[nodiscard]] std::uint32_t first_row() const noexcept;
    [[nodiscard]] std::uint32_t last_row() const noexcept;
    [[nodiscard]] std::uint32_t first_channel() const noexcept;
    [[nodiscard]] std::uint32_t last_channel() const noexcept;
};

struct ChipTrackerClipboard {
    std::uint32_t rows{};
    std::uint32_t channels{};
    std::vector<ChipCell> cells;
    [[nodiscard]] bool empty() const noexcept { return rows == 0U || channels == 0U || cells.empty(); }
};

struct ChipEffectHelperInput {
    int primary{};
    int secondary{};
};

struct ChiptuneAuditionResult {
    AudioSourceHandle source{};
    SampleId sample{};
    AudioBusId bus{AudioBusId::Music};
    std::uint64_t contentHash{};
    bool livePreview{};
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return livePreview || static_cast<bool>(source); }
};

// Transactional editor model for .dvechip songs. All mutations validate before publication and
// produce bounded undo records. The sample renderer and mixer bridge are control-thread tools;
// ChiptunePlayer remains the allocation-free callback path after load.
class ChiptuneAuthoringSession {
public:
    explicit ChiptuneAuthoringSession(ChipSong song = make_demo_chiptune_song());

    [[nodiscard]] const ChipSong& song() const noexcept { return song_; }
    [[nodiscard]] ChipSong& song_for_inspection() noexcept { return song_; }
    [[nodiscard]] const ChipTrackerPosition& cursor() const noexcept { return cursor_; }
    [[nodiscard]] const std::optional<ChipTrackerSelection>& selection() const noexcept { return selection_; }
    [[nodiscard]] const ChipTrackerClipboard& clipboard() const noexcept { return clipboard_; }
    [[nodiscard]] std::size_t selected_instrument() const noexcept { return selectedInstrument_; }
    [[nodiscard]] int octave() const noexcept { return octave_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::string_view undo_label() const noexcept;
    [[nodiscard]] std::string_view redo_label() const noexcept;
    [[nodiscard]] AudioBusId song_bus() const noexcept { return songBus_; }
    [[nodiscard]] AudioBusId instrument_bus() const noexcept { return instrumentBus_; }
    [[nodiscard]] const ChipSfxRequest& sfx_request() const noexcept { return sfxRequest_; }

    void mark_saved() noexcept { dirty_ = false; }
    bool load_file(const std::filesystem::path& path, std::string* error = nullptr);
    bool save_file(const std::filesystem::path& path, std::string* error = nullptr);

    bool set_cursor(ChipTrackerPosition position) noexcept;
    void set_selection(std::optional<ChipTrackerSelection> selection) noexcept;
    void select_all() noexcept;
    void collapse_selection() noexcept { selection_.reset(); }
    void set_selected_instrument(std::size_t index) noexcept;
    void set_octave(int octave) noexcept;
    void set_song_bus(AudioBusId bus) noexcept { songBus_ = bus; }
    void set_instrument_bus(AudioBusId bus) noexcept { instrumentBus_ = bus; }

    bool set_cell(ChipTrackerPosition position, const ChipCell& cell, std::string* error = nullptr);
    bool clear_cell(ChipTrackerPosition position, std::string* error = nullptr);
    bool enter_midi_note(int midiNote, bool noteOff = false, std::uint32_t advanceRows = 1U,
                         std::string* error = nullptr);
    bool enter_tracker_key(std::string_view key, bool noteOff = false,
                           std::uint32_t advanceRows = 1U, std::string* error = nullptr);
    bool set_current_volume(int volume, std::string* error = nullptr);
    bool set_current_instrument(std::size_t instrument, std::string* error = nullptr);
    bool set_current_effect(ChipEffect effect, ChipEffectHelperInput input = {},
                            std::string* error = nullptr);
    bool transpose_selection(int semitones, std::string* error = nullptr);
    bool clear_selection(std::string* error = nullptr);

    bool copy_selection(std::string* error = nullptr);
    bool cut_selection(std::string* error = nullptr);
    bool paste_at_cursor(bool mix = false, std::string* error = nullptr);

    bool add_pattern(std::uint32_t rows = 64U, std::optional<std::uint32_t> duplicate = std::nullopt,
                     std::string* error = nullptr);
    bool remove_pattern(std::uint32_t pattern, std::string* error = nullptr);
    bool resize_pattern(std::uint32_t pattern, std::uint32_t rows, std::string* error = nullptr);
    bool insert_order(std::uint32_t index, std::uint32_t pattern, std::string* error = nullptr);
    bool delete_order(std::uint32_t index, std::string* error = nullptr);
    bool set_order_pattern(std::uint32_t index, std::uint32_t pattern,
                           std::string* error = nullptr);
    bool duplicate_order_pattern(std::uint32_t index, std::string* error = nullptr);

    bool add_instrument(ChipInstrument instrument = {}, std::string* error = nullptr);
    bool remove_instrument(std::size_t instrument, std::string* error = nullptr);
    bool replace_instrument(std::size_t instrument, ChipInstrument replacement,
                            std::string* error = nullptr);
    bool draw_envelope_point(ChipEnvelopeLane lane, std::size_t index, float value,
                             std::string* error = nullptr);
    bool resize_envelope(ChipEnvelopeLane lane, std::size_t size, float fill = 0.0F,
                         std::string* error = nullptr);
    bool set_envelope_loop(ChipEnvelopeLane lane, std::size_t loop,
                           std::string* error = nullptr);
    bool draw_wavetable_point(std::size_t index, float value, std::string* error = nullptr);
    bool resize_wavetable(std::size_t size, std::string* error = nullptr);
    bool generate_wavetable(ChipWavetableShape shape, std::size_t size = 32U,
                            float pulseDuty = 0.5F, std::string* error = nullptr);
    bool normalize_wavetable(std::string* error = nullptr);
    bool remove_wavetable_dc(std::string* error = nullptr);

    void set_sfx_request(ChipSfxRequest request) noexcept;
    bool apply_sfx_request(std::string* error = nullptr);

    [[nodiscard]] DecodedAudioAsset render_song_preview(float seconds = 30.0F,
                                                        std::string* error = nullptr) const;
    [[nodiscard]] DecodedAudioAsset render_instrument_preview(int midiNote = 60,
                                                              float seconds = 1.0F,
                                                              std::string* error = nullptr) const;
    [[nodiscard]] ChiptuneAuditionResult audition_song(AudioMixer& mixer, float seconds = 30.0F,
                                                       bool loop = false);
    [[nodiscard]] ChiptuneAuditionResult audition_instrument(AudioMixer& mixer, int midiNote = 60,
                                                             float seconds = 1.0F);
    void stop_audition(AudioMixer& mixer, float fadeSeconds = 0.02F) noexcept;

    bool undo(std::string* error = nullptr);
    bool redo(std::string* error = nullptr);

    [[nodiscard]] static int tracker_key_to_midi(std::string_view key, int octave) noexcept;
    [[nodiscard]] static std::uint8_t encode_effect_parameter(ChipEffect effect,
                                                              ChipEffectHelperInput input) noexcept;

private:
    struct Snapshot {
        ChipSong song;
        ChipTrackerPosition cursor{};
        std::optional<ChipTrackerSelection> selection;
        std::size_t selectedInstrument{};
        int octave{4};
        ChipSfxRequest sfxRequest{};
        AudioBusId songBus{AudioBusId::Music};
        AudioBusId instrumentBus{AudioBusId::Effects};
        std::string label;
    };

    [[nodiscard]] Snapshot snapshot(std::string label) const;
    void restore(const Snapshot& state);
    bool publish(ChipSong candidate, std::string label, std::string* error);
    [[nodiscard]] bool clamp_position(ChipTrackerPosition& position) const noexcept;
    [[nodiscard]] std::uint32_t pattern_for_order(std::uint32_t order) const noexcept;
    [[nodiscard]] ChipCell* mutable_cell(ChipSong& song, ChipTrackerPosition position) noexcept;
    [[nodiscard]] const ChipCell* cell(ChipTrackerPosition position) const noexcept;
    [[nodiscard]] ChipTrackerSelection effective_selection() const noexcept;
    [[nodiscard]] ChipInstrument* selected_instrument_ptr(ChipSong& song) noexcept;
    [[nodiscard]] const ChipInstrument* selected_instrument_ptr() const noexcept;

    ChipSong song_{};
    ChipTrackerPosition cursor_{};
    std::optional<ChipTrackerSelection> selection_{};
    ChipTrackerClipboard clipboard_{};
    std::size_t selectedInstrument_{};
    int octave_{4};
    bool dirty_{};
    ChipSfxRequest sfxRequest_{};
    AudioBusId songBus_{AudioBusId::Music};
    AudioBusId instrumentBus_{AudioBusId::Effects};
    AudioSourceHandle auditionSource_{};
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
};

} // namespace dve::audio
