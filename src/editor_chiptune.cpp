#include "dve/editor_chiptune.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

namespace dve::editor {
namespace {

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

[[nodiscard]] audio::ChipCell current_cell(const audio::ChiptuneAuthoringSession& session) {
    const auto& song = session.song();
    const auto position = session.cursor();
    if (position.order >= song.order.size() || position.channel >= song.channelCount) return {};
    const auto pattern = song.order[position.order];
    if (pattern >= song.patterns.size() || position.row >= song.patterns[pattern].rowCount) return {};
    return song.patterns[pattern].at(position.row, position.channel, song.channelCount);
}

[[nodiscard]] audio::ChipWave next_wave(audio::ChipWave wave, int direction) noexcept {
    int index = static_cast<int>(wave) + direction;
    while (index < 0) index += 6;
    return static_cast<audio::ChipWave>(index % 6);
}

} // namespace

std::string chip_note_display(std::uint8_t note) {
    if (note == audio::kChipNoteNone) return "---";
    if (note == audio::kChipNoteOff) return "OFF";
    static constexpr std::array<std::string_view, 12> names{
        "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    const int midi = audio::chip_note_to_midi(note);
    if (midi < 0) return "---";
    return std::string(names[static_cast<std::size_t>(midi % 12)]) + std::to_string(midi / 12 - 1);
}

std::string chip_cell_display(const audio::ChipCell& cell) {
    char buffer[48];
    const auto note = chip_note_display(cell.note);
    const unsigned instrument = cell.instrument;
    const unsigned effect = static_cast<unsigned>(cell.effect);
    const unsigned param = cell.effectParam;
    if (cell.volume == audio::kChipVolumeNone) {
        std::snprintf(buffer, sizeof(buffer), "%s %02X -- %X%02X",
                      note.c_str(), instrument, effect, param);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s %02X %02X %X%02X",
                      note.c_str(), instrument, static_cast<unsigned>(cell.volume), effect, param);
    }
    return buffer;
}

EditorChiptunePanel::EditorChiptunePanel() = default;

void EditorChiptunePanel::set_open(bool value, audio::AudioMixer& mixer) noexcept {
    if (open_ && !value) session_.stop_audition(mixer);
    open_ = value;
}

void EditorChiptunePanel::set_document_path(std::filesystem::path path) noexcept {
    try { documentPath_ = std::move(path); }
    catch (...) { documentPath_ = "assets/chiptune/editor_song.dvechip"; }
}

void EditorChiptunePanel::resize(int width, int height, float uiScale) noexcept {
    const int panelWidth = std::clamp(static_cast<int>(1040.0F * uiScale), 720, std::max(720, width - 24));
    const int panelHeight = std::clamp(static_cast<int>(690.0F * uiScale), 520, std::max(520, height - 52));
    const int x = std::max(12, (width - panelWidth) / 2);
    const int y = std::max(34, (height - panelHeight) / 2);
    layout_.panel = {x, y, panelWidth, panelHeight};
    layout_.titleBar = {x, y, panelWidth, 34};
    layout_.closeButton = {x + panelWidth - 31, y + 5, 24, 24};
    for (std::size_t i = 0; i < layout_.tabs.size(); ++i)
        layout_.tabs[i] = {x + 12 + static_cast<int>(i) * 112, y + 42, 104, 25};
    int toolbarX = x + 358;
    auto toolbar = [&](UiRect& rect, int w) { rect = {toolbarX, y + 42, w, 25}; toolbarX += w + 5; };
    toolbar(layout_.playSongButton, 68); toolbar(layout_.playInstrumentButton, 68);
    toolbar(layout_.stopButton, 48); toolbar(layout_.saveButton, 48); toolbar(layout_.openButton, 48);
    toolbar(layout_.undoButton, 48); toolbar(layout_.redoButton, 48);
    toolbar(layout_.copyButton, 44); toolbar(layout_.cutButton, 38); toolbar(layout_.pasteButton, 48);
    layout_.songBusButton = {x + 12, y + panelHeight - 66, 190, 25};
    layout_.instrumentBusButton = {x + 208, y + panelHeight - 66, 190, 25};

    const int bodyY = y + 76;
    const int bodyH = panelHeight - 154;
    const int orderW = 150;
    layout_.orderList = {x + 12, bodyY, orderW, bodyH};
    const int orderRowH = std::max(23, bodyH / static_cast<int>(kChiptuneVisibleOrders + 3U));
    for (std::size_t i = 0; i < kChiptuneVisibleOrders; ++i)
        layout_.orderRows[i] = {layout_.orderList.x + 5, bodyY + 5 + static_cast<int>(i) * orderRowH,
                                orderW - 10, orderRowH - 2};
    int orderButtonY = bodyY + 7 + static_cast<int>(kChiptuneVisibleOrders) * orderRowH;
    layout_.addOrderButton = {x + 17, orderButtonY, 42, 24};
    layout_.deleteOrderButton = {x + 63, orderButtonY, 42, 24};
    layout_.duplicatePatternButton = {x + 109, orderButtonY, 42, 24};
    orderButtonY += 29;
    layout_.addPatternButton = {x + 17, orderButtonY, 65, 24};
    layout_.deletePatternButton = {x + 86, orderButtonY, 65, 24};

    layout_.patternGrid = {x + orderW + 22, bodyY, panelWidth - orderW - 34, bodyH};
    const int rowH = std::max(22, bodyH / static_cast<int>(kChiptuneVisibleRows));
    const int cellW = std::max(78, layout_.patternGrid.width / static_cast<int>(audio::kChiptuneMaxChannels));
    for (std::size_t row = 0; row < kChiptuneVisibleRows; ++row)
        for (std::size_t channel = 0; channel < audio::kChiptuneMaxChannels; ++channel)
            layout_.cells[row][channel] = {layout_.patternGrid.x + static_cast<int>(channel) * cellW,
                                           layout_.patternGrid.y + static_cast<int>(row) * rowH,
                                           cellW - 2, rowH - 2};
    layout_.effectPreviousButton = {layout_.patternGrid.x, y + panelHeight - 99, 28, 24};
    layout_.effectNextButton = {layout_.patternGrid.x + 32, y + panelHeight - 99, 28, 24};
    layout_.effectParamDownButton = {layout_.patternGrid.x + 70, y + panelHeight - 99, 28, 24};
    layout_.effectParamUpButton = {layout_.patternGrid.x + 102, y + panelHeight - 99, 28, 24};

    layout_.instrumentPreviousButton = {x + 20, bodyY + 10, 30, 25};
    layout_.instrumentNextButton = {x + 54, bodyY + 10, 30, 25};
    layout_.addInstrumentButton = {x + 92, bodyY + 10, 72, 25};
    layout_.deleteInstrumentButton = {x + 168, bodyY + 10, 72, 25};
    layout_.wavePreviousButton = {x + 252, bodyY + 10, 30, 25};
    layout_.waveNextButton = {x + 286, bodyY + 10, 30, 25};
    layout_.envelopeCanvas = {x + 20, bodyY + 50, panelWidth - 40, std::max(130, bodyH / 2 - 35)};
    layout_.wavetableCanvas = {x + 20, layout_.envelopeCanvas.y + layout_.envelopeCanvas.height + 38,
                               panelWidth - 250, std::max(105, bodyH / 2 - 68)};
    layout_.normalizeWavetableButton = {x + panelWidth - 218, layout_.wavetableCanvas.y, 94, 25};
    layout_.removeDcButton = {x + panelWidth - 118, layout_.wavetableCanvas.y, 94, 25};
    for (std::size_t i = 0; i < layout_.wavetableShapeButtons.size(); ++i)
        layout_.wavetableShapeButtons[i] = {x + panelWidth - 218 + static_cast<int>(i % 2U) * 100,
                                            layout_.wavetableCanvas.y + 35 + static_cast<int>(i / 2U) * 29,
                                            94, 25};

    for (std::size_t i = 0; i < layout_.sfxPresetButtons.size(); ++i)
        layout_.sfxPresetButtons[i] = {x + 25 + static_cast<int>(i % 4U) * 155,
                                       bodyY + 20 + static_cast<int>(i / 4U) * 38, 145, 30};
    const int sfxY = bodyY + 120;
    layout_.sfxBaseDownButton = {x + 25, sfxY, 30, 25}; layout_.sfxBaseUpButton = {x + 145, sfxY, 30, 25};
    layout_.sfxDurationDownButton = {x + 210, sfxY, 30, 25}; layout_.sfxDurationUpButton = {x + 330, sfxY, 30, 25};
    layout_.sfxGainDownButton = {x + 395, sfxY, 30, 25}; layout_.sfxGainUpButton = {x + 515, sfxY, 30, 25};
    layout_.sfxPanDownButton = {x + 580, sfxY, 30, 25}; layout_.sfxPanUpButton = {x + 700, sfxY, 30, 25};
    layout_.applySfxButton = {x + 25, sfxY + 45, 145, 30};
    layout_.auditionSfxButton = {x + 180, sfxY + 45, 145, 30};
    const int pianoY = bodyY + bodyH - 115;
    const int pianoW = std::max(20, (panelWidth - 50) / 24);
    for (std::size_t i = 0; i < layout_.pianoKeys.size(); ++i)
        layout_.pianoKeys[i] = {x + 25 + static_cast<int>(i) * pianoW, pianoY, pianoW - 1, 92};
}

bool EditorChiptunePanel::pointer_down(int x, int y, audio::AudioMixer& mixer) noexcept {
    if (!open_ || !layout_.panel.contains(x, y)) return false;
    if (layout_.closeButton.contains(x, y)) { set_open(false, mixer); return true; }
    for (std::size_t i = 0; i < layout_.tabs.size(); ++i) if (layout_.tabs[i].contains(x, y)) {
        page_ = static_cast<ChiptunePanelPage>(i); return true;
    }
    if (layout_.playSongButton.contains(x, y)) { play_song(mixer); return true; }
    if (layout_.playInstrumentButton.contains(x, y)) { play_instrument(mixer); return true; }
    if (layout_.stopButton.contains(x, y)) { session_.stop_audition(mixer); set_status("Preview stopped"); return true; }
    if (layout_.saveButton.contains(x, y)) { save(); return true; }
    if (layout_.openButton.contains(x, y)) { load(); return true; }
    std::string error;
    if (layout_.undoButton.contains(x, y)) { set_status(session_.undo(&error) ? "Undo" : error); return true; }
    if (layout_.redoButton.contains(x, y)) { set_status(session_.redo(&error) ? "Redo" : error); return true; }
    if (layout_.copyButton.contains(x, y)) { set_status(session_.copy_selection(&error) ? "Copied tracker selection" : error); return true; }
    if (layout_.cutButton.contains(x, y)) { set_status(session_.cut_selection(&error) ? "Cut tracker selection" : error); return true; }
    if (layout_.pasteButton.contains(x, y)) { set_status(session_.paste_at_cursor(false, &error) ? "Pasted tracker selection" : error); return true; }
    if (layout_.songBusButton.contains(x, y)) { cycle_bus(true, 1); return true; }
    if (layout_.instrumentBusButton.contains(x, y)) { cycle_bus(false, 1); return true; }

    if (page_ == ChiptunePanelPage::Pattern) {
        const auto& song = session_.song();
        for (std::size_t i = 0; i < layout_.orderRows.size(); ++i) if (layout_.orderRows[i].contains(x, y)) {
            const std::uint32_t order = firstVisibleOrder_ + static_cast<std::uint32_t>(i);
            if (order < song.order.size()) {
                auto cursor = session_.cursor(); cursor.order = order; cursor.row = 0U;
                (void)session_.set_cursor(cursor); keep_cursor_visible();
            }
            return true;
        }
        if (layout_.addOrderButton.contains(x, y)) {
            const auto cursor = session_.cursor();
            set_status(session_.insert_order(cursor.order + 1U, song.order[cursor.order], &error) ? "Order entry inserted" : error); return true;
        }
        if (layout_.deleteOrderButton.contains(x, y)) {
            set_status(session_.delete_order(session_.cursor().order, &error) ? "Order entry deleted" : error); keep_cursor_visible(); return true;
        }
        if (layout_.duplicatePatternButton.contains(x, y)) {
            set_status(session_.duplicate_order_pattern(session_.cursor().order, &error) ? "Pattern duplicated in order" : error); return true;
        }
        if (layout_.addPatternButton.contains(x, y)) {
            set_status(session_.add_pattern(64U, std::nullopt, &error) ? "Pattern added" : error); return true;
        }
        if (layout_.deletePatternButton.contains(x, y)) {
            const auto cursor = session_.cursor(); const auto pattern = song.order[cursor.order];
            set_status(session_.remove_pattern(pattern, &error) ? "Pattern deleted" : error); return true;
        }
        for (std::size_t row = 0; row < kChiptuneVisibleRows; ++row) {
            for (std::size_t channel = 0; channel < audio::kChiptuneMaxChannels; ++channel) {
                if (!layout_.cells[row][channel].contains(x, y)) continue;
                auto cursor = session_.cursor();
                cursor.row = firstVisibleRow_ + static_cast<std::uint32_t>(row);
                cursor.channel = static_cast<std::uint32_t>(channel);
                (void)session_.set_cursor(cursor);
                session_.collapse_selection();
                return true;
            }
        }
        if (layout_.effectPreviousButton.contains(x, y)) { cycle_effect(-1); return true; }
        if (layout_.effectNextButton.contains(x, y)) { cycle_effect(1); return true; }
        if (layout_.effectParamDownButton.contains(x, y)) { adjust_effect_parameter(-1); return true; }
        if (layout_.effectParamUpButton.contains(x, y)) { adjust_effect_parameter(1); return true; }
    } else if (page_ == ChiptunePanelPage::Instrument) {
        if (layout_.instrumentPreviousButton.contains(x, y)) { cycle_instrument(-1); return true; }
        if (layout_.instrumentNextButton.contains(x, y)) { cycle_instrument(1); return true; }
        if (layout_.addInstrumentButton.contains(x, y)) { set_status(session_.add_instrument({}, &error) ? "Instrument added" : error); return true; }
        if (layout_.deleteInstrumentButton.contains(x, y)) { set_status(session_.remove_instrument(session_.selected_instrument(), &error) ? "Instrument deleted" : error); return true; }
        if (layout_.wavePreviousButton.contains(x, y)) { cycle_wave(-1); return true; }
        if (layout_.waveNextButton.contains(x, y)) { cycle_wave(1); return true; }
        if (layout_.envelopeCanvas.contains(x, y)) { envelopeDrawing_ = true; draw_envelope(x, y); return true; }
        if (layout_.wavetableCanvas.contains(x, y)) { wavetableDrawing_ = true; draw_wavetable(x, y); return true; }
        if (layout_.normalizeWavetableButton.contains(x, y)) { set_status(session_.normalize_wavetable(&error) ? "Wavetable normalized" : error); return true; }
        if (layout_.removeDcButton.contains(x, y)) { set_status(session_.remove_wavetable_dc(&error) ? "Wavetable DC removed" : error); return true; }
        for (std::size_t i = 0; i < layout_.wavetableShapeButtons.size(); ++i) if (layout_.wavetableShapeButtons[i].contains(x, y)) {
            set_status(session_.generate_wavetable(static_cast<audio::ChipWavetableShape>(i), 32U, 0.5F, &error) ? "Wavetable generated" : error); return true;
        }
    } else {
        auto request = session_.sfx_request();
        for (std::size_t i = 0; i < layout_.sfxPresetButtons.size(); ++i) if (layout_.sfxPresetButtons[i].contains(x, y)) {
            request.preset = static_cast<audio::ChipSfxPreset>(i); session_.set_sfx_request(request);
            set_status("SFX preset selected"); return true;
        }
        auto adjust = [&](UiRect down, UiRect up, auto&& fn) {
            if (down.contains(x,y)) { fn(-1); return true; }
            if (up.contains(x,y)) { fn(1); return true; }
            return false;
        };
        if (adjust(layout_.sfxBaseDownButton, layout_.sfxBaseUpButton, [&](int d){ request.baseMidi += d; session_.set_sfx_request(request); })) return true;
        if (adjust(layout_.sfxDurationDownButton, layout_.sfxDurationUpButton, [&](int d){ request.durationSeconds += 0.05F * static_cast<float>(d); session_.set_sfx_request(request); })) return true;
        if (adjust(layout_.sfxGainDownButton, layout_.sfxGainUpButton, [&](int d){ request.gain += 0.05F * static_cast<float>(d); session_.set_sfx_request(request); })) return true;
        if (adjust(layout_.sfxPanDownButton, layout_.sfxPanUpButton, [&](int d){ request.pan += 0.1F * static_cast<float>(d); session_.set_sfx_request(request); })) return true;
        if (layout_.applySfxButton.contains(x, y)) { set_status(session_.apply_sfx_request(&error) ? "SFX preset applied to document" : error); return true; }
        if (layout_.auditionSfxButton.contains(x, y)) { session_.set_song_bus(audio::AudioBusId::Effects); play_song(mixer); return true; }
        for (std::size_t i = 0; i < layout_.pianoKeys.size(); ++i) if (layout_.pianoKeys[i].contains(x, y)) {
            request.baseMidi = (session_.octave() + 1) * 12 + static_cast<int>(i);
            session_.set_sfx_request(request);
            if (session_.apply_sfx_request(&error)) {
                session_.set_song_bus(audio::AudioBusId::Effects);
                play_song(mixer);
                set_status("Piano note entered and SFX auditioned");
            } else set_status(error);
            return true;
        }
    }
    return true;
}

bool EditorChiptunePanel::pointer_move(int x, int y) noexcept {
    if (!open_) return false;
    if (envelopeDrawing_) { draw_envelope(x, y); return true; }
    if (wavetableDrawing_) { draw_wavetable(x, y); return true; }
    return layout_.panel.contains(x, y);
}

bool EditorChiptunePanel::pointer_up(int, int) noexcept {
    const bool handled = envelopeDrawing_ || wavetableDrawing_;
    envelopeDrawing_ = false; wavetableDrawing_ = false;
    return handled;
}

bool EditorChiptunePanel::key_down(std::string_view key, bool control, bool shift, bool alt,
                                   audio::AudioMixer& mixer) noexcept {
    if (!open_ || alt) return false;
    const auto normalized = lower(key);
    std::string error;
    if (control) {
        if (normalized == "z") { set_status((shift ? session_.redo(&error) : session_.undo(&error)) ? (shift ? "Redo" : "Undo") : error); return true; }
        if (normalized == "y") { set_status(session_.redo(&error) ? "Redo" : error); return true; }
        if (normalized == "c") { set_status(session_.copy_selection(&error) ? "Copied tracker selection" : error); return true; }
        if (normalized == "x") { set_status(session_.cut_selection(&error) ? "Cut tracker selection" : error); return true; }
        if (normalized == "v") { set_status(session_.paste_at_cursor(shift, &error) ? "Pasted tracker selection" : error); return true; }
        if (normalized == "a") { session_.select_all(); set_status("Selected current pattern"); return true; }
        if (normalized == "s") { save(); return true; }
        if (normalized == "o") { load(); return true; }
        if (normalized == "space") { play_song(mixer); return true; }
        if (normalized == "up") { set_status(session_.transpose_selection(shift ? 12 : 1, &error) ? "Selection transposed up" : error); return true; }
        if (normalized == "down") { set_status(session_.transpose_selection(shift ? -12 : -1, &error) ? "Selection transposed down" : error); return true; }
        if (normalized == "d") { set_status(session_.duplicate_order_pattern(session_.cursor().order, &error) ? "Pattern duplicated" : error); return true; }
        return false;
    }
    auto cursor = session_.cursor();
    if (normalized == "escape") { session_.collapse_selection(); return true; }
    if (normalized == "space") { play_song(mixer); return true; }
    if (normalized == "delete") { set_status(session_.clear_selection(&error) ? "Tracker data cleared" : error); return true; }
    if (normalized == "backspace") { set_status(session_.enter_midi_note(0, true, 1U, &error) ? "Note off entered" : error); return true; }
    const auto move_cursor = [&](audio::ChipTrackerPosition target) {
        const auto oldCursor = session_.cursor();
        audio::ChipTrackerPosition anchor = oldCursor;
        if (session_.selection()) anchor = session_.selection()->anchor;
        if (!session_.set_cursor(target)) return;
        if (shift) session_.set_selection(audio::ChipTrackerSelection{anchor, session_.cursor()});
        else session_.collapse_selection();
        keep_cursor_visible();
    };
    if (normalized == "left") { if (cursor.channel > 0U) --cursor.channel; move_cursor(cursor); return true; }
    if (normalized == "right") { ++cursor.channel; move_cursor(cursor); return true; }
    if (normalized == "up") { if (cursor.row > 0U) --cursor.row; move_cursor(cursor); return true; }
    if (normalized == "down") { ++cursor.row; move_cursor(cursor); return true; }
    if (normalized == "pageup") { if (cursor.order > 0U) --cursor.order; cursor.row = 0U; move_cursor(cursor); return true; }
    if (normalized == "pagedown") { ++cursor.order; cursor.row = 0U; move_cursor(cursor); return true; }
    if (normalized == "[") { session_.set_octave(session_.octave() - 1); return true; }
    if (normalized == "]") { session_.set_octave(session_.octave() + 1); return true; }
    if (normalized == "f1") { page_ = ChiptunePanelPage::Pattern; return true; }
    if (normalized == "f2") { page_ = ChiptunePanelPage::Instrument; return true; }
    if (normalized == "f3") { page_ = ChiptunePanelPage::Sfx; return true; }
    if (page_ == ChiptunePanelPage::Pattern && session_.enter_tracker_key(normalized, false, 1U, &error)) {
        set_status("Note entered"); keep_cursor_visible(); return true;
    }
    return false;
}

void EditorChiptunePanel::keep_cursor_visible() noexcept {
    const auto cursor = session_.cursor();
    if (cursor.row < firstVisibleRow_) firstVisibleRow_ = cursor.row;
    else if (cursor.row >= firstVisibleRow_ + kChiptuneVisibleRows)
        firstVisibleRow_ = cursor.row - static_cast<std::uint32_t>(kChiptuneVisibleRows) + 1U;
    if (cursor.order < firstVisibleOrder_) firstVisibleOrder_ = cursor.order;
    else if (cursor.order >= firstVisibleOrder_ + kChiptuneVisibleOrders)
        firstVisibleOrder_ = cursor.order - static_cast<std::uint32_t>(kChiptuneVisibleOrders) + 1U;
}

void EditorChiptunePanel::cycle_bus(bool songBus, int direction) noexcept {
    auto bus = songBus ? session_.song_bus() : session_.instrument_bus();
    int value = static_cast<int>(bus) + direction;
    while (value < 0) value += static_cast<int>(audio::kAudioBusCount);
    bus = static_cast<audio::AudioBusId>(value % static_cast<int>(audio::kAudioBusCount));
    if (songBus) session_.set_song_bus(bus); else session_.set_instrument_bus(bus);
    set_status(std::string(songBus ? "Song bus: " : "Instrument bus: ") + std::string(audio::audio_bus_name(bus)));
}

void EditorChiptunePanel::cycle_effect(int direction) noexcept {
    auto cell = current_cell(session_);
    int effect = static_cast<int>(cell.effect) + direction;
    while (effect < 0) effect += 15;
    effect %= 15;
    std::string error;
    set_status(session_.set_current_effect(static_cast<audio::ChipEffect>(effect), {cell.effectParam, 0}, &error)
        ? std::string("Effect: ") + std::string(audio::chip_effect_name(static_cast<audio::ChipEffect>(effect))) : error);
}

void EditorChiptunePanel::adjust_effect_parameter(int direction) noexcept {
    auto cell = current_cell(session_);
    const int parameter = std::clamp(static_cast<int>(cell.effectParam) + direction, 0, 255);
    std::string error;
    set_status(session_.set_current_effect(cell.effect, {parameter, 0}, &error) ? "Effect parameter changed" : error);
}

void EditorChiptunePanel::cycle_instrument(int direction) noexcept {
    const auto count = session_.song().instruments.size();
    if (count == 0U) return;
    int index = static_cast<int>(session_.selected_instrument()) + direction;
    while (index < 0) index += static_cast<int>(count);
    session_.set_selected_instrument(static_cast<std::size_t>(index) % count);
    set_status("Instrument selected");
}

void EditorChiptunePanel::cycle_wave(int direction) noexcept {
    const auto index = session_.selected_instrument();
    if (index >= session_.song().instruments.size()) return;
    auto instrument = session_.song().instruments[index];
    instrument.wave = next_wave(instrument.wave, direction);
    if (instrument.wave == audio::ChipWave::Wavetable && instrument.wavetable.size() < 2U)
        instrument.wavetable = {0.0F, 1.0F, 0.0F, -1.0F};
    std::string error;
    set_status(session_.replace_instrument(index, std::move(instrument), &error) ? "Instrument waveform changed" : error);
}

void EditorChiptunePanel::draw_envelope(int x, int y) noexcept {
    const auto rect = layout_.envelopeCanvas;
    const float localX = std::clamp(static_cast<float>(x - rect.x) / std::max(1.0F, static_cast<float>(rect.width)), 0.0F, 0.9999F);
    const float localY = std::clamp(static_cast<float>(y - rect.y) / std::max(1.0F, static_cast<float>(rect.height)), 0.0F, 1.0F);
    const std::size_t index = std::min<std::size_t>(audio::kChiptuneEnvelopeMax - 1U,
        static_cast<std::size_t>(localX * static_cast<float>(audio::kChiptuneEnvelopeMax)));
    std::string error;
    if (!session_.draw_envelope_point(audio::ChipEnvelopeLane::Volume, index, 1.0F - localY, &error)) set_status(error);
}

void EditorChiptunePanel::draw_wavetable(int x, int y) noexcept {
    const auto rect = layout_.wavetableCanvas;
    const float localX = std::clamp(static_cast<float>(x - rect.x) / std::max(1.0F, static_cast<float>(rect.width)), 0.0F, 0.9999F);
    const float localY = std::clamp(static_cast<float>(y - rect.y) / std::max(1.0F, static_cast<float>(rect.height)), 0.0F, 1.0F);
    const std::size_t index = std::min<std::size_t>(audio::kChiptuneWavetableMax - 1U,
        static_cast<std::size_t>(localX * static_cast<float>(audio::kChiptuneWavetableMax)));
    std::string error;
    if (!session_.draw_wavetable_point(index, 1.0F - localY * 2.0F, &error)) set_status(error);
}

void EditorChiptunePanel::play_song(audio::AudioMixer& mixer) noexcept {
    session_.stop_audition(mixer, 0.01F);
    const auto result = session_.audition_song(mixer, 30.0F, session_.song().loop);
    set_status(result ? std::string("Song preview -> ") + std::string(audio::audio_bus_name(result.bus)) : result.error);
}

void EditorChiptunePanel::play_instrument(audio::AudioMixer& mixer, int midiNote) noexcept {
    session_.stop_audition(mixer, 0.01F);
    if (midiNote < 0) midiNote = (session_.octave() + 1) * 12;
    const auto result = session_.audition_instrument(mixer, midiNote, 1.0F);
    set_status(result ? std::string("Instrument preview -> ") + std::string(audio::audio_bus_name(result.bus)) : result.error);
}

void EditorChiptunePanel::save() noexcept {
    std::string error;
    set_status(session_.save_file(documentPath_, &error) ? std::string("Saved ") + documentPath_.string() : error);
}

void EditorChiptunePanel::load() noexcept {
    std::string error;
    set_status(session_.load_file(documentPath_, &error) ? std::string("Opened ") + documentPath_.string() : error);
    keep_cursor_visible();
}

} // namespace dve::editor
