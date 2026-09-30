#include "dve/editor_synth.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>

#include "dve/audio/patch_genetics.hpp"

namespace dve::editor {
namespace {
bool contains(UiRect rect, int x, int y) noexcept { return rect.contains(x, y); }
std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
template <class Enum>
Enum cycle_enum(Enum value, unsigned count, int direction) noexcept {
    int index = static_cast<int>(value) + direction;
    while (index < 0) index += static_cast<int>(count);
    return static_cast<Enum>(index % static_cast<int>(count));
}
float stepped(float value, float delta, float minimum, float maximum, int direction) noexcept {
    return std::clamp(value + delta * static_cast<float>(direction), minimum, maximum);
}
std::int8_t cycle_oscillator_source(std::int8_t source, int direction) noexcept {
    int index = static_cast<int>(source) + 1 + direction;
    while (index < 0) index += static_cast<int>(audio::kSynthOscillatorCount + 1U);
    index %= static_cast<int>(audio::kSynthOscillatorCount + 1U);
    return static_cast<std::int8_t>(index - 1);
}
audio::FilterOversampling cycle_oversampling(audio::FilterOversampling value, int direction) noexcept {
    static constexpr std::array values{
        audio::FilterOversampling::X1, audio::FilterOversampling::X2, audio::FilterOversampling::X4,
        audio::FilterOversampling::Auto};
    std::size_t index = 0;
    for (std::size_t i = 0; i < values.size(); ++i) if (values[i] == value) index = i;
    int next = static_cast<int>(index) + direction;
    while (next < 0) next += static_cast<int>(values.size());
    return values[static_cast<std::size_t>(next) % values.size()];
}
} // namespace

void EditorSynthPanel::set_open(bool openValue, audio::Synthesizer& synth) noexcept {
    if (open_ && !openValue) release_panel_notes(synth);
    open_ = openValue;
    if (!openValue) searchPanel_.set_open(false);
}

void EditorSynthPanel::set_preset_directory(std::filesystem::path directory) noexcept {
    try {
        presetDirectory_ = std::move(directory);
        presetStatus_ = "Preset library not scanned";
        selectedPresetEntry_ = 0U;
    } catch (...) {
        presetStatus_ = "Could not set preset directory";
    }
}

bool EditorSynthPanel::refresh_preset_library() noexcept {
    try {
        std::string error;
        if (!presetLibrary_.scan(presetDirectory_, &error)) {
            presetStatus_ = error.empty() ? "Preset scan failed" : error;
            return false;
        }
        if (selectedPresetEntry_ >= presetLibrary_.entries().size()) selectedPresetEntry_ = 0U;
        presetStatus_ = std::to_string(presetLibrary_.entries().size()) + " presets indexed";
        return true;
    } catch (...) {
        presetStatus_ = "Preset scan failed";
        return false;
    }
}

void EditorSynthPanel::resize(int width, int height, float uiScale) noexcept {
    lastWidth_ = width; lastHeight_ = height; lastUiScale_ = uiScale;
    const int requestedScroll = gridScroll_;
    const int panelWidth = std::min(width - 24, std::max(940, static_cast<int>(1240.0F * uiScale)));
    const int panelHeight = std::min(height - 44, std::max(640, static_cast<int>(790.0F * uiScale)));
    layout_.panel = {(width - panelWidth) / 2, std::max(22, (height - panelHeight) / 2), panelWidth, panelHeight};
    layout_.titleBar = {layout_.panel.x, layout_.panel.y, panelWidth, 34};
    layout_.closeButton = {layout_.panel.x + panelWidth - 32, layout_.panel.y + 5, 24, 24};
    layout_.panicButton = {layout_.panel.x + panelWidth - 154, layout_.panel.y + 5, 108, 24};
    // Header row: wide enough for "Reset preset" and "Oct N" without eliding at 100 %.
    // Narrow panels (< 900 px, e.g. the 640x480 minimum) use a compact header so the
    // eight page tabs keep room for their short names.
    const bool compactHeader = panelWidth < 900;
    const int headerY = layout_.panel.y + 43;
    if (compactHeader) {
        layout_.resetButton = {layout_.panel.x + 12, headerY, 58, 24};
        layout_.octaveDownButton = {layout_.panel.x + 76, headerY, 24, 24};
        layout_.octaveUpButton = {layout_.panel.x + 146, headerY, 24, 24};
        layout_.midiThruButton = {layout_.panel.x + 176, headerY, 84, 24};
        // No room in the header row: the MIDI input button moves to the title bar.
        const int searchX = layout_.panel.x + panelWidth - 266;
        const int inputWidth = std::clamp(panelWidth / 4, 110, 150);
        layout_.midiInputButton = {searchX - 6 - inputWidth, layout_.panel.y + 5, inputWidth, 24};
    } else {
        layout_.resetButton = {layout_.panel.x + 12, headerY, 100, 24};
        layout_.octaveDownButton = {layout_.panel.x + 118, headerY, 30, 24};
        layout_.octaveUpButton = {layout_.panel.x + 194, headerY, 30, 24};
        layout_.midiThruButton = {layout_.panel.x + 232, headerY, 102, 24};
        layout_.midiInputButton = {layout_.panel.x + 340, headerY, 170, 24};
    }
    layout_.searchButton = {layout_.panel.x + panelWidth - 266, layout_.panel.y + 5, 104, 24};
    if (searchPanel_.open()) {
        const int searchWidth = std::min(layout_.panel.width - 80, 560);
        const int searchHeight = std::min(layout_.panel.height - 120, 430);
        searchPanel_.resize(layout_.panel.x + (layout_.panel.width - searchWidth) / 2,
                            layout_.panel.y + (layout_.panel.height - searchHeight) / 2,
                            searchWidth, searchHeight);
    }

    // Tabs share the header row with reset / octave / MIDI thru. On narrow panels they
    // shrink (short names, elided if needed, full name on hover) instead of running off the panel.
    const int tabsLeft = compactHeader ? layout_.midiThruButton.x + layout_.midiThruButton.width + 10
                                       : layout_.midiInputButton.x + layout_.midiInputButton.width + 10;
    const int tabWidth = std::max(28, (layout_.panel.x + panelWidth - 12 - tabsLeft) / static_cast<int>(kSynthPanelPageCount));
    for (std::size_t i = 0; i < layout_.tabButtons.size(); ++i)
        layout_.tabButtons[i] = {tabsLeft + static_cast<int>(i) * tabWidth,
                                 layout_.panel.y + 43, tabWidth - 4, 24};

    const int left = layout_.panel.x + 12;
    const int top = layout_.panel.y + 82;
    const int contentWidth = panelWidth - 24;
    constexpr int parameterRowHeight = 29;
    // The 3x10 parameter grid shares the page with a strip below it (Mod Matrix macros,
    // Performance arp steps). That strip used to sit at row 8 (top + 8*29 + 14), on top of
    // grid rows 8-9, and the grid is hit-tested first, so macro clicks edited parameters.
    // Size the grid so grid + strip fit above the piano, and put the strip after row 9.
    // Content must end above the voice meter (text top ~panel bottom - 162), not just the
    // piano. The Performance page is the tallest: grid + arp step buttons + 5 step rows.
    layout_.meterArea = {left, layout_.panel.y + panelHeight - 164, contentWidth, 44};
    const int contentLimit = layout_.meterArea.y - 4;
    constexpr int kGridRows = 10;
    constexpr int kStripGap = 6;
    constexpr int kArpStepButtonsPitch = 29;
    constexpr int kArpStepRowCount = 5;
    const int gridRowHeight = std::clamp(
        (contentLimit - top - kStripGap - kArpStepButtonsPitch) / (kGridRows + kArpStepRowCount),
        kMinSynthGridRowHeight, parameterRowHeight);
    const int gridButtonHeight = std::min(22, gridRowHeight - 5);
    const int gridButtonInset = std::max(1, (gridRowHeight - 3 - gridButtonHeight) / 2);
    const bool gridPage = page_ == SynthPanelPage::FilterEnvelope || page_ == SynthPanelPage::Modulation ||
                          page_ == SynthPanelPage::Performance || page_ == SynthPanelPage::Expression ||
                          page_ == SynthPanelPage::Generative;
    // A grid page is a list of lines: 10 grid rows, then the page's strip (Mod
    // Matrix macros, or Performance arp step buttons + 5 step rows). When the
    // lines do not fit between `top` and the voice meter, the page scrolls by
    // whole lines and lines outside the viewport get empty rects.
    enum class LineKind : std::uint8_t { GridRow, MacroStrip, ArpButtons, ArpStepRow };
    struct GridLine { LineKind kind; int row; int pitch; int extent; };
    std::array<GridLine, 16> lines{};
    int lineCount = 0;
    for (int row = 0; row < kGridRows; ++row)
        lines[static_cast<std::size_t>(lineCount++)] = {LineKind::GridRow, row, gridRowHeight, gridRowHeight - 3};
    if (page_ == SynthPanelPage::Modulation)
        lines[static_cast<std::size_t>(lineCount++)] = {LineKind::MacroStrip, 0, kStripGap + 28, kStripGap + 28};
    if (page_ == SynthPanelPage::Performance) {
        lines[static_cast<std::size_t>(lineCount++)] = {LineKind::ArpButtons, 0, kStripGap + kArpStepButtonsPitch, kStripGap + 23};
        for (int row = 0; row < kArpStepRowCount; ++row)
            lines[static_cast<std::size_t>(lineCount++)] = {LineKind::ArpStepRow, row, gridRowHeight, gridRowHeight - 3};
    }
    const int viewportHeight = std::max(0, contentLimit - top);
    auto span = [&](int first) {  // height of lines [first, lineCount) when laid out from `top`
        int height = 0;
        for (int i = first; i < lineCount; ++i)
            height += i + 1 == lineCount ? lines[static_cast<std::size_t>(i)].extent : lines[static_cast<std::size_t>(i)].pitch;
        return height;
    };
    gridMaxScroll_ = 0;
    if (gridPage)
        while (gridMaxScroll_ + 1 < lineCount && span(gridMaxScroll_) > viewportHeight) ++gridMaxScroll_;
    gridScroll_ = std::clamp(gridScroll_, 0, gridMaxScroll_);
    const bool gridScrolls = gridPage && gridMaxScroll_ > 0;
    const int gridWidth = gridScrolls ? contentWidth - 14 : contentWidth;
    layout_.gridViewport = gridPage ? UiRect{left, top, contentWidth, viewportHeight} : UiRect{};
    layout_.gridScrollTrack = {};
    layout_.gridScrollThumb = {};
    if (gridScrolls) {
        layout_.gridScrollTrack = {left + contentWidth - 8, top, 8, viewportHeight};
        int visibleLines = 0;
        for (int i = gridScroll_, y = 0; i < lineCount; ++i) {
            const GridLine& line = lines[static_cast<std::size_t>(i)];
            if (y + line.extent > viewportHeight) break;
            ++visibleLines;
            y += line.pitch;
        }
        const int thumbHeight = std::clamp(viewportHeight * std::max(1, visibleLines) / std::max(1, lineCount), 16, viewportHeight);
        layout_.gridScrollThumb = {layout_.gridScrollTrack.x,
                                   top + (viewportHeight - thumbHeight) * gridScroll_ / std::max(1, gridMaxScroll_),
                                   8, thumbHeight};
    }

    const int columnWidth = (gridWidth - 20) / 3;
    const int stripColumnWidth = (gridWidth - 30) / 4;
    auto place_grid_row = [&](int row, int y) {
        for (int column = 0; column < 3; ++column) {
            const std::size_t i = static_cast<std::size_t>(column * kGridRows + row);
            if (i >= layout_.parameterRows.size()) continue;
            const int x = left + column * (columnWidth + 10);
            const int by = y + gridButtonInset;
            layout_.parameterRows[i] = {x, y, columnWidth, gridRowHeight - 3};
            layout_.parameterDownButtons[i] = {x + columnWidth - 116, by, 27, gridButtonHeight};
            layout_.parameterUpButtons[i] = {x + columnWidth - 31, by, 27, gridButtonHeight};
            layout_.parameterToggleButtons[i] = {x + columnWidth - 82, by, 78, gridButtonHeight};
        }
    };
    auto hide_grid_row = [&](int row) {
        for (int column = 0; column < 3; ++column) {
            const std::size_t i = static_cast<std::size_t>(column * kGridRows + row);
            if (i >= layout_.parameterRows.size()) continue;
            layout_.parameterRows[i] = {}; layout_.parameterDownButtons[i] = {};
            layout_.parameterUpButtons[i] = {}; layout_.parameterToggleButtons[i] = {};
        }
    };
    auto place_macros = [&](int y) {
        for (std::size_t i = 0; i < layout_.macroRows.size(); ++i) {
            const int x = left + static_cast<int>(i) * (stripColumnWidth + 10);
            layout_.macroRows[i] = {x, y, stripColumnWidth, 28};
            layout_.macroDownButtons[i] = {x + stripColumnWidth - 102, y + 3, 26, 22};
            layout_.macroUpButtons[i] = {x + stripColumnWidth - 30, y + 3, 26, 22};
        }
    };
    auto hide_macros = [&] {
        layout_.macroRows = {}; layout_.macroDownButtons = {}; layout_.macroUpButtons = {};
    };
    auto place_arp_buttons = [&](int y) {
        const int arpStepWidth = std::max(34, gridWidth / static_cast<int>(audio::kArpeggiatorStepCount));
        for (std::size_t i = 0; i < layout_.arpeggiatorStepButtons.size(); ++i) {
            const int x = left + static_cast<int>(i) * arpStepWidth;
            const int right = i + 1U == layout_.arpeggiatorStepButtons.size() ? left + gridWidth : x + arpStepWidth;
            layout_.arpeggiatorStepButtons[i] = {x, y, std::max(22, right - x - 3), 23};
        }
    };
    auto place_arp_row = [&](int row, int y) {
        for (int column = 0; column < 4; ++column) {
            const std::size_t i = static_cast<std::size_t>(column * kArpStepRowCount + row);
            if (i >= layout_.arpeggiatorStepRows.size()) continue;
            const int x = left + column * (stripColumnWidth + 10);
            const int by = y + gridButtonInset;
            layout_.arpeggiatorStepRows[i] = {x, y, stripColumnWidth, gridRowHeight - 3};
            layout_.arpeggiatorStepDownButtons[i] = {x + stripColumnWidth - 116, by, 27, gridButtonHeight};
            layout_.arpeggiatorStepUpButtons[i] = {x + stripColumnWidth - 31, by, 27, gridButtonHeight};
            layout_.arpeggiatorStepToggleButtons[i] = {x + stripColumnWidth - 82, by, 78, gridButtonHeight};
        }
    };
    auto hide_arp_row = [&](int row) {
        for (int column = 0; column < 4; ++column) {
            const std::size_t i = static_cast<std::size_t>(column * kArpStepRowCount + row);
            if (i >= layout_.arpeggiatorStepRows.size()) continue;
            layout_.arpeggiatorStepRows[i] = {}; layout_.arpeggiatorStepDownButtons[i] = {};
            layout_.arpeggiatorStepUpButtons[i] = {}; layout_.arpeggiatorStepToggleButtons[i] = {};
        }
    };

    if (!gridPage) {
        // Unscrolled layout: both strips sit below the grid (only one page shows each).
        for (int row = 0; row < kGridRows; ++row) place_grid_row(row, top + row * gridRowHeight);
        const int gridBottom = top + kGridRows * gridRowHeight;  // one past the last row's gap
        place_macros(gridBottom + kStripGap);
        place_arp_buttons(gridBottom + kStripGap);
        for (int row = 0; row < kArpStepRowCount; ++row)
            place_arp_row(row, gridBottom + kStripGap + kArpStepButtonsPitch + row * gridRowHeight);
    } else {
        hide_macros();
        layout_.arpeggiatorStepButtons = {};
        for (int row = 0; row < kArpStepRowCount; ++row) hide_arp_row(row);
        int y = top;
        for (int i = 0; i < lineCount; ++i) {
            const GridLine& line = lines[static_cast<std::size_t>(i)];
            const bool visible = i >= gridScroll_ && y + line.extent <= contentLimit;
            switch (line.kind) {
                case LineKind::GridRow:
                    if (visible) place_grid_row(line.row, y); else hide_grid_row(line.row);
                    break;
                case LineKind::MacroStrip:
                    if (visible) place_macros(y + kStripGap);
                    break;
                case LineKind::ArpButtons:
                    if (visible) place_arp_buttons(y + kStripGap);
                    break;
                case LineKind::ArpStepRow:
                    if (visible) place_arp_row(line.row, y);
                    break;
            }
            if (i >= gridScroll_) y += line.pitch;
        }
    }

    layout_scrolling_pages(left, top, contentWidth, contentLimit, requestedScroll);

    const int pianoY = layout_.panel.y + panelHeight - 120;
    layout_.pianoArea = {left, pianoY, contentWidth, 90};
    keyboard_.layout(layout_.pianoArea);
    layout_.keyboardKeysButton = {left + contentWidth - 104, layout_.panel.y + panelHeight - 27, 104, 22};
}

namespace {
// One visual line of a scrolling page: its natural (unscrolled) vertical extent and the rects
// that sit on it. Scrolling moves whole lines; lines outside the viewport get empty rects.
struct PageLine {
    int y0{};
    int y1{};
    std::array<UiRect*, 24> rects{};
    std::size_t count{};
    void add(UiRect& rect) noexcept { if (count < rects.size()) rects[count++] = &rect; }
};
struct PageLines {
    std::array<PageLine, 40> lines{};
    std::size_t count{};
    PageLine& next(int y0, int y1) noexcept {
        PageLine& line = lines[std::min(count, lines.size() - 1U)];
        if (count < lines.size()) ++count;
        line = {}; line.y0 = y0; line.y1 = y1;
        return line;
    }
};
} // namespace

void EditorSynthPanel::layout_scrolling_pages(int left, int top, int contentWidth, int contentLimit,
                                              int requestedScroll) noexcept {
    // Oscillators, Effects and Presets are laid out for `width`, then (for the current page)
    // split into lines that scroll like the grid pages when they do not fit above the meter.
    // Below ~920 px the oscillator rows switch to compact columns (value labels move to a
    // header line), the advanced rows use two columns and the wavetable tools wrap.
    std::size_t oscillatorEssentialLines = 0;
    auto place = [&](int width, PageLines& result) {
        result.count = 0;
        PageLines lines;  // lines of the section being laid out; kept only for the current page
        const SynthPanelPage page = page_;
        // ---- Oscillators ----
        const bool compact = width < 916;
        layout_.oscillatorCompact = compact;
        int y = top;
        if (compact) {
            layout_.oscillatorHeader = {left, y, width, 18};
            PageLine& line = lines.next(y, y + 18);
            if (page == SynthPanelPage::Oscillators) line.add(layout_.oscillatorHeader);
            y += 22;
        } else {
            layout_.oscillatorHeader = {};
        }
        constexpr int oscillatorRowHeight = 34;
        const int waveWidth = compact ? std::clamp(width - 88 - 8 - 4 * 96 - 4, 64, 112) : 112;
        const int groupLeft = left + 88 + waveWidth + 8;
        const int groupWidth = compact ? std::max(80, (left + width - 4 - groupLeft) / 4) : 0;
        for (std::size_t i = 0; i < layout_.oscillatorRows.size(); ++i) {
            const int rowY = y + static_cast<int>(i) * oscillatorRowHeight;
            layout_.oscillatorRows[i] = {left, rowY, width, oscillatorRowHeight - 3};
            layout_.oscillatorEnableButtons[i] = {left + 4, rowY + 4, 30, 23};
            layout_.oscillatorWaveButtons[i] = {left + 88, rowY + 4, waveWidth, 23};
            std::array<UiRect*, 8> buttons{&layout_.oscillatorGainDownButtons[i], &layout_.oscillatorGainUpButtons[i],
                                           &layout_.oscillatorTuneDownButtons[i], &layout_.oscillatorTuneUpButtons[i],
                                           &layout_.oscillatorFineDownButtons[i], &layout_.oscillatorFineUpButtons[i],
                                           &layout_.oscillatorPwmDownButtons[i], &layout_.oscillatorPwmUpButtons[i]};
            static constexpr std::array<int, 8> wide{254, 342, 430, 506, 600, 688, 790, 886};
            for (std::size_t b = 0; b < buttons.size(); ++b) {
                const int group = static_cast<int>(b / 2U);
                const int x = compact ? groupLeft + group * groupWidth + ((b & 1U) != 0U ? groupWidth - 30 : 0)
                                      : left + wide[b];
                *buttons[b] = {x, rowY + 4, 24, 23};
            }
            PageLine& line = lines.next(rowY, rowY + oscillatorRowHeight - 3);
            if (page == SynthPanelPage::Oscillators) {
                line.add(layout_.oscillatorRows[i]); line.add(layout_.oscillatorEnableButtons[i]);
                line.add(layout_.oscillatorWaveButtons[i]);
                for (UiRect* button : buttons) line.add(*button);
            }
        }
        constexpr int parameterRowHeight = 29;
        const int advancedTop = y + static_cast<int>(audio::kSynthOscillatorCount) * oscillatorRowHeight + 7;
        const int columns = width >= 760 ? 3 : 2;
        const int rowsPerColumn = static_cast<int>((kSynthOscillatorAdvancedPropertyCount + static_cast<std::size_t>(columns) - 1U) /
                                                   static_cast<std::size_t>(columns));
        const int advancedColumnWidth = (width - 10 * (columns - 1)) / columns;
        std::array<PageLine*, kSynthOscillatorAdvancedPropertyCount> advancedLines{};
        for (int row = 0; row < rowsPerColumn; ++row) {
            const int rowY = advancedTop + row * parameterRowHeight;
            advancedLines[static_cast<std::size_t>(row)] = &lines.next(rowY, rowY + parameterRowHeight - 3);
        }
        for (std::size_t i = 0; i < layout_.oscillatorAdvancedRows.size(); ++i) {
            const int column = static_cast<int>(i) / rowsPerColumn;
            const int row = static_cast<int>(i) % rowsPerColumn;
            const int x = left + column * (advancedColumnWidth + 10);
            const int rowY = advancedTop + row * parameterRowHeight;
            layout_.oscillatorAdvancedRows[i] = {x, rowY, advancedColumnWidth, parameterRowHeight - 3};
            layout_.oscillatorAdvancedDownButtons[i] = {x + advancedColumnWidth - 116, rowY + 2, 27, 22};
            layout_.oscillatorAdvancedUpButtons[i] = {x + advancedColumnWidth - 31, rowY + 2, 27, 22};
            if (page == SynthPanelPage::Oscillators) {
                PageLine& line = *advancedLines[static_cast<std::size_t>(row)];
                line.add(layout_.oscillatorAdvancedRows[i]); line.add(layout_.oscillatorAdvancedDownButtons[i]);
                line.add(layout_.oscillatorAdvancedUpButtons[i]);
            }
        }
        oscillatorEssentialLines = lines.count;  // the wavetable section below is optional
        const int canvasY = advancedTop + rowsPerColumn * parameterRowHeight + 6;
        layout_.wavetableCanvas = {left, canvasY, width, 76};
        {
            PageLine& line = lines.next(canvasY, canvasY + 76);
            if (page == SynthPanelPage::Oscillators) line.add(layout_.wavetableCanvas);
        }
        const int frames = static_cast<int>(audio::kWavetableFrameCount);
        int frameWidth = std::max(44, (width - 330) / frames);
        constexpr int toolsWidth = 272;
        const bool wrapTools = frames * frameWidth + 8 + toolsWidth > width;
        if (wrapTools) frameWidth = std::max(30, std::min(60, width / frames));
        const int framesY = canvasY + 76 + 5;
        PageLine& frameLine = lines.next(framesY, framesY + 23);
        for (std::size_t i = 0; i < layout_.wavetableFrameButtons.size(); ++i) {
            layout_.wavetableFrameButtons[i] = {left + static_cast<int>(i) * frameWidth, framesY, frameWidth - 3, 23};
            if (page == SynthPanelPage::Oscillators) frameLine.add(layout_.wavetableFrameButtons[i]);
        }
        const int toolsX = wrapTools ? left : left + frames * frameWidth + 8;
        const int toolsY = wrapTools ? framesY + 28 : framesY;
        layout_.wavetableNormalizeButton = {toolsX, toolsY, 96, 23};
        layout_.wavetableRemoveDcButton = {toolsX + 102, toolsY, 82, 23};
        layout_.wavetableAlignButton = {toolsX + 190, toolsY, 82, 23};
        PageLine& toolsLine = wrapTools ? lines.next(toolsY, toolsY + 23) : frameLine;
        if (page == SynthPanelPage::Oscillators) {
            toolsLine.add(layout_.wavetableNormalizeButton); toolsLine.add(layout_.wavetableRemoveDcButton);
            toolsLine.add(layout_.wavetableAlignButton);
        }
        if (page == SynthPanelPage::Oscillators) result = lines;
        lines.count = 0;

        // ---- Effects ----
        const int effectColumnWidth = (width - 10) / 2;
        // Effect rows are 30 px on a 36 px pitch so the list plus the selected effect's
        // parameters fit a 1280x800 window without scrolling.
        constexpr int effectPitch = 36;
        for (int row = 0; row < 7; ++row) {
            const int rowY = top + row * effectPitch;
            PageLine& line = lines.next(rowY, rowY + 30);
            for (int column = 0; column < 2; ++column) {
                const std::size_t i = static_cast<std::size_t>(column * 7 + row);
                if (i >= layout_.effectRows.size()) continue;
                const int x = left + column * (effectColumnWidth + 10);
                layout_.effectRows[i] = {x, rowY, effectColumnWidth, 30};
                layout_.effectToggleButtons[i] = {x + effectColumnWidth - 70, rowY + 3, 60, 24};
                if (page == SynthPanelPage::Effects) { line.add(layout_.effectRows[i]); line.add(layout_.effectToggleButtons[i]); }
            }
        }
        {
            const int titleY = top + 7 * effectPitch;
            layout_.effectParamTitle = {left, titleY, effectColumnWidth, 20};
            PageLine& titleLine = lines.next(titleY, titleY + 20);
            if (page == SynthPanelPage::Effects) titleLine.add(layout_.effectParamTitle);
            const int paramTop = titleY + 24;
            for (std::size_t i = 0; i < layout_.effectParamRows.size(); ++i) {
                const int rowY = paramTop + static_cast<int>(i) * 32;
                layout_.effectParamRows[i] = {left, rowY, effectColumnWidth, 29};
                layout_.effectParamDownButtons[i] = {left + effectColumnWidth - 116, rowY + 2, 27, 22};
                layout_.effectParamUpButtons[i] = {left + effectColumnWidth - 31, rowY + 2, 27, 22};
                layout_.effectParamToggleButtons[i] = {left + effectColumnWidth - 82, rowY + 2, 78, 22};
                PageLine& line = lines.next(rowY, rowY + 29);
                if (page == SynthPanelPage::Effects) {
                    line.add(layout_.effectParamRows[i]); line.add(layout_.effectParamDownButtons[i]);
                    line.add(layout_.effectParamUpButtons[i]); line.add(layout_.effectParamToggleButtons[i]);
                }
            }
        }
        if (page == SynthPanelPage::Effects) result = lines;
        lines.count = 0;

        // ---- Presets ----
        // Toolbar (wraps into two lines below 700 px), 7 MIDI-learn rows, the preset
        // entries and a status line. (The learn rows used to sit on top of the toolbar.)
        const bool wrapToolbar = width < 700;
        y = top;
        {
            PageLine& line = lines.next(y, y + 26);
            layout_.presetScanButton = {left, y, 118, 26};
            layout_.presetPreviousButton = {left + 128, y, 36, 26};
            layout_.presetNextButton = {left + 170, y, 36, 26};
            layout_.presetLoadButton = {left + 214, y, 90, 26};
            PageLine* second = &line;
            int x = left + 318;
            if (wrapToolbar) { y += 32; second = &lines.next(y, y + 26); x = left; }
            layout_.presetCaptureAButton = {x, y, 90, 26};
            layout_.presetCaptureBButton = {x + 96, y, 90, 26};
            layout_.presetMorphDownButton = {x + 202, y, 36, 26};
            layout_.presetMorphUpButton = {x + 334, y, 36, 26};
            if (page == SynthPanelPage::Presets) {
                for (UiRect* rect : {&layout_.presetScanButton, &layout_.presetPreviousButton,
                                     &layout_.presetNextButton, &layout_.presetLoadButton}) line.add(*rect);
                for (UiRect* rect : {&layout_.presetCaptureAButton, &layout_.presetCaptureBButton,
                                     &layout_.presetMorphDownButton, &layout_.presetMorphUpButton}) second->add(*rect);
            }
            y += 34;
        }
        const int learnWidth = width >= 700 ? (width - 10) / 2 : width;
        if (page == SynthPanelPage::Presets) {
            for (std::size_t i = 0; i < kSynthParameterRowCount; ++i) {
                if (i < 7U) {
                    const int rowY = y + static_cast<int>(i) * parameterRowHeight;
                    layout_.parameterRows[i] = {left, rowY, learnWidth, parameterRowHeight - 3};
                    layout_.parameterDownButtons[i] = {left + learnWidth - 116, rowY + 2, 27, 22};
                    layout_.parameterUpButtons[i] = {left + learnWidth - 31, rowY + 2, 27, 22};
                    layout_.parameterToggleButtons[i] = {left + learnWidth - 82, rowY + 2, 78, 22};
                    PageLine& line = lines.next(rowY, rowY + parameterRowHeight - 3);
                    line.add(layout_.parameterRows[i]); line.add(layout_.parameterDownButtons[i]);
                    line.add(layout_.parameterUpButtons[i]); line.add(layout_.parameterToggleButtons[i]);
                } else {
                    layout_.parameterRows[i] = {}; layout_.parameterDownButtons[i] = {};
                    layout_.parameterUpButtons[i] = {}; layout_.parameterToggleButtons[i] = {};
                }
            }
        }
        y += 7 * parameterRowHeight + 8;
        const int entryColumnWidth = (width - 10) / 2;
        for (int row = 0; row < 4; ++row) {
            const int rowY = y + row * 34;
            PageLine& line = lines.next(rowY, rowY + 29);
            for (int column = 0; column < 2; ++column) {
                const std::size_t i = static_cast<std::size_t>(column * 4 + row);
                if (i >= layout_.presetEntryButtons.size()) continue;
                layout_.presetEntryButtons[i] = {left + column * (entryColumnWidth + 10), rowY, entryColumnWidth, 29};
                if (page == SynthPanelPage::Presets) line.add(layout_.presetEntryButtons[i]);
            }
        }
        y += 4 * 34;
        layout_.presetStatusRow = {left, y, width, 18};
        PageLine& statusLine = lines.next(y, y + 18);
        if (page == SynthPanelPage::Presets) statusLine.add(layout_.presetStatusRow);
        if (page == SynthPanelPage::Presets) result = lines;
    };

    const bool scrollingPage = page_ == SynthPanelPage::Oscillators || page_ == SynthPanelPage::Effects ||
                               page_ == SynthPanelPage::Presets;
    PageLines lines;
    place(contentWidth, lines);
    if (!scrollingPage) return;
    const int viewportHeight = std::max(0, contentLimit - top);
    auto max_scroll = [&] {
        // The wavetable / sample section only counts when the selected oscillator shows it.
        const std::size_t scrollLines = page_ == SynthPanelPage::Oscillators && !wavetableSectionVisible_
            ? std::min(lines.count, oscillatorEssentialLines) : lines.count;
        if (scrollLines == 0) return 0;
        const int bottom = lines.lines[scrollLines - 1U].y1;
        int first = 0;
        while (first + 1 < static_cast<int>(lines.count) &&
               bottom - lines.lines[static_cast<std::size_t>(first)].y0 > viewportHeight) ++first;
        return first;
    };
    int maxScroll = max_scroll();
    int width = contentWidth;
    if (maxScroll > 0) {
        width = contentWidth - 14;  // room for the scrollbar
        place(width, lines);
        maxScroll = max_scroll();
    }
    gridMaxScroll_ = maxScroll;
    gridScroll_ = std::clamp(requestedScroll, 0, gridMaxScroll_);
    layout_.gridViewport = {left, top, contentWidth, viewportHeight};
    layout_.gridScrollTrack = {};
    layout_.gridScrollThumb = {};
    if (lines.count == 0) return;
    const int dy = lines.lines[static_cast<std::size_t>(gridScroll_)].y0 - top;
    int visibleLines = 0;
    for (std::size_t i = 0; i < lines.count; ++i) {
        PageLine& line = lines.lines[i];
        const bool visible = static_cast<int>(i) >= gridScroll_ && line.y1 - dy <= contentLimit;
        if (visible) ++visibleLines;
        for (std::size_t r = 0; r < line.count; ++r) {
            UiRect& rect = *line.rects[r];
            if (visible) rect.y -= dy;
            else rect = {};
        }
    }
    if (gridMaxScroll_ > 0) {
        layout_.gridScrollTrack = {left + contentWidth - 8, top, 8, viewportHeight};
        const int thumbHeight = std::clamp(viewportHeight * std::max(1, visibleLines) /
                                           std::max(1, static_cast<int>(lines.count)), 16, viewportHeight);
        layout_.gridScrollThumb = {layout_.gridScrollTrack.x,
                                   top + (viewportHeight - thumbHeight) * gridScroll_ / std::max(1, gridMaxScroll_),
                                   8, thumbHeight};
    }
}

void EditorSynthPanel::sync_wavetable_section(const audio::Synthesizer& synth) {
    if (!open_ || page_ != SynthPanelPage::Oscillators) return;
    const audio::SynthPreset preset = synth.preset();
    const auto waveform = preset.oscillators[std::min(selectedOscillator_, preset.oscillators.size() - 1U)].waveform;
    const bool visible = waveform == audio::OscillatorWaveform::Wavetable ||
        ((waveform == audio::OscillatorWaveform::Sample || waveform == audio::OscillatorWaveform::Granular) &&
         preset.sampleBank.enabled && preset.sampleBank.frameCount > 1U);
    if (visible == wavetableSectionVisible_) return;
    wavetableSectionVisible_ = visible;
    resize(lastWidth_, lastHeight_, lastUiScale_);
}

void EditorSynthPanel::set_keyboard_key_count(int count) noexcept {
    keyboard_.set_key_count(count);
    keyboard_.layout(layout_.pianoArea);
}

void EditorSynthPanel::set_page(SynthPanelPage page) noexcept {
    if (page_ != page) gridScroll_ = 0;
    page_ = page;
    resize(lastWidth_, lastHeight_, lastUiScale_);
}

bool EditorSynthPanel::scroll_grid(int lines) noexcept {
    const int before = gridScroll_;
    gridScroll_ = std::clamp(gridScroll_ + lines, 0, gridMaxScroll_);
    if (gridScroll_ == before) return false;
    resize(lastWidth_, lastHeight_, lastUiScale_);
    return true;
}

bool EditorSynthPanel::grid_scrollbar_press(int x, int y) noexcept {
    const UiRect& track = layout_.gridScrollTrack;
    if (track.width <= 0 || !contains(UiRect{track.x - 3, track.y, track.width + 6, track.height}, x, y)) return false;
    const UiRect& thumb = layout_.gridScrollThumb;
    if (y < thumb.y) (void)scroll_grid(-3);
    else if (y >= thumb.y + thumb.height) (void)scroll_grid(3);
    return true;
}

bool EditorSynthPanel::pointer_wheel(float steps, int x, int y) noexcept {
    if (!open_ || !contains(layout_.panel, x, y)) return false;
    if (contains(layout_.pianoArea, x, y)) { (void)keyboard_.wheel(steps); return true; }
    if (contains(layout_.gridViewport, x, y) && gridMaxScroll_ > 0) {
        const int lines = steps > 0.0F ? -std::max(1, static_cast<int>(steps)) : std::max(1, static_cast<int>(-steps));
        (void)scroll_grid(lines);
        return true;
    }
    return true;  // the modal panel swallows wheel input
}

void EditorSynthPanel::release_panel_notes(audio::Synthesizer& synth) noexcept {
    for (std::size_t note = 0; note < keyboardNotes_.size(); ++note) {
        if (keyboardNotes_[note]) (void)synth.note_off(static_cast<std::uint8_t>(note));
        keyboardNotes_[note] = false;
    }
    if (pointerNote_ >= 0) (void)synth.note_off(static_cast<std::uint8_t>(pointerNote_));
    pointerNote_ = -1;
}

void EditorSynthPanel::cycle_waveform(std::size_t oscillator, int direction,
                                      audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    preset.oscillators[oscillator].waveform = cycle_enum(
        preset.oscillators[oscillator].waveform, 17U, direction);  // Phase 5: +Spectral
    synth.set_preset(preset);
}

void EditorSynthPanel::toggle_effect(std::size_t index, audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    switch (index) {
        case 0: preset.distortion.enabled = !preset.distortion.enabled; break;
        case 1: preset.bitcrusher.enabled = !preset.bitcrusher.enabled; break;
        case 2: preset.harmonizer.enabled = !preset.harmonizer.enabled; break;
        case 3: preset.eq.enabled = !preset.eq.enabled; break;
        case 4: preset.chorus.enabled = !preset.chorus.enabled; break;
        case 5: preset.flanger.enabled = !preset.flanger.enabled; break;
        case 6: preset.ensemble.enabled = !preset.ensemble.enabled; break;
        case 7: preset.phaser.enabled = !preset.phaser.enabled; break;
        case 8: preset.delay.enabled = !preset.delay.enabled; break;
        case 9: preset.reverb.enabled = !preset.reverb.enabled; break;
        case 10: preset.compressor.enabled = !preset.compressor.enabled; break;
        case 11: preset.limiter.enabled = !preset.limiter.enabled; break;
        case 12: preset.diffusionDelay.enabled = !preset.diffusionDelay.enabled; break;
        default: return;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_effect_param(std::size_t paramIndex, int direction,
                                           audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    switch (selectedEffect_) {
        case 0: // Distortion
            switch (paramIndex) {
                case 0: preset.distortion.drive = stepped(preset.distortion.drive, 0.2F, 0.05F, 32.0F, direction); break;
                case 1: preset.distortion.mix = stepped(preset.distortion.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 1: // Bitcrusher
            switch (paramIndex) {
                case 0: preset.bitcrusher.bits = static_cast<std::uint8_t>(std::clamp<int>(preset.bitcrusher.bits + direction, 1, 16)); break;
                case 1: preset.bitcrusher.downsample = static_cast<std::uint8_t>(std::clamp<int>(preset.bitcrusher.downsample + direction, 1, 64)); break;
                case 2: preset.bitcrusher.mix = stepped(preset.bitcrusher.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 2: // Harmonizer
            switch (paramIndex) {
                case 0: preset.harmonizer.subLevel = stepped(preset.harmonizer.subLevel, 0.05F, 0.0F, 1.0F, direction); break;
                case 1: preset.harmonizer.upLevel = stepped(preset.harmonizer.upLevel, 0.05F, 0.0F, 1.0F, direction); break;
                case 2: preset.harmonizer.mix = stepped(preset.harmonizer.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 3: // EQ
            switch (paramIndex) {
                case 0: preset.eq.lowGainDb = stepped(preset.eq.lowGainDb, 0.5F, -24.0F, 24.0F, direction); break;
                case 1: preset.eq.midGainDb = stepped(preset.eq.midGainDb, 0.5F, -24.0F, 24.0F, direction); break;
                case 2: preset.eq.highGainDb = stepped(preset.eq.highGainDb, 0.5F, -24.0F, 24.0F, direction); break;
                default: return;
            }
            break;
        case 4: // Chorus
            switch (paramIndex) {
                case 0: preset.chorus.rateHertz = stepped(preset.chorus.rateHertz, 0.05F, 0.01F, 20.0F, direction); break;
                case 1: preset.chorus.depthMilliseconds = stepped(preset.chorus.depthMilliseconds, 0.5F, 0.0F, 30.0F, direction); break;
                case 2: preset.chorus.mix = stepped(preset.chorus.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 5: // Flanger
            switch (paramIndex) {
                case 0: preset.flanger.rateHertz = stepped(preset.flanger.rateHertz, 0.05F, 0.01F, 20.0F, direction); break;
                case 1: preset.flanger.depthMilliseconds = stepped(preset.flanger.depthMilliseconds, 0.25F, 0.0F, 10.0F, direction); break;
                case 2: preset.flanger.feedback = stepped(preset.flanger.feedback, 0.05F, -0.92F, 0.92F, direction); break;
                case 3: preset.flanger.mix = stepped(preset.flanger.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 6: // Ensemble
            switch (paramIndex) {
                case 1: preset.ensemble.mix = stepped(preset.ensemble.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 7: // Phaser
            switch (paramIndex) {
                case 0: preset.phaser.rateHertz = stepped(preset.phaser.rateHertz, 0.05F, 0.01F, 20.0F, direction); break;
                case 1: preset.phaser.depth = stepped(preset.phaser.depth, 0.05F, 0.0F, 1.0F, direction); break;
                case 2: preset.phaser.feedback = stepped(preset.phaser.feedback, 0.05F, -0.95F, 0.95F, direction); break;
                case 3: preset.phaser.mix = stepped(preset.phaser.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 8: // Delay
            switch (paramIndex) {
                case 0: preset.delay.timeSeconds = stepped(preset.delay.timeSeconds, 0.01F, 0.01F, 1.95F, direction); break;
                case 1: preset.delay.feedback = stepped(preset.delay.feedback, 0.02F, 0.0F, 0.94F, direction); break;
                case 2: preset.delay.mix = stepped(preset.delay.mix, 0.05F, 0.0F, 1.0F, direction); break;
                case 5: preset.delay.syncBeats = stepped(preset.delay.syncBeats, 0.25F, 0.03125F, 32.0F, direction); break;
                default: return;
            }
            break;
        case 9: // Reverb
            switch (paramIndex) {
                case 0: preset.reverb.roomSize = stepped(preset.reverb.roomSize, 0.05F, 0.0F, 1.0F, direction); break;
                case 1: preset.reverb.damping = stepped(preset.reverb.damping, 0.02F, 0.0F, 0.98F, direction); break;
                case 2: preset.reverb.width = stepped(preset.reverb.width, 0.05F, 0.0F, 1.0F, direction); break;
                case 3: preset.reverb.mix = stepped(preset.reverb.mix, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        case 10: // Compressor
            switch (paramIndex) {
                case 0: preset.compressor.thresholdDb = stepped(preset.compressor.thresholdDb, 1.0F, -60.0F, 0.0F, direction); break;
                case 1: preset.compressor.ratio = stepped(preset.compressor.ratio, 0.5F, 1.0F, 30.0F, direction); break;
                case 2: preset.compressor.attackMilliseconds = stepped(preset.compressor.attackMilliseconds, 1.0F, 0.05F, 500.0F, direction); break;
                case 3: preset.compressor.releaseMilliseconds = stepped(preset.compressor.releaseMilliseconds, 10.0F, 1.0F, 5000.0F, direction); break;
                case 4: preset.compressor.makeupDb = stepped(preset.compressor.makeupDb, 0.5F, -24.0F, 24.0F, direction); break;
                default: return;
            }
            break;
        case 11: // Limiter
            switch (paramIndex) {
                case 0: preset.limiter.ceilingDb = stepped(preset.limiter.ceilingDb, 0.5F, -24.0F, 0.0F, direction); break;
                case 1: preset.limiter.releaseMilliseconds = stepped(preset.limiter.releaseMilliseconds, 10.0F, 1.0F, 5000.0F, direction); break;
                default: return;
            }
            break;
        case 12: // Diffusion delay (Phase 2)
            switch (paramIndex) {
                case 0: preset.diffusionDelay.timeSeconds = stepped(preset.diffusionDelay.timeSeconds, 0.01F, 0.01F, 1.95F, direction); break;
                case 1: preset.diffusionDelay.feedback = stepped(preset.diffusionDelay.feedback, 0.02F, 0.0F, 0.94F, direction); break;
                case 2: preset.diffusionDelay.mix = stepped(preset.diffusionDelay.mix, 0.05F, 0.0F, 1.0F, direction); break;
                case 3: preset.diffusionDelay.diffusion = stepped(preset.diffusionDelay.diffusion, 0.05F, 0.0F, 1.0F, direction); break;
                default: return;
            }
            break;
        default: return;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::toggle_effect_param(std::size_t paramIndex, audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    switch (selectedEffect_) {
        case 0: // Distortion: cycle Classic/Fuzz/SoftClip/Foldback (Phase 2 adds the last two)
            if (paramIndex == 2) preset.distortion.mode = static_cast<audio::DistortionMode>(
                (static_cast<unsigned>(preset.distortion.mode) + 1U) % 4U);
            else return;
            break;
        case 6: // Ensemble: mode I/II/Both
            if (paramIndex == 0) preset.ensemble.mode = static_cast<audio::EnsembleMode>(
                (static_cast<unsigned>(preset.ensemble.mode) + 1U) % 3U);
            else return;
            break;
        case 8: // Delay: ping-pong and tempo sync
            if (paramIndex == 3) preset.delay.pingPong = !preset.delay.pingPong;
            else if (paramIndex == 4) preset.delay.tempoSync = !preset.delay.tempoSync;
            else return;
            break;
        default: return;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_oscillator_advanced(std::size_t index, int direction,
                                                   audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    auto& oscillator = preset.oscillators[selectedOscillator_];
    if (oscillator.waveform == audio::OscillatorWaveform::Sample) {
        switch (index) {
            case 0: oscillator.sampleStart = stepped(oscillator.sampleStart, 0.025F, 0.0F, oscillator.sampleEnd - 0.01F, direction); break;
            case 1: oscillator.sampleEnd = stepped(oscillator.sampleEnd, 0.025F, oscillator.sampleStart + 0.01F, 1.0F, direction); break;
            case 2: oscillator.sampleLoopStart = stepped(oscillator.sampleLoopStart, 0.025F, 0.0F, oscillator.sampleLoopEnd - 0.01F, direction); break;
            case 3: oscillator.sampleLoopEnd = stepped(oscillator.sampleLoopEnd, 0.025F, oscillator.sampleLoopStart + 0.01F, 1.0F, direction); break;
            case 4: oscillator.sampleLoop = !oscillator.sampleLoop; break;
            case 5: oscillator.sampleReverse = !oscillator.sampleReverse; break;
            case 6: oscillator.sampleKeyTrack = !oscillator.sampleKeyTrack; break;
            case 7: oscillator.sampleVelocityToGain = stepped(oscillator.sampleVelocityToGain, 0.05F, 0.0F, 1.0F, direction); break;
            case 8: oscillator.sampleOneShot = !oscillator.sampleOneShot; break;
            case 9: preset.sampleBank.rootNote = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.sampleBank.rootNote) + direction, 0, 127)); break;
            default: return;
        }
    } else if (oscillator.waveform == audio::OscillatorWaveform::Sampler) {
        // Phase 2: dedicated sampler generator reads the preset sample bank.
        auto& sampler = preset.sampler;
        switch (index) {
            case 0: sampler.playbackMode = cycle_enum(sampler.playbackMode, 2U, direction); break;
            case 1: sampler.direction = cycle_enum(sampler.direction, 2U, direction); break;
            case 2: sampler.pitchTracking = !sampler.pitchTracking; break;
            case 3: sampler.startOffsetSeconds = stepped(sampler.startOffsetSeconds, 0.01F, 0.0F, 3600.0F, direction); break;
            case 4: sampler.gain = stepped(sampler.gain, 0.05F, 0.0F, 2.0F, direction); break;
            case 5: sampler.loopStartSeconds = stepped(sampler.loopStartSeconds, 0.01F, 0.0F, 3600.0F, direction); break;
            case 6: sampler.loopEndSeconds = stepped(sampler.loopEndSeconds, 0.01F, 0.0F, 3600.0F, direction); break;
            case 7: sampler.loopCrossfadeSeconds = stepped(sampler.loopCrossfadeSeconds, 0.001F, 0.0F, 60.0F, direction); break;
            case 8: sampler.enabled = !sampler.enabled; break;
            case 9: preset.sampleBank.rootNote = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.sampleBank.rootNote) + direction, 0, 127)); break;
            default: return;
        }
    } else if (oscillator.waveform == audio::OscillatorWaveform::ModalResonator) {
        // Phase 2: modal resonator bank key parameters.
        auto& modal = oscillator.modalResonator;
        switch (index) {
            case 0: modal.excitation = cycle_enum(modal.excitation, 4U, direction); break;
            case 1: modal.baseFrequency = stepped(modal.baseFrequency, 10.0F, 0.0F, 20000.0F, direction); break;
            case 2: modal.damping = stepped(modal.damping, 0.05F, 0.01F, 8.0F, direction); break;
            case 3: modal.inharmonicity = stepped(modal.inharmonicity, 0.02F, 0.0F, 1.0F, direction); break;
            case 4: modal.brightness = stepped(modal.brightness, 0.05F, 0.0F, 1.0F, direction); break;
            case 5: modal.excitationLevel = stepped(modal.excitationLevel, 0.05F, 0.0F, 4.0F, direction); break;
            case 6: modal.noiseBurstMilliseconds = stepped(modal.noiseBurstMilliseconds, 5.0F, 1.0F, 2000.0F, direction); break;
            case 7: modal.transientMilliseconds = stepped(modal.transientMilliseconds, 5.0F, 1.0F, 2000.0F, direction); break;
            case 8: modal.modeCount = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(modal.modeCount) + direction, 1, 32)); break;
            case 9: modal.modes[0].frequencyRatio = stepped(modal.modes[0].frequencyRatio, 0.01F, 0.25F, 4.0F, direction); break;
            default: return;
        }
    } else if (oscillator.waveform == audio::OscillatorWaveform::Granular) {
        // Phase 4: dedicated granular generator reads preset-level GranularParameters.
        auto& granular = preset.granular;
        switch (index) {
            case 0: granular.enabled = !granular.enabled; break;
            case 1: granular.densityHz = stepped(granular.densityHz, 1.0F, 0.5F, 4000.0F, direction); break;
            case 2: granular.durationMs = stepped(granular.durationMs, 10.0F, 10.0F, 2000.0F, direction); break;
            case 3: granular.position01 = stepped(granular.position01, 0.025F, 0.0F, 1.0F, direction); break;
            case 4: granular.positionJitter01 = stepped(granular.positionJitter01, 0.025F, 0.0F, 1.0F, direction); break;
            case 5: granular.envelopeShape = cycle_enum(granular.envelopeShape, 4U, direction); break;
            case 6: granular.cloud01 = stepped(granular.cloud01, 0.05F, 0.0F, 1.0F, direction); break;
            case 7: granular.scatter01 = stepped(granular.scatter01, 0.05F, 0.0F, 1.0F, direction); break;
            case 8: granular.dust01 = stepped(granular.dust01, 0.05F, 0.0F, 1.0F, direction); break;
            case 9: granular.freeze01 = stepped(granular.freeze01, 0.05F, 0.0F, 1.0F, direction); break;
            case 10: granular.smear01 = stepped(granular.smear01, 0.05F, 0.0F, 1.0F, direction); break;
            case 11: granular.width01 = stepped(granular.width01, 0.05F, 0.0F, 1.0F, direction); break;
            case 12: granular.pitchSemitones = stepped(granular.pitchSemitones, 1.0F, -48.0F, 48.0F, direction); break;
            case 13: granular.gain = stepped(granular.gain, 0.05F, 0.0F, 2.0F, direction); break;
            case 14: granular.panScatter01 = stepped(granular.panScatter01, 0.05F, 0.0F, 1.0F, direction); break;
            case 15: granular.reverseProbability01 = stepped(granular.reverseProbability01, 0.05F, 0.0F, 1.0F, direction); break;
            case 16: granular.freezePosition01 = stepped(granular.freezePosition01, 0.025F, 0.0F, 1.0F, direction); break;
            case 17: granular.granularQuality = cycle_enum(granular.granularQuality, 4U, direction); break;
            default: return;
        }
    } else if (oscillator.waveform == audio::OscillatorWaveform::Spectral) {
        // Phase 5: spectral/resynthesis oscillator reads preset-level
        // SpectralParameters (the spectral.hpp contract, unified at merge).
        // 14 rows — fits the 18-row advanced budget, so
        // kSynthOscillatorAdvancedPropertyCount is unchanged (no ABI break).
        auto& spectral = preset.spectral;
        switch (index) {
            case 0: spectral.enabled = !spectral.enabled; break;
            case 1: spectral.gain = stepped(spectral.gain, 0.05F, 0.0F, 2.0F, direction); break;
            case 2: spectral.timeStretch = stepped(spectral.timeStretch, 0.05F, 0.0625F, 16.0F, direction); break;
            case 3: spectral.freeze01 = stepped(spectral.freeze01, 0.05F, 0.0F, 1.0F, direction); break;
            case 4: spectral.formantShiftSemitones = stepped(spectral.formantShiftSemitones, 1.0F, -48.0F, 48.0F, direction); break;
            case 5: spectral.harmonicStretch = stepped(spectral.harmonicStretch, 0.05F, 0.25F, 4.0F, direction); break;
            case 6: spectral.spectralTiltDbPerOct = stepped(spectral.spectralTiltDbPerOct, 0.5F, -24.0F, 24.0F, direction); break;
            case 7: spectral.partialThreshold01 = stepped(spectral.partialThreshold01, 0.025F, 0.0F, 1.0F, direction); break;
            case 8: spectral.spectralBlur01 = stepped(spectral.spectralBlur01, 0.025F, 0.0F, 1.0F, direction); break;
            case 9: spectral.frequencyQuantize01 = stepped(spectral.frequencyQuantize01, 0.025F, 0.0F, 1.0F, direction); break;
            case 10: spectral.inharmonicity01 = stepped(spectral.inharmonicity01, 0.025F, 0.0F, 1.0F, direction); break;
            case 11: spectral.phaseRandom01 = stepped(spectral.phaseRandom01, 0.025F, 0.0F, 1.0F, direction); break;
            case 12: spectral.stereoSpread01 = stepped(spectral.stereoSpread01, 0.025F, 0.0F, 1.0F, direction); break;
            case 13: spectral.spectralQuality = cycle_enum(spectral.spectralQuality, 4U, direction); break;
            default: return;
        }
    } else {
        switch (index) {
            case 0: oscillator.pwmRateHertz = stepped(oscillator.pwmRateHertz, 0.1F, 0.01F, 40.0F, direction); break;
            case 1:
                if (oscillator.waveform == audio::OscillatorWaveform::Wavetable)
                    oscillator.wavetablePosition = stepped(oscillator.wavetablePosition, 0.05F, 0.0F, 1.0F, direction);
                else oscillator.shape = stepped(oscillator.shape, 0.05F, 0.0F, 1.0F, direction);
                break;
            case 2: oscillator.hardSyncSource = cycle_oscillator_source(oscillator.hardSyncSource, direction); break;
            case 3: oscillator.frequencyModSource = cycle_oscillator_source(oscillator.frequencyModSource, direction); break;
            case 4: oscillator.frequencyModMode = cycle_enum(oscillator.frequencyModMode, 3U, direction); break;
            case 5: oscillator.frequencyModAmount = stepped(oscillator.frequencyModAmount, 0.1F, -4.0F, 4.0F, direction); break;
            case 6: oscillator.ringModSource = cycle_oscillator_source(oscillator.ringModSource, direction); break;
            case 7: oscillator.ringModDepth = stepped(oscillator.ringModDepth, 0.05F, 0.0F, 1.0F, direction); break;
            case 8: oscillator.subOscillatorLevel = stepped(oscillator.subOscillatorLevel, 0.05F, 0.0F, 1.0F, direction); break;
            case 9:
                oscillator.subOscillatorOctaves = static_cast<std::uint8_t>(
                    std::clamp<int>(static_cast<int>(oscillator.subOscillatorOctaves) + direction, 1, 3));
                break;
            default: return;
        }
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_macro(std::size_t index, int direction,
                                    audio::Synthesizer& synth) noexcept {
    if (index >= audio::kSynthMacroCount) return;
    auto preset = synth.preset();
    preset.macros.values[index] = stepped(preset.macros.values[index], 0.025F, 0.0F, 1.0F, direction);
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_parameter(std::size_t index, int direction,
                                        audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    if (page_ == SynthPanelPage::FilterEnvelope) {
        switch (index) {
            case 0: preset.filter.topology = cycle_enum(preset.filter.topology, 6U, direction); break;
            case 1: preset.filter.mode = cycle_enum(preset.filter.mode, 4U, direction); break;
            case 2: preset.filter.cutoffHertz = std::clamp(preset.filter.cutoffHertz * (direction > 0 ? 1.18F : 1.0F / 1.18F), 18.0F, 22000.0F); break;
            case 3: preset.filter.resonance = stepped(preset.filter.resonance, 0.05F, 0.0F, 1.0F, direction); break;
            case 4: preset.filter.drive = stepped(preset.filter.drive, 0.15F, 0.1F, 12.0F, direction); break;
            case 5: preset.filter.envelopeAmountOctaves = stepped(preset.filter.envelopeAmountOctaves, 0.25F, -10.0F, 10.0F, direction); break;
            case 6: preset.filter.bassCompensation = stepped(preset.filter.bassCompensation, 0.05F, 0.0F, 1.0F, direction); break;
            case 7: preset.filter.morph = stepped(preset.filter.morph, 0.05F, 0.0F, 1.0F, direction); break;
            case 8: preset.filter.oversampling = cycle_oversampling(preset.filter.oversampling, direction); break;
            case 9: preset.filter.ms20HighPassCutoffHertz = std::clamp(preset.filter.ms20HighPassCutoffHertz * (direction > 0 ? 1.20F : 1.0F / 1.20F), 18.0F, 22000.0F); break;
            case 10: preset.filter.selfOscillation = stepped(preset.filter.selfOscillation, 0.05F, 0.0F, 1.5F, direction); break;
            case 11: preset.filter.keyTrack = stepped(preset.filter.keyTrack, 0.05F, 0.0F, 2.0F, direction); break;
            case 12: preset.ampEnvelope.delaySeconds = stepped(preset.ampEnvelope.delaySeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 13: preset.ampEnvelope.attackSeconds = stepped(preset.ampEnvelope.attackSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 14: preset.ampEnvelope.holdSeconds = stepped(preset.ampEnvelope.holdSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 15: preset.ampEnvelope.decaySeconds = stepped(preset.ampEnvelope.decaySeconds, 0.02F, 0.0F, 60.0F, direction); break;
            case 16: preset.ampEnvelope.sustainLevel = stepped(preset.ampEnvelope.sustainLevel, 0.05F, 0.0F, 1.0F, direction); break;
            case 17: preset.ampEnvelope.releaseSeconds = stepped(preset.ampEnvelope.releaseSeconds, 0.02F, 0.0F, 60.0F, direction); break;
            case 18: preset.filter.envelope.delaySeconds = stepped(preset.filter.envelope.delaySeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 19: preset.filter.envelope.attackSeconds = stepped(preset.filter.envelope.attackSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 20: preset.filter.envelope.holdSeconds = stepped(preset.filter.envelope.holdSeconds, 0.01F, 0.0F, 60.0F, direction); break;
            case 21: preset.filter.envelope.decaySeconds = stepped(preset.filter.envelope.decaySeconds, 0.02F, 0.0F, 60.0F, direction); break;
            case 22: preset.filter.envelope.sustainLevel = stepped(preset.filter.envelope.sustainLevel, 0.05F, 0.0F, 1.0F, direction); break;
            case 23: preset.filter.envelope.releaseSeconds = stepped(preset.filter.envelope.releaseSeconds, 0.02F, 0.0F, 60.0F, direction); break;
            case 24: preset.filter.comb.damping = stepped(preset.filter.comb.damping, 0.05F, 0.0F, 1.0F, direction); break;
            case 25: preset.filter.comb.mix = stepped(preset.filter.comb.mix, 0.05F, 0.0F, 1.0F, direction); break;
            case 26: preset.filter.comb.feedbackScale = stepped(preset.filter.comb.feedbackScale, 0.05F, 0.25F, 2.0F, direction); break;
            case 27: preset.filter.formant.dryMix = stepped(preset.filter.formant.dryMix, 0.05F, 0.0F, 1.0F, direction); break;
            default: break;
        }
    } else if (page_ == SynthPanelPage::Modulation) {
        auto adjust_lfo = [&](std::size_t lfo, std::size_t property) {
            auto& value = preset.lfos[lfo];
            switch (property) {
                case 1: value.waveform = cycle_enum(value.waveform, 6U, direction); break;
                case 2: value.rateHertz = stepped(value.rateHertz, 0.1F, 0.01F, 100.0F, direction); break;
                case 3: value.depth = stepped(value.depth, 0.05F, 0.0F, 1.0F, direction); break;
                case 4: value.phase = stepped(value.phase, 0.05F, 0.0F, 1.0F, direction); break;
                case 5: value.fadeInSeconds = stepped(value.fadeInSeconds, 0.05F, 0.0F, 20.0F, direction); break;
                case 8: value.beatsPerCycle = stepped(value.beatsPerCycle, 0.25F, 0.0625F, 32.0F, direction); break;
                default: break;
            }
        };
        if (index <= 8U) adjust_lfo(0U, index);
        else if (index <= 17U) adjust_lfo(1U, index - 9U);
        else {
            auto& slot = preset.modulation[selectedModulationSlot_];
            switch (index) {
                case 18:
                    selectedModulationSlot_ = static_cast<std::size_t>(
                        (static_cast<int>(selectedModulationSlot_) + direction +
                         static_cast<int>(audio::kSynthModulationSlotCount)) %
                        static_cast<int>(audio::kSynthModulationSlotCount));
                    break;
                case 19: slot.source = cycle_enum(slot.source, 21U, direction); break;
                case 20: slot.destination = cycle_enum(slot.destination, 42U, direction); break;
                case 21: slot.amount = stepped(slot.amount, 0.05F, -1.0F, 1.0F, direction); break;
                case 22: slot.curve = cycle_enum(slot.curve, 3U, direction); break;
                case 23: slot.smoothingMilliseconds = stepped(slot.smoothingMilliseconds, 1.0F, 0.0F, 200.0F, direction); break;
                case 24: slot.bias = stepped(slot.bias, 0.05F, -1.0F, 1.0F, direction); break;
                default: break;
            }
        }
    } else if (page_ == SynthPanelPage::Performance) {
        switch (index) {
            case 0: preset.tuning.referenceHertz = stepped(preset.tuning.referenceHertz, 0.5F, 400.0F, 480.0F, direction); break;
            case 1: preset.tuning.transposeSemitones = stepped(preset.tuning.transposeSemitones, 1.0F, -48.0F, 48.0F, direction); break;
            case 2: preset.tuning.fineCents = stepped(preset.tuning.fineCents, 1.0F, -100.0F, 100.0F, direction); break;
            case 3: preset.tuning.analogDriftCents = stepped(preset.tuning.analogDriftCents, 0.1F, 0.0F, 12.0F, direction); break;
            case 5: preset.chord.type = cycle_enum(preset.chord.type, 16U, direction); break;
            case 6: preset.chord.inversion = static_cast<std::int8_t>(std::clamp<int>(preset.chord.inversion + direction, -7, 7)); break;
            case 7: preset.chord.spreadOctaves = static_cast<std::uint8_t>(std::clamp<int>(preset.chord.spreadOctaves + direction, 0, 4)); break;
            case 8: preset.chord.scale = cycle_enum(preset.chord.scale, 7U, direction); break;
            case 9: preset.chord.scaleRoot = static_cast<std::uint8_t>((static_cast<int>(preset.chord.scaleRoot) + direction + 12) % 12); break;
            case 10: preset.chord.strumMilliseconds = stepped(preset.chord.strumMilliseconds, 2.0F, 0.0F, 250.0F, direction); break;
            case 11: preset.chord.velocityScale = stepped(preset.chord.velocityScale, 0.05F, 0.0F, 2.0F, direction); break;
            case 13: preset.arpeggiator.mode = cycle_enum(preset.arpeggiator.mode, 7U, direction); break;
            case 14: preset.arpeggiator.division = cycle_enum(preset.arpeggiator.division, 9U, direction); break;
            case 15: preset.arpeggiator.tempoBpm = stepped(preset.arpeggiator.tempoBpm, 1.0F, 20.0F, 400.0F, direction); break;
            case 16: preset.arpeggiator.clockSource = cycle_enum(preset.arpeggiator.clockSource, 3U, direction); break;
            case 17: preset.arpeggiator.externalTempoBpm = stepped(preset.arpeggiator.externalTempoBpm, 1.0F, 20.0F, 400.0F, direction); break;
            case 18: preset.arpeggiator.gate = stepped(preset.arpeggiator.gate, 0.03F, 0.02F, 1.0F, direction); break;
            case 19: preset.arpeggiator.swing = stepped(preset.arpeggiator.swing, 0.025F, 0.0F, 0.75F, direction); break;
            case 20: preset.arpeggiator.octaveRange = static_cast<std::uint8_t>(std::clamp<int>(preset.arpeggiator.octaveRange + direction, 1, 4)); break;
            case 21: preset.arpeggiator.stepCount = static_cast<std::uint8_t>(std::clamp<int>(preset.arpeggiator.stepCount + direction, 1, 16)); break;
            case 24: preset.arpeggiator.humanizeTiming = stepped(preset.arpeggiator.humanizeTiming, 0.05F, 0.0F, 1.0F, direction); break;
            case 25: preset.arpeggiator.humanizeVelocity = stepped(preset.arpeggiator.humanizeVelocity, 0.05F, 0.0F, 1.0F, direction); break;
            case 26: preset.arpeggiator.scale = cycle_enum(preset.arpeggiator.scale, 7U, direction); break;
            case 27: preset.arpeggiator.scaleRoot = static_cast<std::uint8_t>((static_cast<int>(preset.arpeggiator.scaleRoot) + direction + 12) % 12); break;
            case 28: preset.arpeggiator.phraseVelocityStart = stepped(preset.arpeggiator.phraseVelocityStart, 0.05F, 0.0F, 2.0F, direction); break;
            case 29: preset.arpeggiator.phraseVelocityEnd = stepped(preset.arpeggiator.phraseVelocityEnd, 0.05F, 0.0F, 2.0F, direction); break;
            default: break;
        }
    } else if (page_ == SynthPanelPage::Expression) {
        switch (index) {
            case 0: preset.mpe.zoneMode = cycle_enum(preset.mpe.zoneMode, 4U, direction); break;
            case 1: preset.mpe.lowerMasterChannel = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.lowerMasterChannel) + direction, 0, 15)); break;
            case 2: preset.mpe.lowerMemberCount = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.lowerMemberCount) + direction, 0, 15)); break;
            case 3: preset.mpe.upperMasterChannel = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.upperMasterChannel) + direction, 0, 15)); break;
            case 4: preset.mpe.upperMemberCount = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.upperMemberCount) + direction, 0, 15)); break;
            case 5: preset.mpe.masterPitchBendRangeSemitones = stepped(preset.mpe.masterPitchBendRangeSemitones, 1.0F, 0.0F, 96.0F, direction); break;
            case 6: preset.mpe.memberPitchBendRangeSemitones = stepped(preset.mpe.memberPitchBendRangeSemitones, 1.0F, 0.0F, 96.0F, direction); break;
            case 7: preset.mpe.timbreController = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.mpe.timbreController) + direction, 0, 127)); break;
            case 10: preset.microtuning.referenceHertz = stepped(preset.microtuning.referenceHertz, 0.5F, 300.0F, 600.0F, direction); break;
            case 11: preset.microtuning.referenceNote = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.microtuning.referenceNote) + direction, 0, 127)); break;
            case 13: preset.unison.voices = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(preset.unison.voices) + direction, 1, static_cast<int>(audio::kSynthUnisonMax))); break;
            case 14: preset.unison.detuneCents = stepped(preset.unison.detuneCents, 0.5F, 0.0F, 100.0F, direction); break;
            case 15: preset.unison.stereoSpread = stepped(preset.unison.stereoSpread, 0.05F, 0.0F, 1.0F, direction); break;
            case 16: preset.unison.phaseSpread = stepped(preset.unison.phaseSpread, 0.025F, 0.0F, 1.0F, direction); break;
            case 18: preset.oscillatorQuality = cycle_enum(preset.oscillatorQuality, 3U, direction); break;
            case 19: preset.filterQuality = cycle_enum(preset.filterQuality, 4U, direction); break;
            case 20:
                selectedModulationSlot_ = static_cast<std::size_t>(
                    (static_cast<int>(selectedModulationSlot_) + direction +
                     static_cast<int>(audio::kSynthModulationSlotCount)) %
                    static_cast<int>(audio::kSynthModulationSlotCount));
                break;
            case 21: preset.modulation[selectedModulationSlot_].polarity =
                cycle_enum(preset.modulation[selectedModulationSlot_].polarity, 2U, direction); break;
            case 22: preset.modulation[selectedModulationSlot_].smoothingMilliseconds =
                stepped(preset.modulation[selectedModulationSlot_].smoothingMilliseconds, 1.0F, 0.0F, 500.0F, direction); break;
            default: break;
        }
    } else if (page_ == SynthPanelPage::Generative) {
        auto& seq = preset.sequencer;
        switch (index) {
            case 1: case 2: case 3: case 4: case 5: case 6: case 7: {
                auto& lane = seq.lanes[static_cast<std::size_t>(index - 1)];
                lane.stepCount = static_cast<std::uint8_t>(
                    std::clamp<int>(lane.stepCount + direction, 2, 64));
                break;
            }
            case 8:
                selectedSequencerLane_ = static_cast<std::size_t>(
                    (static_cast<int>(selectedSequencerLane_) + direction +
                     static_cast<int>(audio::kSequencerLaneCount)) %
                    static_cast<int>(audio::kSequencerLaneCount));
                break;
            case 9:
                seq.lanes[selectedSequencerLane_].direction =
                    cycle_enum(seq.lanes[selectedSequencerLane_].direction, 4U, direction);
                break;
            case 10: seq.scale = cycle_enum(seq.scale, 6U, direction); break;
            case 11:
                seq.rootNote = static_cast<std::uint8_t>(
                    std::clamp<int>(seq.rootNote + direction, 0, 127));
                break;
            case 12:
                seq.octaveRange = static_cast<std::uint8_t>(
                    std::clamp<int>(seq.octaveRange + direction, 0, 8));
                break;
            case 13:
                seq.randomSeed = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(seq.randomSeed) + direction);
                break;
            case 14:
                preset.genetics.mutationIntensity =
                    stepped(preset.genetics.mutationIntensity, 0.05F, 0.0F, 1.0F, direction);
                break;
            case 15:
                preset.genetics.mutationSeed = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(preset.genetics.mutationSeed) + direction);
                break;
            case 27:
                preset.attractor.config.bpm =
                    stepped(static_cast<float>(preset.attractor.config.bpm), 1.0F, 20.0F, 400.0F, direction);
                break;
            default: break;
        }
    } else if (page_ == SynthPanelPage::Presets) {
        auto& mapping = preset.midiLearn[selectedMidiLearnMapping_];
        switch (index) {
            case 0:
                selectedMidiLearnMapping_ = static_cast<std::size_t>(
                    (static_cast<int>(selectedMidiLearnMapping_) + direction +
                     static_cast<int>(audio::kSynthMidiLearnCount)) %
                    static_cast<int>(audio::kSynthMidiLearnCount));
                break;
            case 2: mapping.controller = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(mapping.controller) + direction, 0, 127)); break;
            case 3: mapping.macroIndex = static_cast<std::uint8_t>((static_cast<int>(mapping.macroIndex) + direction + static_cast<int>(audio::kSynthMacroCount)) % static_cast<int>(audio::kSynthMacroCount)); break;
            case 4: mapping.minimum = stepped(mapping.minimum, 0.05F, 0.0F, 1.0F, direction); break;
            case 5: mapping.maximum = stepped(mapping.maximum, 0.05F, 0.0F, 1.0F, direction); break;
            default: break;
        }
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::toggle_parameter(std::size_t index, audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    if (page_ == SynthPanelPage::FilterEnvelope) {
        if (index == 0U) preset.filter.enabled = !preset.filter.enabled;
        else if (index == 1U) preset.filter.alternateRevision = !preset.filter.alternateRevision;
    } else if (page_ == SynthPanelPage::Modulation) {
        if (index == 0U) preset.lfos[0].enabled = !preset.lfos[0].enabled;
        else if (index == 6U) preset.lfos[0].keySync = !preset.lfos[0].keySync;
        else if (index == 7U) preset.lfos[0].tempoSync = !preset.lfos[0].tempoSync;
        else if (index == 9U) preset.lfos[1].enabled = !preset.lfos[1].enabled;
        else if (index == 15U) preset.lfos[1].keySync = !preset.lfos[1].keySync;
        else if (index == 16U) preset.lfos[1].tempoSync = !preset.lfos[1].tempoSync;
        else if (index == 23U) {
            auto& slot = preset.modulation[selectedModulationSlot_];
            slot.enabled = !slot.enabled;
        }
    } else if (page_ == SynthPanelPage::Performance) {
        if (index == 4U) preset.chord.enabled = !preset.chord.enabled;
        else if (index == 12U) preset.arpeggiator.enabled = !preset.arpeggiator.enabled;
        else if (index == 22U) preset.arpeggiator.latch = !preset.arpeggiator.latch;
        else if (index == 23U) preset.arpeggiator.retriggerEnvelopes = !preset.arpeggiator.retriggerEnvelopes;
    } else if (page_ == SynthPanelPage::Expression) {
        if (index == 8U) preset.mpe.masterSustainToMembers = !preset.mpe.masterSustainToMembers;
        else if (index == 9U) preset.microtuning.enabled = !preset.microtuning.enabled;
        else if (index == 12U) preset.unison.enabled = !preset.unison.enabled;
        else if (index == 17U) preset.unison.preserveLevel = !preset.unison.preserveLevel;
        else if (index == 23U) preset.metadata.favorite = !preset.metadata.favorite;
    } else if (page_ == SynthPanelPage::Generative) {
        if (index == 0U) {
            preset.sequencer.enabled = !preset.sequencer.enabled;
        } else if (index == 16U) {
            // Mutate: evolve the current patch with its own genetics settings.
            preset = audio::mutate_preset(preset, preset.genetics.mutationIntensity,
                                          preset.genetics.mutationSeed,
                                          preset.genetics.lockedGroups);
            presetStatus_ = "Mutated preset";
        } else if (index == 17U) {
            // Breed: child of the current patch (A) and the captured morph-B
            // preset (B); falls back to a self-cross when B was never captured.
            const audio::SynthPreset& parentB = compareB_ ? *compareB_ : preset;
            preset = audio::breed_presets(preset, parentB, 0U, preset.genetics.mutationSeed);
            presetStatus_ = compareB_ ? "Bred with captured B" : "Bred with self (no B captured)";
        } else if (index >= 18U && index <= 25U) {
            preset.genetics.lockedGroups ^= audio::gene_group_bit(
                static_cast<audio::GeneGroup>(index - 18U));
        } else if (index == 26U) {
            preset.attractor.enabled = !preset.attractor.enabled;
        }
    } else if (page_ == SynthPanelPage::Presets) {
        auto& mapping = preset.midiLearn[selectedMidiLearnMapping_];
        if (index == 1U) mapping.enabled = !mapping.enabled;
        else if (index == 6U) mapping.inverted = !mapping.inverted;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::adjust_arpeggiator_step_parameter(std::size_t index, int direction,
                                                          audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    auto& step = preset.arpeggiator.steps[selectedArpeggiatorStep_];
    switch (index) {
        case 1: step.condition = cycle_enum(step.condition, 7U, direction); break;
        case 2: step.conditionA = static_cast<std::uint8_t>(std::clamp<int>(step.conditionA + direction, 1, 8)); break;
        case 3: step.conditionB = static_cast<std::uint8_t>(std::clamp<int>(step.conditionB + direction, 1, 8)); break;
        case 4: step.automationCurve = cycle_enum(step.automationCurve, 3U, direction); break;
        case 7: step.transpose = static_cast<std::int8_t>(std::clamp<int>(step.transpose + direction, -48, 48)); break;
        case 8: step.octaveOffset = static_cast<std::int8_t>(std::clamp<int>(step.octaveOffset + direction, -4, 4)); break;
        case 9: step.velocityScale = stepped(step.velocityScale, 0.05F, 0.0F, 2.0F, direction); break;
        case 10: step.gateScale = stepped(step.gateScale, 0.05F, 0.1F, 2.0F, direction); break;
        case 11: step.probability = stepped(step.probability, 0.05F, 0.0F, 1.0F, direction); break;
        case 12: step.ratchets = static_cast<std::uint8_t>(std::clamp<int>(step.ratchets + direction, 1, 8)); break;
        case 14: step.macro1 = stepped(step.macro1, 0.05F, -1.0F, 1.0F, direction); break;
        case 15: step.macro2 = stepped(step.macro2, 0.05F, -1.0F, 1.0F, direction); break;
        case 16: step.macro3 = stepped(step.macro3, 0.05F, -1.0F, 1.0F, direction); break;
        case 17: step.macro4 = stepped(step.macro4, 0.05F, -1.0F, 1.0F, direction); break;
        default: break;
    }
    synth.set_preset(preset);
}

void EditorSynthPanel::toggle_arpeggiator_step_parameter(std::size_t index,
                                                          audio::Synthesizer& synth) noexcept {
    auto preset = synth.preset();
    auto& step = preset.arpeggiator.steps[selectedArpeggiatorStep_];
    if (index == 0U) step.enabled = !step.enabled;
    else if (index == 5U) step.accent = !step.accent;
    else if (index == 6U) step.slide = !step.slide;
    else if (index == 13U) step.tie = !step.tie;
    else return;
    synth.set_preset(preset);
}

void EditorSynthPanel::load_selected_preset(audio::Synthesizer& synth) noexcept {
    try {
        if (selectedPresetEntry_ >= presetLibrary_.entries().size()) {
            presetStatus_ = "No preset selected";
            return;
        }
        std::string error;
        auto preset = audio::SynthPreset::load(presetLibrary_.entries()[selectedPresetEntry_].path, &error);
        if (!preset) {
            presetStatus_ = error.empty() ? "Preset load failed" : error;
            return;
        }
        synth.set_preset(*preset);
        presetStatus_ = "Loaded " + preset->name;
    } catch (...) {
        presetStatus_ = "Preset load failed";
    }
}

void EditorSynthPanel::apply_preset_morph(audio::Synthesizer& synth) noexcept {
    if (!compareA_ || !compareB_) {
        presetStatus_ = "Capture A and B before morphing";
        return;
    }
    try {
        synth.set_preset(audio::morph_synth_presets(*compareA_, *compareB_, presetMorphAmount_));
        presetStatus_ = "Applied A/B morph";
    } catch (...) {
        presetStatus_ = "Preset morph failed";
    }
}

void EditorSynthPanel::draw_wavetable_point(int x, int y, audio::Synthesizer& synth) noexcept {
    if (!layout_.wavetableCanvas.contains(x, y)) return;
    if (!wavetableDraft_) wavetableDraft_ = synth.preset();
    auto& preset = *wavetableDraft_;
    preset.wavetable.enabled = true;
    preset.wavetable.frameCount = std::max<std::uint8_t>(preset.wavetable.frameCount,
        static_cast<std::uint8_t>(selectedWavetableFrame_ + 1U));
    const float normalizedX = static_cast<float>(x - layout_.wavetableCanvas.x) /
                              static_cast<float>(std::max(1, layout_.wavetableCanvas.width - 1));
    const int sample = std::clamp(static_cast<int>(std::lround(normalizedX *
        static_cast<float>(audio::kWavetableSampleCount - 1U))), 0,
        static_cast<int>(audio::kWavetableSampleCount - 1U));
    const float value = std::clamp(1.0F - 2.0F * static_cast<float>(y - layout_.wavetableCanvas.y) /
                                   static_cast<float>(std::max(1, layout_.wavetableCanvas.height - 1)), -1.0F, 1.0F);
    auto& samples = preset.wavetable.samples;
    const std::size_t base = selectedWavetableFrame_ * audio::kWavetableSampleCount;
    if (wavetableLastSample_ >= 0 && wavetableLastSample_ != sample) {
        const int begin = std::min(wavetableLastSample_, sample);
        const int end = std::max(wavetableLastSample_, sample);
        for (int i = begin; i <= end; ++i) {
            const float t = sample == wavetableLastSample_ ? 1.0F :
                static_cast<float>(i - wavetableLastSample_) / static_cast<float>(sample - wavetableLastSample_);
            samples[base + static_cast<std::size_t>(i)] = std::clamp(
                wavetableLastValue_ + (value - wavetableLastValue_) * t, -1.0F, 1.0F);
        }
    } else samples[base + static_cast<std::size_t>(sample)] = value;
    wavetableLastSample_ = sample;
    wavetableLastValue_ = value;
    wavetableDraftDirty_ = true;
    const auto now = std::chrono::steady_clock::now();
    if (wavetableDrawPublishes_ == 0U || now - wavetableLastPublish_ >= kWavetableDrawPublishInterval)
        flush_wavetable_draft(synth);
}

void EditorSynthPanel::flush_wavetable_draft_if_due(audio::Synthesizer& synth) noexcept {
    if (wavetableDraftDirty_ &&
        std::chrono::steady_clock::now() - wavetableLastPublish_ >= kWavetableDrawPublishInterval)
        flush_wavetable_draft(synth);
}

void EditorSynthPanel::flush_wavetable_draft(audio::Synthesizer& synth) noexcept {
    if (!wavetableDraftDirty_ || !wavetableDraft_) return;
    synth.set_preset(*wavetableDraft_);
    wavetableDraftDirty_ = false;
    wavetableLastPublish_ = std::chrono::steady_clock::now();
    ++wavetableDrawPublishes_;
}

bool EditorSynthPanel::pointer_move(int x, int y, audio::Synthesizer& synth) noexcept {
    if (open_ && keyboard_.dragging()) { (void)keyboard_.pointer_drag(x); return true; }
    if (!open_ || !wavetableDrawing_) return false;
    draw_wavetable_point(x, y, synth);
    return true;
}

bool EditorSynthPanel::pointer_down(int x, int y, audio::Synthesizer& synth) noexcept {
    if (!open_ || !contains(layout_.panel, x, y)) return false;
    if (contains(layout_.closeButton, x, y)) { set_open(false, synth); return true; }
    // Phase 6: the search browser is an overlay — it gets first refusal.
    if (searchPanel_.open() && searchPanel_.pointer_down(x, y, synth)) return true;
    if (contains(layout_.searchButton, x, y)) {
        searchPanel_.toggle();
        // Re-run layout so the overlay gets positioned on open.
        resize(lastWidth_, lastHeight_, lastUiScale_);
        return true;
    }
    if (contains(layout_.panicButton, x, y)) { synth.all_notes_off(true); release_panel_notes(synth); return true; }
    if (contains(layout_.resetButton, x, y)) { synth.set_preset(audio::SynthPreset::make_default()); return true; }
    if (contains(layout_.octaveDownButton, x, y)) {
        (void)keyboard_.shift_octave(-1); keyboard_.ensure_visible(keyboard_.computer_key_base()); return true;
    }
    if (contains(layout_.octaveUpButton, x, y)) {
        (void)keyboard_.shift_octave(1); keyboard_.ensure_visible(keyboard_.computer_key_base()); return true;
    }
    if (contains(layout_.keyboardKeysButton, x, y)) {
        // Cycle 25 -> 37 -> ... -> 88 -> 25; the controller persists the choice.
        std::size_t next = 0;
        for (std::size_t i = 0; i < kPianoKeyboardSizes.size(); ++i)
            if (kPianoKeyboardSizes[i] == keyboard_.key_count()) next = (i + 1U) % kPianoKeyboardSizes.size();
        release_panel_notes(synth);
        set_keyboard_key_count(kPianoKeyboardSizes[next]);
        requestedKeyCount_ = keyboard_.key_count();
        return true;
    }
    if (contains(layout_.midiInputButton, x, y)) { midiPortCycleRequest_ = 1; return true; }
    if (contains(layout_.midiThruButton, x, y)) {
        auto preset = synth.preset(); preset.midiThru = !preset.midiThru; synth.set_preset(preset); return true;
    }
    for (std::size_t i = 0; i < layout_.tabButtons.size(); ++i) {
        if (contains(layout_.tabButtons[i], x, y)) { set_page(static_cast<SynthPanelPage>(i)); return true; }
    }

    if (page_ == SynthPanelPage::Oscillators) {
        const auto waveform = synth.preset().oscillators[selectedOscillator_].waveform;
        if (waveform == audio::OscillatorWaveform::Wavetable) {
            if (layout_.wavetableCanvas.contains(x, y)) {
                wavetableDrawing_ = true; wavetableLastSample_ = -1; wavetableDraft_.reset();
                draw_wavetable_point(x, y, synth); return true;
            }
            for (std::size_t i = 0; i < layout_.wavetableFrameButtons.size(); ++i)
                if (layout_.wavetableFrameButtons[i].contains(x, y)) { selectedWavetableFrame_ = i; return true; }
            if (layout_.wavetableNormalizeButton.contains(x, y)) { auto preset=synth.preset(); (void)audio::wavetable_normalize(preset.wavetable); synth.set_preset(preset); return true; }
            if (layout_.wavetableRemoveDcButton.contains(x, y)) { auto preset=synth.preset(); (void)audio::wavetable_remove_dc(preset.wavetable); synth.set_preset(preset); return true; }
            if (layout_.wavetableAlignButton.contains(x, y)) { auto preset=synth.preset(); (void)audio::wavetable_align_phases(preset.wavetable); synth.set_preset(preset); return true; }
        }
        for (std::size_t i = 0; i < layout_.oscillatorRows.size(); ++i) {
            if (!contains(layout_.oscillatorRows[i], x, y)) continue;
            selectedOscillator_ = i;
            auto preset = synth.preset(); auto& oscillator = preset.oscillators[i];
            if (contains(layout_.oscillatorEnableButtons[i], x, y)) oscillator.enabled = !oscillator.enabled;
            else if (contains(layout_.oscillatorWaveButtons[i], x, y)) { cycle_waveform(i, 1, synth); return true; }
            else if (contains(layout_.oscillatorGainDownButtons[i], x, y)) oscillator.gain = stepped(oscillator.gain, 0.025F, 0.0F, 2.0F, -1);
            else if (contains(layout_.oscillatorGainUpButtons[i], x, y)) oscillator.gain = stepped(oscillator.gain, 0.025F, 0.0F, 2.0F, 1);
            else if (contains(layout_.oscillatorTuneDownButtons[i], x, y)) oscillator.semitones = stepped(oscillator.semitones, 1.0F, -96.0F, 96.0F, -1);
            else if (contains(layout_.oscillatorTuneUpButtons[i], x, y)) oscillator.semitones = stepped(oscillator.semitones, 1.0F, -96.0F, 96.0F, 1);
            else if (contains(layout_.oscillatorFineDownButtons[i], x, y)) oscillator.cents = stepped(oscillator.cents, 1.0F, -100.0F, 100.0F, -1);
            else if (contains(layout_.oscillatorFineUpButtons[i], x, y)) oscillator.cents = stepped(oscillator.cents, 1.0F, -100.0F, 100.0F, 1);
            else if (contains(layout_.oscillatorPwmDownButtons[i], x, y)) oscillator.pwmDepth = stepped(oscillator.pwmDepth, 0.05F, 0.0F, 1.0F, -1);
            else if (contains(layout_.oscillatorPwmUpButtons[i], x, y)) oscillator.pwmDepth = stepped(oscillator.pwmDepth, 0.05F, 0.0F, 1.0F, 1);
            synth.set_preset(preset); return true;
        }
        for (std::size_t i = 0; i < layout_.oscillatorAdvancedRows.size(); ++i) {
            if (!contains(layout_.oscillatorAdvancedRows[i], x, y)) continue;
            if (contains(layout_.oscillatorAdvancedDownButtons[i], x, y)) adjust_oscillator_advanced(i, -1, synth);
            else if (contains(layout_.oscillatorAdvancedUpButtons[i], x, y)) adjust_oscillator_advanced(i, 1, synth);
            return true;
        }
    } else if (page_ == SynthPanelPage::Effects) {
        for (std::size_t i = 0; i < layout_.effectToggleButtons.size(); ++i)
            if (contains(layout_.effectToggleButtons[i], x, y)) { toggle_effect(i, synth); return true; }
        for (std::size_t i = 0; i < layout_.effectRows.size(); ++i)
            if (contains(layout_.effectRows[i], x, y)) { selectedEffect_ = i; return true; }
        for (std::size_t i = 0; i < layout_.effectParamRows.size(); ++i) {
            if (!contains(layout_.effectParamRows[i], x, y)) continue;
            if (contains(layout_.effectParamDownButtons[i], x, y)) adjust_effect_param(i, -1, synth);
            else if (contains(layout_.effectParamUpButtons[i], x, y)) adjust_effect_param(i, 1, synth);
            else if (contains(layout_.effectParamToggleButtons[i], x, y)) toggle_effect_param(i, synth);
            return true;
        }
    } else if (page_ == SynthPanelPage::Presets) {
        if (contains(layout_.presetScanButton, x, y)) { (void)refresh_preset_library(); return true; }
        if (contains(layout_.presetPreviousButton, x, y)) {
            if (!presetLibrary_.entries().empty()) selectedPresetEntry_ =
                (selectedPresetEntry_ + presetLibrary_.entries().size() - 1U) % presetLibrary_.entries().size();
            return true;
        }
        if (contains(layout_.presetNextButton, x, y)) {
            if (!presetLibrary_.entries().empty()) selectedPresetEntry_ =
                (selectedPresetEntry_ + 1U) % presetLibrary_.entries().size();
            return true;
        }
        if (contains(layout_.presetLoadButton, x, y)) { load_selected_preset(synth); return true; }
        if (contains(layout_.presetCaptureAButton, x, y)) { compareA_ = synth.preset(); presetStatus_ = "Captured A"; return true; }
        if (contains(layout_.presetCaptureBButton, x, y)) { compareB_ = synth.preset(); presetStatus_ = "Captured B"; return true; }
        if (contains(layout_.presetMorphDownButton, x, y)) { presetMorphAmount_ = stepped(presetMorphAmount_, 0.05F, 0.0F, 1.0F, -1); apply_preset_morph(synth); return true; }
        if (contains(layout_.presetMorphUpButton, x, y)) { presetMorphAmount_ = stepped(presetMorphAmount_, 0.05F, 0.0F, 1.0F, 1); apply_preset_morph(synth); return true; }
        for (std::size_t i = 0; i < layout_.presetEntryButtons.size(); ++i) {
            if (contains(layout_.presetEntryButtons[i], x, y) && i < presetLibrary_.entries().size()) {
                selectedPresetEntry_ = i;
                return true;
            }
        }
        for (std::size_t i = 0; i < 7U; ++i) {
            if (!contains(layout_.parameterRows[i], x, y)) continue;
            if (contains(layout_.parameterDownButtons[i], x, y)) adjust_parameter(i, -1, synth);
            else if (contains(layout_.parameterUpButtons[i], x, y)) adjust_parameter(i, 1, synth);
            else if (contains(layout_.parameterToggleButtons[i], x, y)) toggle_parameter(i, synth);
            return true;
        }
    } else {
        for (std::size_t i = 0; i < layout_.parameterRows.size(); ++i) {
            if (!contains(layout_.parameterRows[i], x, y)) continue;
            if (contains(layout_.parameterDownButtons[i], x, y)) adjust_parameter(i, -1, synth);
            else if (contains(layout_.parameterUpButtons[i], x, y)) adjust_parameter(i, 1, synth);
            else if (contains(layout_.parameterToggleButtons[i], x, y)) toggle_parameter(i, synth);
            return true;
        }
        if (page_ == SynthPanelPage::Modulation) {
            for (std::size_t i = 0; i < layout_.macroRows.size(); ++i) {
                if (!contains(layout_.macroRows[i], x, y)) continue;
                if (contains(layout_.macroDownButtons[i], x, y)) adjust_macro(i, -1, synth);
                else if (contains(layout_.macroUpButtons[i], x, y)) adjust_macro(i, 1, synth);
                return true;
            }
        }
        if (page_ == SynthPanelPage::Performance) {
            for (std::size_t i = 0; i < layout_.arpeggiatorStepButtons.size(); ++i) {
                if (contains(layout_.arpeggiatorStepButtons[i], x, y)) { selectedArpeggiatorStep_ = i; return true; }
            }
            for (std::size_t i = 0; i < layout_.arpeggiatorStepRows.size(); ++i) {
                if (!contains(layout_.arpeggiatorStepRows[i], x, y)) continue;
                if (contains(layout_.arpeggiatorStepDownButtons[i], x, y))
                    adjust_arpeggiator_step_parameter(i, -1, synth);
                else if (contains(layout_.arpeggiatorStepUpButtons[i], x, y))
                    adjust_arpeggiator_step_parameter(i, 1, synth);
                else if (contains(layout_.arpeggiatorStepToggleButtons[i], x, y))
                    toggle_arpeggiator_step_parameter(i, synth);
                return true;
            }
        }
    }

    if (grid_scrollbar_press(x, y)) return true;
    if (keyboard_.pointer_down(x, y)) return true;
    if (const auto note = keyboard_.note_at(x, y)) {
        if (pointerNote_ >= 0) (void)synth.note_off(static_cast<std::uint8_t>(pointerNote_));
        pointerNote_ = std::clamp(*note, 0, 127);
        (void)synth.note_on(static_cast<std::uint8_t>(pointerNote_), 0.85F);
        return true;
    }
    return true;
}

bool EditorSynthPanel::pointer_up(int x, int y, audio::Synthesizer& synth) noexcept {
    if (!open_) return false;
    if (keyboard_.dragging()) { keyboard_.pointer_up(); return true; }
    if (searchPanel_.open() && searchPanel_.pointer_up(x, y, synth)) return true;
    if (wavetableDrawing_) {
        flush_wavetable_draft(synth);  // the final stroke state always lands
        wavetableDrawing_ = false; wavetableLastSample_ = -1; wavetableDraft_.reset();
        return true;
    }
    if (pointerNote_ < 0) return false;
    (void)synth.note_off(static_cast<std::uint8_t>(pointerNote_));
    pointerNote_ = -1;
    return true;
}

int EditorSynthPanel::keyboard_key_index(std::string_view key) noexcept {
    static constexpr std::array<std::string_view, 20> keys{
        "z","s","x","d","c","v","g","b","h","n","j","m",",","l",".",";","/","q","2","w"};
    const std::string normalized = lower(key);
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (normalized == keys[i]) return static_cast<int>(i);
    return -1;
}

int EditorSynthPanel::keyboard_note(std::string_view key, int base) noexcept {
    const int index = keyboard_key_index(key);
    return index < 0 ? -1 : std::clamp(base + index, 0, 127);
}

bool EditorSynthPanel::key_down(std::string_view key, audio::Synthesizer& synth) noexcept {
    if (!open_) return false;
    const std::string normalized = lower(key);
    if (normalized == "escape") { set_open(false, synth); return true; }
    if (normalized == "left" || normalized == "right") {
        (void)keyboard_.shift_octave(normalized == "left" ? -1 : 1);
        keyboard_.ensure_visible(keyboard_.computer_key_base());
        return true;
    }
    static constexpr std::array<std::string_view, kSynthPanelPageCount> pageKeys{"1","3","4","5","6","7","8","9"};
    for (std::size_t i = 0; i < pageKeys.size(); ++i) {
        if (normalized == pageKeys[i] || normalized == "f" + std::to_string(i + 1U)) {
            set_page(static_cast<SynthPanelPage>(i));
            return true;
        }
    }
    const int keyIndex = keyboard_key_index(normalized);
    const int note = keyboard_note(normalized, keyboard_.computer_key_base());
    if (note < 0) return false;
    computerKeyNotes_[static_cast<std::size_t>(keyIndex)] = note;
    keyboard_.ensure_visible(note);  // follow the played note on a scrolled piano
    if (!keyboardNotes_[static_cast<std::size_t>(note)]) {
        keyboardNotes_[static_cast<std::size_t>(note)] = true;
        (void)synth.note_on(static_cast<std::uint8_t>(note), 0.82F);
    }
    return true;
}

bool EditorSynthPanel::key_up(std::string_view key, audio::Synthesizer& synth) noexcept {
    if (!open_) return false;
    const int keyIndex = keyboard_key_index(key);
    if (keyIndex < 0) return false;
    // Release the note this key started, even if the octave changed meanwhile.
    int note = computerKeyNotes_[static_cast<std::size_t>(keyIndex)];
    if (note < 0) note = keyboard_note(key, keyboard_.computer_key_base());
    computerKeyNotes_[static_cast<std::size_t>(keyIndex)] = -1;
    if (keyboardNotes_[static_cast<std::size_t>(note)]) {
        keyboardNotes_[static_cast<std::size_t>(note)] = false;
        (void)synth.note_off(static_cast<std::uint8_t>(note));
    }
    return true;
}

} // namespace dve::editor
