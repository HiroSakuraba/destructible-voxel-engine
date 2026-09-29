#pragma once

// On-screen piano keyboard shared by the synth and chiptune editors.
//
// The keyboard has an adjustable key count (25, 37, 49, 61, 76 or 88 keys,
// User-scope setting `editor.keyboard_keys`). It lays out a real piano:
// white keys side by side, and narrower, shorter black keys on top of them.
// Hit-testing checks black keys first.
//
// Range:
//   * 88 keys always span A0-C8 (MIDI 21-108).
//   * Every other size starts on a C. The first note is (octave + 1) * 12
//     (scientific pitch, C4 = MIDI 60), and the octave is clamped so the whole
//     range stays inside MIDI 21-108.
// The computer keyboard ("z" = C) maps from computer_key_base(), which is
// (octave + 1) * 12 for every size (for 88 keys the octave only moves the
// computer-key window and not the drawn range).
//
// When the area is too narrow for kPianoMinWhiteKeyWidth logical px per
// white key, the keys keep that width and scroll horizontally. A scrollbar
// strip is reserved at the bottom of the area, and the mouse wheel and
// ensure_visible() also scroll it.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/editor_viewport.hpp"

namespace dve::editor {

inline constexpr std::array<int, 6> kPianoKeyboardSizes{25, 37, 49, 61, 76, 88};
inline constexpr int kPianoDefaultKeyCount = 25;
inline constexpr int kPianoLowestMidi = 21;   // A0
inline constexpr int kPianoHighestMidi = 108; // C8
inline constexpr int kPianoMinWhiteKeyWidth = 12;
inline constexpr int kPianoScrollbarHeight = 10;
inline constexpr std::string_view kKeyboardKeysSettingId = "editor.keyboard_keys";

// Nearest supported key count (ties go to the smaller size).
[[nodiscard]] int snap_piano_key_count(int count) noexcept;
[[nodiscard]] bool is_black_key(int midi) noexcept;
// Scientific note name: 60 -> "C4", 21 -> "A0".
[[nodiscard]] std::string piano_note_name(int midi);

struct PianoKey {
    int midi{};
    bool black{};
    UiRect rect{};  // screen rect, clipped to the visible key area
};

class PianoKeyboard {
public:
    PianoKeyboard() noexcept;

    void set_key_count(int count) noexcept;
    [[nodiscard]] int key_count() const noexcept { return keyCount_; }

    void set_octave(int octave) noexcept;
    // Returns true when the octave changed.
    bool shift_octave(int delta) noexcept;
    [[nodiscard]] int octave() const noexcept { return octave_; }
    [[nodiscard]] int min_octave() const noexcept;
    [[nodiscard]] int max_octave() const noexcept;

    [[nodiscard]] int first_note() const noexcept;
    [[nodiscard]] int last_note() const noexcept;
    [[nodiscard]] int computer_key_base() const noexcept { return (octave_ + 1) * 12; }
    [[nodiscard]] int white_key_count() const noexcept;

    void layout(UiRect area) noexcept;
    [[nodiscard]] UiRect area() const noexcept { return area_; }
    [[nodiscard]] UiRect keys_area() const noexcept;
    [[nodiscard]] int white_key_width() const noexcept { return whiteWidth_; }
    [[nodiscard]] int black_key_width() const noexcept { return blackWidth_; }
    [[nodiscard]] int black_key_height() const noexcept;

    [[nodiscard]] bool scrollable() const noexcept { return scrollable_; }
    [[nodiscard]] int content_width() const noexcept { return contentWidth_; }
    [[nodiscard]] int scroll_offset() const noexcept { return scroll_; }
    [[nodiscard]] int max_scroll() const noexcept;
    void set_scroll_offset(int offset) noexcept;
    [[nodiscard]] UiRect scrollbar_track() const noexcept;
    [[nodiscard]] UiRect scrollbar_thumb() const noexcept;
    // Scrolls by two white keys per wheel step. Returns true when it moved.
    bool wheel(float steps) noexcept;
    // Scrolls just enough to show the whole key (no-op when unscrollable or
    // outside the range).
    void ensure_visible(int midi) noexcept;

    // Screen-space hit test. Black keys win over the white keys underneath.
    [[nodiscard]] std::optional<int> note_at(int x, int y) const noexcept;
    // Unclipped screen rect of a key, or nullopt when the note is outside the range.
    [[nodiscard]] std::optional<UiRect> key_rect(int midi) const noexcept;
    // Visible keys in draw order: white keys first, then black keys.
    [[nodiscard]] std::vector<PianoKey> visible_keys() const;

    // Scrollbar interaction. pointer_down returns true when it consumed the
    // press (thumb drag starts, a press on the track pages).
    bool pointer_down(int x, int y) noexcept;
    bool pointer_drag(int x) noexcept;
    void pointer_up() noexcept { dragging_ = false; }
    [[nodiscard]] bool dragging() const noexcept { return dragging_; }

private:
    [[nodiscard]] int white_index(int midi) const noexcept;   // whites before midi
    [[nodiscard]] int white_left(int index) const noexcept;   // content x of white key `index`
    [[nodiscard]] UiRect content_rect(int midi) const noexcept;
    void clamp_octave() noexcept;
    void clamp_scroll() noexcept;

    int keyCount_{kPianoDefaultKeyCount};
    int octave_{4};
    UiRect area_{};
    int whiteWidth_{kPianoMinWhiteKeyWidth};
    int blackWidth_{7};
    int contentWidth_{0};
    int availableWidth_{0};
    bool scrollable_{false};
    int scroll_{0};
    bool dragging_{false};
    int dragAnchorX_{0};
    int dragAnchorScroll_{0};
};

}  // namespace dve::editor
