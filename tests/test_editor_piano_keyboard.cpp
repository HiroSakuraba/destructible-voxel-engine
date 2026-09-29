// On-screen piano keyboard: key -> MIDI mapping for every size, black/white
// hit-testing (including the edges), octave clamping, horizontal scrolling,
// the synth/chiptune integration (computer keys, follow-the-played-note) and
// User-scope persistence of `editor.keyboard_keys`.
#include "dve/editor_native.hpp"
#include "dve/editor_piano_keyboard.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
std::string n(int value) { return std::to_string(value); }

int expected_white_count(int first, int last) {
    int count = 0;
    for (int note = first; note <= last; ++note) count += is_black_key(note) ? 0 : 1;
    return count;
}

void test_basics() {
    check(snap_piano_key_count(24) == 25 && snap_piano_key_count(88) == 88 && snap_piano_key_count(1000) == 88 &&
          snap_piano_key_count(0) == 25 && snap_piano_key_count(61) == 61 && snap_piano_key_count(70) == 76,
          "snap_piano_key_count");
    check(piano_note_name(60) == "C4" && piano_note_name(21) == "A0" && piano_note_name(108) == "C8" &&
          piano_note_name(61) == "C#4", "scientific note names");
    int blacks = 0;
    for (int note = 0; note < 12; ++note) blacks += is_black_key(note) ? 1 : 0;
    check(blacks == 5 && is_black_key(61) && !is_black_key(64) && !is_black_key(65), "black-key pattern");
    PianoKeyboard keyboard;
    check(keyboard.key_count() == kPianoDefaultKeyCount && keyboard.first_note() == 60 && keyboard.last_note() == 84,
          "default keyboard is 25 keys C4-C6");
}

// Every key of every size maps to the expected MIDI note, and each key's own
// rect hit-tests back to it.
void test_mapping_every_size() {
    for (const int size : kPianoKeyboardSizes) {
        PianoKeyboard keyboard;
        keyboard.set_key_count(size);
        for (int octave = keyboard.min_octave(); octave <= keyboard.max_octave(); ++octave) {
            keyboard.set_octave(octave);
            const std::string tag = n(size) + " keys oct " + n(octave) + ": ";
            const int first = keyboard.first_note();
            const int last = keyboard.last_note();
            check(last - first + 1 == size, tag + "range has the wrong number of keys");
            check(first >= kPianoLowestMidi && last <= kPianoHighestMidi, tag + "range leaves MIDI 21-108");
            if (size == 88) check(first == 21 && last == 108, tag + "88 keys must span A0-C8");
            else check(first % 12 == 0 && first == (octave + 1) * 12, tag + "range must start on C (octave+1)*12");
            check(keyboard.computer_key_base() == (octave + 1) * 12, tag + "computer-key base");
            for (const int width : {1216, 900, 560}) {
                keyboard.layout({100, 400, width, 90});
                check(keyboard.white_key_count() == expected_white_count(first, last), tag + "white key count");
                check(keyboard.white_key_width() >= kPianoMinWhiteKeyWidth - 1, tag + "white keys narrower than the minimum");
                check(keyboard.scrollable() == (expected_white_count(first, last) * kPianoMinWhiteKeyWidth > width),
                      tag + "scrollable flag at width " + n(width));
                for (int note = first; note <= last; ++note) {
                    keyboard.ensure_visible(note);
                    const auto rect = keyboard.key_rect(note);
                    if (!rect) { check(false, tag + "missing rect for " + n(note)); continue; }
                    const bool black = is_black_key(note);
                    const int probeY = black ? rect->y + rect->height / 2 : rect->y + rect->height - 4;
                    const auto hit = keyboard.note_at(rect->x + rect->width / 2, probeY);
                    check(hit && *hit == note, tag + "width " + n(width) + " key " + piano_note_name(note) +
                                               " hit-tests to " + (hit ? n(*hit) : std::string("nothing")));
                    if (black) check(rect->width < keyboard.white_key_width() &&
                                     rect->height < keyboard.keys_area().height, tag + "black keys must be narrower/shorter");
                }
                check(!keyboard.key_rect(first - 1) && !keyboard.key_rect(last + 1), tag + "rects outside the range");
                const auto keys = keyboard.visible_keys();
                bool whitesFirst = true;
                bool seenBlack = false;
                for (const PianoKey& key : keys) {
                    if (key.black) seenBlack = true;
                    else if (seenBlack) whitesFirst = false;
                    check(key.rect.x >= keyboard.keys_area().x &&
                          key.rect.x + key.rect.width <= keyboard.keys_area().x + keyboard.keys_area().width,
                          tag + "visible key outside the viewport");
                }
                check(whitesFirst, tag + "black keys must be drawn after (on top of) white keys");
                if (!keyboard.scrollable()) check(static_cast<int>(keys.size()) == size, tag + "all keys visible");
            }
        }
    }
}

void test_hit_edges() {
    PianoKeyboard keyboard;  // 25 keys, C4..C6
    keyboard.layout({50, 200, 750, 90});
    const UiRect area = keyboard.keys_area();
    // C#4 (61) sits over the C4/D4 boundary.
    const UiRect cs = *keyboard.key_rect(61);
    const UiRect c = *keyboard.key_rect(60);
    const UiRect d = *keyboard.key_rect(62);
    check(cs.x > c.x && cs.x + cs.width < d.x + d.width && cs.x < d.x && cs.x + cs.width > d.x,
          "C#4 must straddle the C4/D4 boundary");
    check(keyboard.note_at(cs.x, cs.y) == 61, "black key top-left corner");
    check(keyboard.note_at(cs.x + cs.width - 1, cs.y + cs.height - 1) == 61, "black key bottom-right pixel");
    check(keyboard.note_at(cs.x - 1, cs.y + 2) == 60, "just left of C#4 is C4");
    check(keyboard.note_at(cs.x + cs.width, cs.y + 2) == 62, "just right of C#4 is D4");
    check(keyboard.note_at(cs.x, cs.y + cs.height) == 60, "just below C#4 (left half) is C4");
    check(keyboard.note_at(cs.x + cs.width - 1, cs.y + cs.height) == 62, "just below C#4 (right half) is D4");
    // E/F have no black key between them.
    const UiRect f = *keyboard.key_rect(65);
    check(keyboard.note_at(f.x, area.y + 2) == 65 && keyboard.note_at(f.x - 1, area.y + 2) == 64,
          "E4/F4 boundary without a black key");
    check(keyboard.note_at(area.x, area.y) == 60, "top-left pixel is the first key");
    check(keyboard.note_at(area.x + area.width - 1, area.y + area.height - 1) == 84, "bottom-right pixel is the last key");
    check(!keyboard.note_at(area.x - 1, area.y + 5) && !keyboard.note_at(area.x + area.width, area.y + 5) &&
          !keyboard.note_at(area.x + 5, area.y - 1) && !keyboard.note_at(area.x + 5, area.y + area.height),
          "points outside the keys miss");
    // Every column of pixels maps to exactly one key, black keys win in the upper part.
    for (int x = area.x; x < area.x + area.width; ++x) {
        const auto top = keyboard.note_at(x, area.y + 1);
        const auto bottom = keyboard.note_at(x, area.y + area.height - 2);
        check(top.has_value() && bottom.has_value() && !is_black_key(*bottom), "pixel column " + n(x) + " unmapped");
        if (top && is_black_key(*top)) check(bottom && std::abs(*bottom - *top) == 1, "black key over a non-neighbour");
    }
    // 88 keys: A0 is white, A#0 black, and C8 is the last white key.
    PianoKeyboard full;
    full.set_key_count(88);
    full.layout({0, 0, 1300, 100});
    check(full.note_at(1, 95) == 21 && full.note_at(full.key_rect(22)->x + 2, 5) == 22 &&
          full.note_at(1299, 95) == 108, "88-key ends");
    // 76 keys end on a black key (D#7), which must stay inside the area.
    PianoKeyboard seventySix;
    seventySix.set_key_count(76);
    seventySix.layout({10, 0, 1000, 100});
    const auto last = seventySix.key_rect(seventySix.last_note());
    check(is_black_key(seventySix.last_note()) && last && last->x + last->width <= 1010, "76-key trailing black key fits");
}

void test_octave_clamp() {
    for (const int size : kPianoKeyboardSizes) {
        PianoKeyboard keyboard;
        keyboard.set_key_count(size);
        keyboard.set_octave(-10);
        check(keyboard.octave() == keyboard.min_octave() && keyboard.first_note() >= kPianoLowestMidi,
              n(size) + " keys: low clamp");
        check(!keyboard.shift_octave(-1), n(size) + " keys: shifting below the minimum reports no change");
        keyboard.set_octave(99);
        check(keyboard.octave() == keyboard.max_octave() && keyboard.last_note() <= kPianoHighestMidi,
              n(size) + " keys: high clamp");
        check(keyboard.computer_key_base() + 19 <= kPianoHighestMidi || size != 88, "88-key computer window");
        check(!keyboard.shift_octave(1), n(size) + " keys: shifting above the maximum reports no change");
        if (size != 88) {
            // One more octave would leave the range.
            check((keyboard.octave() + 2) * 12 + size - 1 > kPianoHighestMidi, n(size) + " keys: max octave is the highest valid");
        }
    }
    PianoKeyboard keyboard;
    keyboard.set_key_count(61);  // octave 4 would end at C9 -> clamp to C3..C8
    check(keyboard.first_note() == 48 && keyboard.last_note() == 108, "61 keys clamp the default octave to C3-C8");
    keyboard.set_key_count(88);
    check(keyboard.first_note() == 21 && keyboard.last_note() == 108, "88 keys ignore the octave for the range");
    check(keyboard.shift_octave(1) && keyboard.first_note() == 21, "88 keys: octave moves only the computer-key window");
}

void test_scrolling() {
    PianoKeyboard keyboard;
    keyboard.set_key_count(88);
    keyboard.layout({20, 100, 400, 90});
    check(keyboard.scrollable() && keyboard.white_key_width() == kPianoMinWhiteKeyWidth, "88 keys in 400 px scroll");
    check(keyboard.content_width() == 52 * kPianoMinWhiteKeyWidth && keyboard.max_scroll() == 52 * 12 - 400,
          "scroll extent");
    check(keyboard.keys_area().height == 90 - kPianoScrollbarHeight, "scrollbar strip reserved");
    check(keyboard.note_at(20, 185) == std::nullopt, "scrollbar strip is not a key");
    keyboard.ensure_visible(108);
    check(keyboard.scroll_offset() == keyboard.max_scroll(), "follow the highest note");
    const auto top = keyboard.key_rect(108);
    check(top && top->x + top->width <= 420, "C8 on screen after ensure_visible");
    check(keyboard.note_at(419, 170) == 108, "hit test honours the scroll offset");
    keyboard.ensure_visible(21);
    check(keyboard.scroll_offset() == 0 && keyboard.note_at(20, 170) == 21, "follow back to A0");
    check(keyboard.wheel(-1.0F) && keyboard.scroll_offset() == 24, "wheel down scrolls right by two white keys");
    check(keyboard.wheel(1.0F) && keyboard.scroll_offset() == 0 && !keyboard.wheel(1.0F), "wheel up clamps at 0");
    // Drag the thumb to the far right.
    const UiRect thumb = keyboard.scrollbar_thumb();
    check(thumb.width >= 24 && thumb.x == 20, "thumb starts at the left");
    check(keyboard.pointer_down(thumb.x + 2, thumb.y + 1) && keyboard.dragging(), "thumb drag starts");
    (void)keyboard.pointer_drag(thumb.x + 2 + 1000);
    keyboard.pointer_up();
    check(keyboard.scroll_offset() == keyboard.max_scroll() && !keyboard.dragging(), "thumb drag scrolls to the end");
    const UiRect moved = keyboard.scrollbar_thumb();
    check(moved.x + moved.width == 420, "thumb at the right end");
    check(keyboard.pointer_down(25, moved.y + 1) && keyboard.scroll_offset() == std::max(0, keyboard.max_scroll() - 400),
          "track press pages left");
    PianoKeyboard wide;
    wide.set_key_count(88);
    wide.layout({0, 0, 2400, 90});
    check(!wide.scrollable() && wide.max_scroll() == 0 && !wide.wheel(-3.0F), "wide 88 keys fit without scrolling");
}

std::filesystem::path temp_root(const char* name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    return root;
}

bool voice_playing(NativeEditorController& controller, int note, std::size_t frames = 512U) {
    std::vector<float> audio(frames * 2U);
    controller.synthesizer().render(audio);
    for (const auto& voice : controller.synthesizer().voices())
        if (voice.active && voice.note == note) return true;
    return false;
}

void test_synth_integration() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 800);
    (void)controller.dispatch_action("window.toggle_synth");
    const auto& panel = controller.synth_panel();
    check(panel.keyboard().key_count() == 25 && panel.keyboard().first_note() == 60, "synth default piano C4-C6");
    controller.key_down("z", false, false, false);
    check(voice_playing(controller, 60), "computer key z plays C4 (first drawn key)");
    controller.key_down("right", false, false, false);  // octave up while z is held
    controller.key_up("z", false, false, false);
    check(!voice_playing(controller, 60, 48000U * 3U), "z releases the note it started even after an octave change");
    controller.key_down("z", false, false, false);
    check(voice_playing(controller, 72), "octave up moves z to C5");
    controller.key_up("z", false, false, false);
    // Pointer on a black key.
    const auto cs = panel.keyboard().key_rect(panel.keyboard().first_note() + 1);
    check(cs.has_value(), "C# rect");
    if (cs) {
        controller.pointer_down(PointerButton::Primary, cs->x + cs->width / 2, cs->y + 3);
        check(voice_playing(controller, panel.keyboard().first_note() + 1), "click on a black key plays it");
        controller.pointer_up(PointerButton::Primary, cs->x + cs->width / 2, cs->y + 3);
    }
    for (int i = 0; i < 12; ++i) controller.key_down("left", false, false, false);
    check(panel.octave() == panel.keyboard().min_octave() && panel.keyboard().first_note() >= 21, "octave clamps low");

    // 88 keys in the smallest logical canvas: the piano scrolls and follows notes.
    check(controller.set_keyboard_key_count(88), "set 88 keys");
    controller.resize(640, 480);
    const PianoKeyboard& keyboard = panel.keyboard();
    check(keyboard.key_count() == 88 && keyboard.scrollable(), "88 keys at 640x480 scroll");
    const UiRect area = keyboard.area();
    controller.pointer_wheel(-5.0F, area.x + 20, area.y + 20);
    check(keyboard.scroll_offset() == keyboard.max_scroll() && keyboard.max_scroll() > 0,
          "mouse wheel over the piano scrolls it right");
    controller.pointer_wheel(1.0F, area.x + 20, area.y + 20);
    check(keyboard.scroll_offset() < keyboard.max_scroll(), "mouse wheel up scrolls it back left");
    controller.pointer_wheel(-5.0F, area.x + 20, area.y + 20);
    for (int i = 0; i < 12; ++i) controller.key_down("left", false, false, false);  // octave 1: z = C1 (24)
    controller.key_down("z", false, false, false);
    const int played = keyboard.computer_key_base();
    check(played == 24 && voice_playing(controller, played), "z plays C1 at the lowest octave on 88 keys");
    const auto rect = keyboard.key_rect(played);
    check(rect && rect->x >= area.x && rect->x + rect->width <= area.x + area.width,
          "the played note is scrolled into view");
    check(keyboard.scroll_offset() < keyboard.max_scroll(), "following the note moved the piano");
    controller.key_up("z", false, false, false);
    // Key-count button cycles and persists through the controller.
    const UiRect button = panel.layout().keyboardKeysButton;
    controller.pointer_down(PointerButton::Primary, button.x + 4, button.y + 4);
    controller.pointer_up(PointerButton::Primary, button.x + 4, button.y + 4);
    check(controller.keyboard_key_count() == 25 && panel.keyboard().key_count() == 25, "keys button cycles 88 -> 25");
}

void test_chiptune_integration() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 800);
    (void)controller.dispatch_action("view.keyboard_keys_49");
    (void)controller.dispatch_action("window.toggle_chiptune");
    auto& chip = controller.chiptune_panel();
    check(chip.open(), "chiptune panel opened");
    check(chip.keyboard().key_count() == 49, "chiptune follows the key-count setting");
    chip.session().set_octave(8);
    check(chip.keyboard().last_note() <= 108 && chip.keyboard().first_note() == 60, "chiptune piano octave clamps (49 keys)");
    chip.session().set_octave(0);
    check(chip.keyboard().first_note() == 24, "chiptune piano clamps low to C1");
}

void test_persistence() {
    const auto root = temp_root("dve_piano_keyboard_settings");
    const auto path = root / "editor_settings.txt";
    {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.configure_user_settings(path);
        check(controller.keyboard_key_count() == 25, "default key count");
        check(controller.dispatch_action("view.keyboard_keys_61"), "menu action sets 61 keys");
        check(!controller.dispatch_action("view.keyboard_keys_60"), "unsupported size is rejected");
        const auto* item = controller.workspace().menus().find("view.keyboard_keys_61");
        check(item != nullptr && item->checked, "61-key menu item checked");
        const auto* other = controller.workspace().menus().find("view.keyboard_keys_25");
        check(other != nullptr && !other->checked, "radio group unchecks 25");
        check(std::filesystem::exists(path), "User settings saved");
    }
    {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.configure_user_settings(path);
        check(controller.keyboard_key_count() == 61, "key count reloaded from User settings");
        check(controller.synth_panel().keyboard().key_count() == 61, "synth piano uses the reloaded key count");
        check(controller.chiptune_panel().keyboard().key_count() == 61, "chiptune piano uses the reloaded key count");
        check(controller.workspace().settings().has_override(SettingScope::User, kKeyboardKeysSettingId) &&
              !controller.workspace().settings().has_override(SettingScope::Project, kKeyboardKeysSettingId),
              "key count comes from the User scope");
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
    test_basics();
    test_mapping_every_size();
    test_hit_edges();
    test_octave_clamp();
    test_scrolling();
    test_synth_integration();
    test_chiptune_integration();
    test_persistence();
    if (g_failures == 0) std::printf("editor piano keyboard tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
