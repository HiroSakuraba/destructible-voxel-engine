#include "dve/audio/chiptune_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <utility>

namespace dve::audio {
namespace {

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

[[nodiscard]] std::uint8_t midi_to_chip(int midi) noexcept {
    const int encoded = midi - 11;
    if (encoded < 1 || encoded >= static_cast<int>(kChipNoteOff)) return kChipNoteNone;
    return static_cast<std::uint8_t>(encoded);
}

[[nodiscard]] bool cell_empty(const ChipCell& cell) noexcept {
    return cell.note == kChipNoteNone && cell.instrument == 0U &&
           cell.volume == kChipVolumeNone && cell.effect == ChipEffect::NoEffect &&
           cell.effectParam == 0U;
}

[[nodiscard]] bool read_text(const std::filesystem::path& path, std::string& text,
                             std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "unable to open chiptune asset";
        return false;
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    if (!input.good() && !input.eof()) {
        if (error) *error = "unable to read chiptune asset";
        return false;
    }
    text = stream.str();
    return true;
}

[[nodiscard]] bool write_text_atomic(const std::filesystem::path& path, std::string_view text,
                                     std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "unable to create chiptune output directory";
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error) *error = "unable to create temporary chiptune asset";
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) {
            if (error) *error = "unable to write temporary chiptune asset";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = "unable to publish chiptune asset";
        return false;
    }
    return true;
}

[[nodiscard]] ChipInstrument default_instrument() {
    ChipInstrument instrument;
    instrument.name = "New Instrument";
    instrument.wave = ChipWave::Pulse;
    instrument.volume.values = {1.0F, 0.85F, 0.72F, 0.62F};
    instrument.volume.releaseValues = {0.5F, 0.25F, 0.0F};
    instrument.volume.loopIndex = 3U;
    return instrument;
}

[[nodiscard]] ChipSong make_instrument_preview_song(const ChipSong& source,
                                                     const ChipInstrument* selected,
                                                     int midiNote) {
    ChipSong preview;
    preview.name = selected ? selected->name + " preview" : "instrument preview";
    preview.sampleRate = source.sampleRate;
    preview.channelCount = 1U;
    preview.ticksPerRow = 3U;
    preview.ticksPerSecond = 60U;
    preview.masterGain = 0.8F;
    preview.loop = false;
    preview.instruments = {selected ? *selected : default_instrument()};
    ChipPattern pattern;
    pattern.rowCount = 16U;
    pattern.cells.resize(16U);
    pattern.cells[0].note = midi_to_chip(std::clamp(midiNote, 12, 119));
    pattern.cells[0].instrument = 1U;
    pattern.cells[0].volume = 15U;
    pattern.cells[10].note = kChipNoteOff;
    preview.patterns = {std::move(pattern)};
    preview.order = {0U};
    return preview;
}

} // namespace

std::uint32_t ChipTrackerSelection::first_row() const noexcept {
    return std::min(anchor.row, cursor.row);
}
std::uint32_t ChipTrackerSelection::last_row() const noexcept {
    return std::max(anchor.row, cursor.row);
}
std::uint32_t ChipTrackerSelection::first_channel() const noexcept {
    return std::min(anchor.channel, cursor.channel);
}
std::uint32_t ChipTrackerSelection::last_channel() const noexcept {
    return std::max(anchor.channel, cursor.channel);
}

ChiptuneAuthoringSession::ChiptuneAuthoringSession(ChipSong song) : song_(std::move(song)) {
    std::string error;
    if (!song_.validate(&error)) song_ = make_demo_chiptune_song();
    if (song_.instruments.empty()) song_.instruments.push_back(default_instrument());
    (void)clamp_position(cursor_);
}

std::string_view ChiptuneAuthoringSession::undo_label() const noexcept {
    return undo_.empty() ? std::string_view{} : std::string_view(undo_.back().label);
}
std::string_view ChiptuneAuthoringSession::redo_label() const noexcept {
    return redo_.empty() ? std::string_view{} : std::string_view(redo_.back().label);
}

bool ChiptuneAuthoringSession::load_file(const std::filesystem::path& path, std::string* error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    ChipSong loaded;
    if (!ChipSong::parse(text, loaded, error)) return false;
    undo_.clear();
    redo_.clear();
    song_ = std::move(loaded);
    cursor_ = {};
    selection_.reset();
    selectedInstrument_ = 0U;
    dirty_ = false;
    return true;
}

bool ChiptuneAuthoringSession::save_file(const std::filesystem::path& path, std::string* error) {
    std::string validation;
    if (!song_.validate(&validation)) {
        if (error) *error = validation;
        return false;
    }
    if (!write_text_atomic(path, song_.serialize(), error)) return false;
    dirty_ = false;
    return true;
}

bool ChiptuneAuthoringSession::set_cursor(ChipTrackerPosition position) noexcept {
    if (!clamp_position(position)) return false;
    cursor_ = position;
    return true;
}

void ChiptuneAuthoringSession::set_selection(std::optional<ChipTrackerSelection> selection) noexcept {
    if (selection) {
        (void)clamp_position(selection->anchor);
        (void)clamp_position(selection->cursor);
        if (selection->anchor.order != selection->cursor.order)
            selection->cursor.order = selection->anchor.order;
    }
    selection_ = std::move(selection);
}

void ChiptuneAuthoringSession::select_all() noexcept {
    if (song_.order.empty()) return;
    const std::uint32_t patternIndex = pattern_for_order(cursor_.order);
    if (patternIndex >= song_.patterns.size()) return;
    ChipTrackerSelection selection;
    selection.anchor = {cursor_.order, 0U, 0U, ChipTrackerField::Note};
    selection.cursor = {cursor_.order, song_.patterns[patternIndex].rowCount - 1U,
                        song_.channelCount - 1U, ChipTrackerField::EffectParameter};
    selection_ = selection;
}

void ChiptuneAuthoringSession::set_selected_instrument(std::size_t index) noexcept {
    selectedInstrument_ = song_.instruments.empty() ? 0U : std::min(index, song_.instruments.size() - 1U);
}

void ChiptuneAuthoringSession::set_octave(int octave) noexcept {
    octave_ = std::clamp(octave, 0, 8);
}

bool ChiptuneAuthoringSession::set_cell(ChipTrackerPosition position, const ChipCell& value,
                                        std::string* error) {
    if (!clamp_position(position)) {
        if (error) *error = "tracker position is invalid";
        return false;
    }
    ChipSong candidate = song_;
    ChipCell* target = mutable_cell(candidate, position);
    if (!target) {
        if (error) *error = "tracker cell is unavailable";
        return false;
    }
    *target = value;
    if (!publish(std::move(candidate), "Edit tracker cell", error)) return false;
    cursor_ = position;
    return true;
}

bool ChiptuneAuthoringSession::clear_cell(ChipTrackerPosition position, std::string* error) {
    return set_cell(position, {}, error);
}

bool ChiptuneAuthoringSession::enter_midi_note(int midiNote, bool noteOff,
                                               std::uint32_t advanceRows,
                                               std::string* error) {
    ChipCell value = cell(cursor_) ? *cell(cursor_) : ChipCell{};
    if (noteOff) value.note = kChipNoteOff;
    else {
        value.note = midi_to_chip(midiNote);
        if (value.note == kChipNoteNone) {
            if (error) *error = "MIDI note is outside the tracker range";
            return false;
        }
        value.instrument = static_cast<std::uint8_t>(std::min<std::size_t>(255U, selectedInstrument_ + 1U));
    }
    const ChipTrackerPosition original = cursor_;
    if (!set_cell(cursor_, value, error)) return false;
    cursor_ = original;
    const std::uint32_t patternIndex = pattern_for_order(cursor_.order);
    if (patternIndex < song_.patterns.size())
        cursor_.row = std::min(cursor_.row + advanceRows, song_.patterns[patternIndex].rowCount - 1U);
    return true;
}

bool ChiptuneAuthoringSession::enter_tracker_key(std::string_view key, bool noteOff,
                                                 std::uint32_t advanceRows,
                                                 std::string* error) {
    const int midi = tracker_key_to_midi(key, octave_);
    if (midi < 0 && !noteOff) return false;
    return enter_midi_note(std::max(midi, 0), noteOff, advanceRows, error);
}

bool ChiptuneAuthoringSession::set_current_volume(int volume, std::string* error) {
    if (volume < 0 || volume > 15) {
        if (error) *error = "tracker volume must be in [0,15]";
        return false;
    }
    ChipCell value = cell(cursor_) ? *cell(cursor_) : ChipCell{};
    value.volume = static_cast<std::uint8_t>(volume);
    return set_cell(cursor_, value, error);
}

bool ChiptuneAuthoringSession::set_current_instrument(std::size_t instrument,
                                                      std::string* error) {
    if (instrument >= song_.instruments.size() || instrument >= 255U) {
        if (error) *error = "instrument index is invalid";
        return false;
    }
    ChipCell value = cell(cursor_) ? *cell(cursor_) : ChipCell{};
    value.instrument = static_cast<std::uint8_t>(instrument + 1U);
    return set_cell(cursor_, value, error);
}

bool ChiptuneAuthoringSession::set_current_effect(ChipEffect effect, ChipEffectHelperInput input,
                                                  std::string* error) {
    ChipCell value = cell(cursor_) ? *cell(cursor_) : ChipCell{};
    value.effect = effect;
    value.effectParam = encode_effect_parameter(effect, input);
    return set_cell(cursor_, value, error);
}

bool ChiptuneAuthoringSession::transpose_selection(int semitones, std::string* error) {
    ChipSong candidate = song_;
    const auto range = effective_selection();
    const std::uint32_t patternIndex = pattern_for_order(range.anchor.order);
    if (patternIndex >= candidate.patterns.size()) return false;
    for (std::uint32_t row = range.first_row(); row <= range.last_row(); ++row) {
        for (std::uint32_t channel = range.first_channel(); channel <= range.last_channel(); ++channel) {
            ChipCell& value = candidate.patterns[patternIndex].at(row, channel, candidate.channelCount);
            if (value.note == kChipNoteNone || value.note == kChipNoteOff) continue;
            const int midi = chip_note_to_midi(value.note) + semitones;
            const auto encoded = midi_to_chip(midi);
            if (encoded == kChipNoteNone) {
                if (error) *error = "transpose would move a note outside the tracker range";
                return false;
            }
            value.note = encoded;
        }
    }
    return publish(std::move(candidate), "Transpose selection", error);
}

bool ChiptuneAuthoringSession::clear_selection(std::string* error) {
    ChipSong candidate = song_;
    const auto range = effective_selection();
    const std::uint32_t patternIndex = pattern_for_order(range.anchor.order);
    if (patternIndex >= candidate.patterns.size()) return false;
    for (std::uint32_t row = range.first_row(); row <= range.last_row(); ++row)
        for (std::uint32_t channel = range.first_channel(); channel <= range.last_channel(); ++channel)
            candidate.patterns[patternIndex].at(row, channel, candidate.channelCount) = {};
    return publish(std::move(candidate), "Clear selection", error);
}

bool ChiptuneAuthoringSession::copy_selection(std::string* error) {
    const auto range = effective_selection();
    const std::uint32_t patternIndex = pattern_for_order(range.anchor.order);
    if (patternIndex >= song_.patterns.size()) {
        if (error) *error = "selection pattern is invalid";
        return false;
    }
    clipboard_.rows = range.last_row() - range.first_row() + 1U;
    clipboard_.channels = range.last_channel() - range.first_channel() + 1U;
    clipboard_.cells.clear();
    clipboard_.cells.reserve(static_cast<std::size_t>(clipboard_.rows) * clipboard_.channels);
    for (std::uint32_t row = range.first_row(); row <= range.last_row(); ++row)
        for (std::uint32_t channel = range.first_channel(); channel <= range.last_channel(); ++channel)
            clipboard_.cells.push_back(song_.patterns[patternIndex].at(row, channel, song_.channelCount));
    return true;
}

bool ChiptuneAuthoringSession::cut_selection(std::string* error) {
    if (!copy_selection(error)) return false;
    return clear_selection(error);
}

bool ChiptuneAuthoringSession::paste_at_cursor(bool mix, std::string* error) {
    if (clipboard_.empty()) {
        if (error) *error = "tracker clipboard is empty";
        return false;
    }
    ChipSong candidate = song_;
    const std::uint32_t patternIndex = pattern_for_order(cursor_.order);
    if (patternIndex >= candidate.patterns.size()) return false;
    const auto rows = candidate.patterns[patternIndex].rowCount;
    for (std::uint32_t row = 0; row < clipboard_.rows && cursor_.row + row < rows; ++row) {
        for (std::uint32_t channel = 0;
             channel < clipboard_.channels && cursor_.channel + channel < candidate.channelCount;
             ++channel) {
            const ChipCell& source = clipboard_.cells[static_cast<std::size_t>(row) * clipboard_.channels + channel];
            if (mix && cell_empty(source)) continue;
            candidate.patterns[patternIndex].at(cursor_.row + row, cursor_.channel + channel,
                                                candidate.channelCount) = source;
        }
    }
    return publish(std::move(candidate), mix ? "Mix-paste tracker data" : "Paste tracker data", error);
}

bool ChiptuneAuthoringSession::add_pattern(std::uint32_t rows,
                                           std::optional<std::uint32_t> duplicate,
                                           std::string* error) {
    if (rows == 0U || rows > 1024U) {
        if (error) *error = "pattern rows must be in [1,1024]";
        return false;
    }
    ChipSong candidate = song_;
    ChipPattern pattern;
    if (duplicate) {
        if (*duplicate >= candidate.patterns.size()) {
            if (error) *error = "source pattern is invalid";
            return false;
        }
        pattern = candidate.patterns[*duplicate];
    } else {
        pattern.rowCount = rows;
        pattern.cells.resize(static_cast<std::size_t>(rows) * candidate.channelCount);
    }
    candidate.patterns.push_back(std::move(pattern));
    return publish(std::move(candidate), duplicate ? "Duplicate pattern" : "Add pattern", error);
}

bool ChiptuneAuthoringSession::remove_pattern(std::uint32_t pattern, std::string* error) {
    if (song_.patterns.size() <= 1U || pattern >= song_.patterns.size()) {
        if (error) *error = "cannot remove the requested pattern";
        return false;
    }
    ChipSong candidate = song_;
    candidate.patterns.erase(candidate.patterns.begin() + static_cast<std::ptrdiff_t>(pattern));
    for (auto& order : candidate.order) {
        if (order == pattern) order = std::min<std::uint32_t>(pattern, static_cast<std::uint32_t>(candidate.patterns.size() - 1U));
        else if (order > pattern) --order;
    }
    return publish(std::move(candidate), "Remove pattern", error);
}

bool ChiptuneAuthoringSession::resize_pattern(std::uint32_t pattern, std::uint32_t rows,
                                              std::string* error) {
    if (pattern >= song_.patterns.size() || rows == 0U || rows > 1024U) {
        if (error) *error = "pattern resize request is invalid";
        return false;
    }
    ChipSong candidate = song_;
    auto& target = candidate.patterns[pattern];
    std::vector<ChipCell> cells(static_cast<std::size_t>(rows) * candidate.channelCount);
    const std::uint32_t copiedRows = std::min(rows, target.rowCount);
    for (std::uint32_t row = 0; row < copiedRows; ++row)
        for (std::uint32_t channel = 0; channel < candidate.channelCount; ++channel)
            cells[static_cast<std::size_t>(row) * candidate.channelCount + channel] =
                target.at(row, channel, candidate.channelCount);
    target.rowCount = rows;
    target.cells = std::move(cells);
    return publish(std::move(candidate), "Resize pattern", error);
}

bool ChiptuneAuthoringSession::insert_order(std::uint32_t index, std::uint32_t pattern,
                                            std::string* error) {
    if (pattern >= song_.patterns.size() || index > song_.order.size()) {
        if (error) *error = "order insertion request is invalid";
        return false;
    }
    ChipSong candidate = song_;
    candidate.order.insert(candidate.order.begin() + static_cast<std::ptrdiff_t>(index), pattern);
    return publish(std::move(candidate), "Insert order entry", error);
}

bool ChiptuneAuthoringSession::delete_order(std::uint32_t index, std::string* error) {
    if (song_.order.size() <= 1U || index >= song_.order.size()) {
        if (error) *error = "cannot delete the requested order entry";
        return false;
    }
    ChipSong candidate = song_;
    candidate.order.erase(candidate.order.begin() + static_cast<std::ptrdiff_t>(index));
    return publish(std::move(candidate), "Delete order entry", error);
}

bool ChiptuneAuthoringSession::set_order_pattern(std::uint32_t index, std::uint32_t pattern,
                                                 std::string* error) {
    if (index >= song_.order.size() || pattern >= song_.patterns.size()) {
        if (error) *error = "order assignment is invalid";
        return false;
    }
    ChipSong candidate = song_;
    candidate.order[index] = pattern;
    return publish(std::move(candidate), "Set order pattern", error);
}

bool ChiptuneAuthoringSession::duplicate_order_pattern(std::uint32_t index, std::string* error) {
    if (index >= song_.order.size()) {
        if (error) *error = "order index is invalid";
        return false;
    }
    ChipSong candidate = song_;
    const std::uint32_t source = candidate.order[index];
    candidate.patterns.push_back(candidate.patterns[source]);
    const auto duplicate = static_cast<std::uint32_t>(candidate.patterns.size() - 1U);
    candidate.order.insert(candidate.order.begin() + static_cast<std::ptrdiff_t>(index + 1U), duplicate);
    return publish(std::move(candidate), "Duplicate order pattern", error);
}

bool ChiptuneAuthoringSession::add_instrument(ChipInstrument instrument, std::string* error) {
    if (song_.instruments.size() >= 255U) {
        if (error) *error = "instrument capacity is exhausted";
        return false;
    }
    if (instrument.name.empty()) instrument = default_instrument();
    ChipSong candidate = song_;
    candidate.instruments.push_back(std::move(instrument));
    if (!publish(std::move(candidate), "Add instrument", error)) return false;
    selectedInstrument_ = song_.instruments.size() - 1U;
    return true;
}

bool ChiptuneAuthoringSession::remove_instrument(std::size_t instrument, std::string* error) {
    if (song_.instruments.size() <= 1U || instrument >= song_.instruments.size()) {
        if (error) *error = "cannot remove the requested instrument";
        return false;
    }
    ChipSong candidate = song_;
    candidate.instruments.erase(candidate.instruments.begin() + static_cast<std::ptrdiff_t>(instrument));
    const std::uint8_t removed = static_cast<std::uint8_t>(instrument + 1U);
    for (auto& pattern : candidate.patterns) {
        for (auto& value : pattern.cells) {
            if (value.instrument == removed) value.instrument = 0U;
            else if (value.instrument > removed) --value.instrument;
        }
    }
    if (!publish(std::move(candidate), "Remove instrument", error)) return false;
    selectedInstrument_ = std::min(selectedInstrument_, song_.instruments.size() - 1U);
    return true;
}

bool ChiptuneAuthoringSession::replace_instrument(std::size_t instrument,
                                                  ChipInstrument replacement,
                                                  std::string* error) {
    if (instrument >= song_.instruments.size()) {
        if (error) *error = "instrument index is invalid";
        return false;
    }
    ChipSong candidate = song_;
    candidate.instruments[instrument] = std::move(replacement);
    return publish(std::move(candidate), "Edit instrument", error);
}

bool ChiptuneAuthoringSession::draw_envelope_point(ChipEnvelopeLane lane, std::size_t index,
                                                   float value, std::string* error) {
    if (index >= kChiptuneEnvelopeMax) {
        if (error) *error = "envelope point exceeds capacity";
        return false;
    }
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument) return false;
    switch (lane) {
        case ChipEnvelopeLane::Volume:
            instrument->volume.values.resize(std::max(instrument->volume.values.size(), index + 1U));
            instrument->volume.values[index] = std::clamp(value, 0.0F, 1.0F);
            break;
        case ChipEnvelopeLane::Release:
            instrument->volume.releaseValues.resize(std::max(instrument->volume.releaseValues.size(), index + 1U));
            instrument->volume.releaseValues[index] = std::clamp(value, 0.0F, 1.0F);
            break;
        case ChipEnvelopeLane::Pitch:
            instrument->pitch.semitones.resize(std::max(instrument->pitch.semitones.size(), index + 1U));
            instrument->pitch.semitones[index] = std::clamp(value, -48.0F, 48.0F);
            break;
        case ChipEnvelopeLane::Duty:
            instrument->dutyEnvelope.values.resize(std::max(instrument->dutyEnvelope.values.size(), index + 1U));
            instrument->dutyEnvelope.values[index] = std::clamp(value, 0.0F, 1.0F);
            break;
        case ChipEnvelopeLane::Arpeggio:
            instrument->arpeggio.semitones.resize(std::max(instrument->arpeggio.semitones.size(), index + 1U));
            instrument->arpeggio.semitones[index] = static_cast<std::int8_t>(std::clamp(std::lround(value), -48L, 48L));
            break;
    }
    return publish(std::move(candidate), "Draw instrument envelope", error);
}

bool ChiptuneAuthoringSession::resize_envelope(ChipEnvelopeLane lane, std::size_t size, float fill,
                                               std::string* error) {
    const std::size_t capacity = lane == ChipEnvelopeLane::Arpeggio ? kChiptuneArpeggioMax : kChiptuneEnvelopeMax;
    if (size > capacity) {
        if (error) *error = "envelope size exceeds capacity";
        return false;
    }
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument) return false;
    switch (lane) {
        case ChipEnvelopeLane::Volume: instrument->volume.values.resize(size, std::clamp(fill, 0.0F, 1.0F)); break;
        case ChipEnvelopeLane::Release: instrument->volume.releaseValues.resize(size, std::clamp(fill, 0.0F, 1.0F)); break;
        case ChipEnvelopeLane::Pitch: instrument->pitch.semitones.resize(size, std::clamp(fill, -48.0F, 48.0F)); break;
        case ChipEnvelopeLane::Duty: instrument->dutyEnvelope.values.resize(size, std::clamp(fill, 0.0F, 1.0F)); break;
        case ChipEnvelopeLane::Arpeggio:
            instrument->arpeggio.semitones.resize(size, static_cast<std::int8_t>(std::clamp(std::lround(fill), -48L, 48L)));
            break;
    }
    return publish(std::move(candidate), "Resize instrument envelope", error);
}

bool ChiptuneAuthoringSession::set_envelope_loop(ChipEnvelopeLane lane, std::size_t loop,
                                                 std::string* error) {
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument) return false;
    switch (lane) {
        case ChipEnvelopeLane::Volume:
            if (loop > instrument->volume.values.size()) return false;
            instrument->volume.loopIndex = loop; break;
        case ChipEnvelopeLane::Release:
            if (loop > instrument->volume.releaseValues.size()) return false;
            instrument->volume.loopIndex = std::min(loop, instrument->volume.values.size()); break;
        case ChipEnvelopeLane::Pitch:
            if (loop > instrument->pitch.semitones.size()) return false;
            instrument->pitch.loopIndex = loop; break;
        case ChipEnvelopeLane::Duty:
            if (loop > instrument->dutyEnvelope.values.size()) return false;
            instrument->dutyEnvelope.loopIndex = loop; break;
        case ChipEnvelopeLane::Arpeggio:
            if (loop > instrument->arpeggio.semitones.size()) return false;
            instrument->arpeggio.loopIndex = loop; break;
    }
    return publish(std::move(candidate), "Set envelope loop", error);
}

bool ChiptuneAuthoringSession::draw_wavetable_point(std::size_t index, float value,
                                                    std::string* error) {
    if (index >= kChiptuneWavetableMax) {
        if (error) *error = "wavetable point exceeds capacity";
        return false;
    }
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument) return false;
    instrument->wave = ChipWave::Wavetable;
    instrument->wavetable.resize(std::max<std::size_t>(2U, std::max(instrument->wavetable.size(), index + 1U)));
    instrument->wavetable[index] = std::clamp(value, -1.0F, 1.0F);
    return publish(std::move(candidate), "Draw wavetable", error);
}

bool ChiptuneAuthoringSession::resize_wavetable(std::size_t size, std::string* error) {
    if (size < 2U || size > kChiptuneWavetableMax) {
        if (error) *error = "wavetable size must be in [2,64]";
        return false;
    }
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument) return false;
    instrument->wave = ChipWave::Wavetable;
    instrument->wavetable.resize(size, 0.0F);
    return publish(std::move(candidate), "Resize wavetable", error);
}

bool ChiptuneAuthoringSession::generate_wavetable(ChipWavetableShape shape, std::size_t size,
                                                  float pulseDuty, std::string* error) {
    if (size < 2U || size > kChiptuneWavetableMax) {
        if (error) *error = "wavetable size must be in [2,64]";
        return false;
    }
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument) return false;
    instrument->wave = ChipWave::Wavetable;
    instrument->wavetable.resize(size);
    pulseDuty = std::clamp(pulseDuty, 0.01F, 0.99F);
    for (std::size_t i = 0; i < size; ++i) {
        const float phase = static_cast<float>(i) / static_cast<float>(size);
        float value{};
        switch (shape) {
            case ChipWavetableShape::Sine: value = std::sin(2.0F * std::numbers::pi_v<float> * phase); break;
            case ChipWavetableShape::Triangle: value = 1.0F - 4.0F * std::abs(phase - 0.5F); break;
            case ChipWavetableShape::Sawtooth: value = phase * 2.0F - 1.0F; break;
            case ChipWavetableShape::Pulse: value = phase < pulseDuty ? 1.0F : -1.0F; break;
            case ChipWavetableShape::Silence: value = 0.0F; break;
        }
        instrument->wavetable[i] = value;
    }
    return publish(std::move(candidate), "Generate wavetable", error);
}

bool ChiptuneAuthoringSession::normalize_wavetable(std::string* error) {
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument || instrument->wavetable.empty()) return false;
    float peak{};
    for (float value : instrument->wavetable) peak = std::max(peak, std::abs(value));
    if (peak > 1.0e-6F) for (float& value : instrument->wavetable) value /= peak;
    return publish(std::move(candidate), "Normalize wavetable", error);
}

bool ChiptuneAuthoringSession::remove_wavetable_dc(std::string* error) {
    ChipSong candidate = song_;
    ChipInstrument* instrument = selected_instrument_ptr(candidate);
    if (!instrument || instrument->wavetable.empty()) return false;
    float mean{};
    for (float value : instrument->wavetable) mean += value;
    mean /= static_cast<float>(instrument->wavetable.size());
    for (float& value : instrument->wavetable) value = std::clamp(value - mean, -1.0F, 1.0F);
    return publish(std::move(candidate), "Remove wavetable DC", error);
}

void ChiptuneAuthoringSession::set_sfx_request(ChipSfxRequest request) noexcept {
    request.sampleRate = std::clamp(request.sampleRate, 8000U, 192000U);
    request.durationSeconds = std::clamp(request.durationSeconds, 0.05F, 2.0F);
    request.baseMidi = std::clamp(request.baseMidi, 12, 119);
    request.gain = std::clamp(request.gain, 0.0F, 2.0F);
    request.pan = std::clamp(request.pan, -1.0F, 1.0F);
    sfxRequest_ = request;
}

bool ChiptuneAuthoringSession::apply_sfx_request(std::string* error) {
    return publish(make_chiptune_sfx_song(sfxRequest_), "Apply SFX preset", error);
}

DecodedAudioAsset ChiptuneAuthoringSession::render_song_preview(float seconds,
                                                                std::string* error) const {
    return render_chiptune_audio_asset(song_, std::clamp(seconds, 0.05F, 600.0F), error);
}

DecodedAudioAsset ChiptuneAuthoringSession::render_instrument_preview(int midiNote, float seconds,
                                                                      std::string* error) const {
    const ChipSong preview = make_instrument_preview_song(song_, selected_instrument_ptr(), midiNote);
    return render_chiptune_audio_asset(preview, std::clamp(seconds, 0.05F, 10.0F), error);
}

ChiptuneAuditionResult ChiptuneAuthoringSession::audition_song(AudioMixer& mixer, float seconds,
                                                               bool loop) {
    ChiptuneAuditionResult result;
    result.bus = songBus_;
    result.contentHash = song_.content_hash();
    (void)seconds;
    result.livePreview = mixer.start_chiptune_preview(song_, result.bus, loop, &result.error);
    return result;
}

ChiptuneAuditionResult ChiptuneAuthoringSession::audition_instrument(AudioMixer& mixer, int midiNote,
                                                                     float seconds) {
    ChiptuneAuditionResult result;
    result.bus = instrumentBus_;
    const ChipSong preview = make_instrument_preview_song(song_, selected_instrument_ptr(), midiNote);
    result.contentHash = preview.content_hash();
    (void)seconds;
    result.livePreview = mixer.start_chiptune_preview(preview, result.bus, false, &result.error);
    return result;
}

void ChiptuneAuthoringSession::stop_audition(AudioMixer& mixer, float fadeSeconds) noexcept {
    mixer.stop_chiptune_preview();
    if (auditionSource_) (void)mixer.stop(auditionSource_, fadeSeconds);
    auditionSource_ = {};
}

bool ChiptuneAuthoringSession::undo(std::string* error) {
    if (undo_.empty()) {
        if (error) *error = "nothing to undo";
        return false;
    }
    Snapshot target = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(snapshot(target.label));
    restore(target);
    dirty_ = true;
    return true;
}

bool ChiptuneAuthoringSession::redo(std::string* error) {
    if (redo_.empty()) {
        if (error) *error = "nothing to redo";
        return false;
    }
    Snapshot target = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(snapshot(target.label));
    restore(target);
    dirty_ = true;
    return true;
}

int ChiptuneAuthoringSession::tracker_key_to_midi(std::string_view key, int octave) noexcept {
    const std::string normalized = lower(key);
    struct Mapping { std::string_view key; int semitone; int octaveOffset; };
    static constexpr Mapping mappings[] = {
        {"z",0,0},{"s",1,0},{"x",2,0},{"d",3,0},{"c",4,0},{"v",5,0},
        {"g",6,0},{"b",7,0},{"h",8,0},{"n",9,0},{"j",10,0},{"m",11,0},
        {"q",0,1},{"2",1,1},{"w",2,1},{"3",3,1},{"e",4,1},{"r",5,1},
        {"5",6,1},{"t",7,1},{"6",8,1},{"y",9,1},{"7",10,1},{"u",11,1},
        {"i",0,2},{"9",1,2},{"o",2,2},{"0",3,2},{"p",4,2}
    };
    for (const auto& mapping : mappings) {
        if (normalized == mapping.key) {
            const int midi = (std::clamp(octave, 0, 8) + 1 + mapping.octaveOffset) * 12 + mapping.semitone;
            return midi <= 127 ? midi : -1;
        }
    }
    return -1;
}

std::uint8_t ChiptuneAuthoringSession::encode_effect_parameter(ChipEffect effect,
                                                               ChipEffectHelperInput input) noexcept {
    switch (effect) {
        case ChipEffect::Arpeggio:
        case ChipEffect::Vibrato:
        case ChipEffect::VolumeSlide:
            return static_cast<std::uint8_t>((std::clamp(input.primary, 0, 15) << 4) |
                                             std::clamp(input.secondary, 0, 15));
        case ChipEffect::SetPan: {
            const float normalized = std::clamp(static_cast<float>(input.primary) / 100.0F, -1.0F, 1.0F);
            return static_cast<std::uint8_t>(std::lround((normalized + 1.0F) * 127.5F));
        }
        case ChipEffect::SetDuty: {
            const float duty = std::clamp(static_cast<float>(input.primary) / 100.0F, 0.0F, 1.0F);
            return static_cast<std::uint8_t>(std::lround(duty * 255.0F));
        }
        case ChipEffect::SetWave: return static_cast<std::uint8_t>(std::clamp(input.primary, 0, 5));
        case ChipEffect::NoEffect: return 0U;
        default: return static_cast<std::uint8_t>(std::clamp(input.primary, 0, 255));
    }
}

ChiptuneAuthoringSession::Snapshot ChiptuneAuthoringSession::snapshot(std::string label) const {
    Snapshot result;
    result.song = song_;
    result.cursor = cursor_;
    result.selection = selection_;
    result.selectedInstrument = selectedInstrument_;
    result.octave = octave_;
    result.sfxRequest = sfxRequest_;
    result.songBus = songBus_;
    result.instrumentBus = instrumentBus_;
    result.label = std::move(label);
    return result;
}

void ChiptuneAuthoringSession::restore(const Snapshot& state) {
    song_ = state.song;
    cursor_ = state.cursor;
    selection_ = state.selection;
    selectedInstrument_ = state.selectedInstrument;
    octave_ = state.octave;
    sfxRequest_ = state.sfxRequest;
    songBus_ = state.songBus;
    instrumentBus_ = state.instrumentBus;
    (void)clamp_position(cursor_);
    selectedInstrument_ = song_.instruments.empty() ? 0U : std::min(selectedInstrument_, song_.instruments.size() - 1U);
}

bool ChiptuneAuthoringSession::publish(ChipSong candidate, std::string label, std::string* error) {
    std::string validation;
    if (!candidate.validate(&validation)) {
        if (error) *error = validation;
        return false;
    }
    if (candidate.serialize() == song_.serialize()) return true;
    if (undo_.size() >= kChiptuneAuthoringHistoryLimit) undo_.erase(undo_.begin());
    undo_.push_back(snapshot(std::move(label)));
    redo_.clear();
    song_ = std::move(candidate);
    (void)clamp_position(cursor_);
    selectedInstrument_ = song_.instruments.empty() ? 0U : std::min(selectedInstrument_, song_.instruments.size() - 1U);
    dirty_ = true;
    return true;
}

bool ChiptuneAuthoringSession::clamp_position(ChipTrackerPosition& position) const noexcept {
    if (song_.order.empty() || song_.patterns.empty() || song_.channelCount == 0U) return false;
    position.order = std::min<std::uint32_t>(position.order, static_cast<std::uint32_t>(song_.order.size() - 1U));
    const std::uint32_t patternIndex = pattern_for_order(position.order);
    if (patternIndex >= song_.patterns.size() || song_.patterns[patternIndex].rowCount == 0U) return false;
    position.row = std::min(position.row, song_.patterns[patternIndex].rowCount - 1U);
    position.channel = std::min(position.channel, song_.channelCount - 1U);
    return true;
}

std::uint32_t ChiptuneAuthoringSession::pattern_for_order(std::uint32_t order) const noexcept {
    if (song_.order.empty()) return 0U;
    return song_.order[std::min<std::uint32_t>(order, static_cast<std::uint32_t>(song_.order.size() - 1U))];
}

ChipCell* ChiptuneAuthoringSession::mutable_cell(ChipSong& song, ChipTrackerPosition position) noexcept {
    if (song.order.empty() || position.order >= song.order.size() || position.channel >= song.channelCount) return nullptr;
    const std::uint32_t patternIndex = song.order[position.order];
    if (patternIndex >= song.patterns.size() || position.row >= song.patterns[patternIndex].rowCount) return nullptr;
    return &song.patterns[patternIndex].at(position.row, position.channel, song.channelCount);
}

const ChipCell* ChiptuneAuthoringSession::cell(ChipTrackerPosition position) const noexcept {
    if (song_.order.empty() || position.order >= song_.order.size() || position.channel >= song_.channelCount) return nullptr;
    const std::uint32_t patternIndex = song_.order[position.order];
    if (patternIndex >= song_.patterns.size() || position.row >= song_.patterns[patternIndex].rowCount) return nullptr;
    return &song_.patterns[patternIndex].at(position.row, position.channel, song_.channelCount);
}

ChipTrackerSelection ChiptuneAuthoringSession::effective_selection() const noexcept {
    if (selection_) return *selection_;
    return {cursor_, cursor_};
}

ChipInstrument* ChiptuneAuthoringSession::selected_instrument_ptr(ChipSong& song) noexcept {
    if (song.instruments.empty()) return nullptr;
    return &song.instruments[std::min(selectedInstrument_, song.instruments.size() - 1U)];
}

const ChipInstrument* ChiptuneAuthoringSession::selected_instrument_ptr() const noexcept {
    if (song_.instruments.empty()) return nullptr;
    return &song_.instruments[std::min(selectedInstrument_, song_.instruments.size() - 1U)];
}

} // namespace dve::audio
