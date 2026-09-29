#include "dve/editor_piano_keyboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dve::editor {

int snap_piano_key_count(int count) noexcept {
    int best = kPianoKeyboardSizes.front();
    for (const int size : kPianoKeyboardSizes)
        if (std::abs(size - count) < std::abs(best - count)) best = size;
    return best;
}

bool is_black_key(int midi) noexcept {
    switch (((midi % 12) + 12) % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

std::string piano_note_name(int midi) {
    static constexpr std::array<std::string_view, 12> names{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int pitch = ((midi % 12) + 12) % 12;
    const int octave = (midi - pitch) / 12 - 1;
    return std::string(names[static_cast<std::size_t>(pitch)]) + std::to_string(octave);
}

PianoKeyboard::PianoKeyboard() noexcept { clamp_octave(); }

void PianoKeyboard::set_key_count(int count) noexcept {
    const int snapped = snap_piano_key_count(count);
    if (snapped == keyCount_) return;
    keyCount_ = snapped;
    clamp_octave();
    layout(area_);
}

int PianoKeyboard::min_octave() const noexcept {
    // The first C-start note (or computer-key base for 88 keys) must be >= A0.
    return 1;
}

int PianoKeyboard::max_octave() const noexcept {
    if (keyCount_ == 88) return (kPianoHighestMidi - 19) / 12 - 1;  // 20 computer keys stay <= C8
    return std::max(min_octave(), (kPianoHighestMidi - (keyCount_ - 1)) / 12 - 1);
}

void PianoKeyboard::clamp_octave() noexcept { octave_ = std::clamp(octave_, min_octave(), max_octave()); }

void PianoKeyboard::set_octave(int octave) noexcept {
    const int before = first_note();
    octave_ = octave;
    clamp_octave();
    if (first_note() != before) layout(area_);
}

bool PianoKeyboard::shift_octave(int delta) noexcept {
    const int before = octave_;
    set_octave(octave_ + delta);
    return octave_ != before;
}

int PianoKeyboard::first_note() const noexcept {
    return keyCount_ == 88 ? kPianoLowestMidi : (octave_ + 1) * 12;
}

int PianoKeyboard::last_note() const noexcept { return first_note() + keyCount_ - 1; }

int PianoKeyboard::white_index(int midi) const noexcept {
    int count = 0;
    for (int note = first_note(); note < midi; ++note)
        if (!is_black_key(note)) ++count;
    return count;
}

int PianoKeyboard::white_key_count() const noexcept { return white_index(last_note() + 1); }

int PianoKeyboard::white_left(int index) const noexcept {
    if (scrollable_) return index * whiteWidth_;
    const int whites = std::max(1, white_key_count());
    return index * availableWidth_ / whites;
}

UiRect PianoKeyboard::keys_area() const noexcept {
    UiRect keys = area_;
    if (scrollable_) keys.height = std::max(0, keys.height - kPianoScrollbarHeight);
    return keys;
}

int PianoKeyboard::black_key_height() const noexcept { return keys_area().height * 62 / 100; }

void PianoKeyboard::layout(UiRect area) noexcept {
    area_ = area;
    const int whites = std::max(1, white_key_count());
    const bool trailingBlack = is_black_key(last_note());
    const int proportional = std::max(0, area.width) / whites;
    scrollable_ = proportional < kPianoMinWhiteKeyWidth;
    whiteWidth_ = scrollable_ ? kPianoMinWhiteKeyWidth : proportional;
    blackWidth_ = std::max(5, whiteWidth_ * 3 / 5);
    const int trailing = trailingBlack ? (blackWidth_ + 1) / 2 : 0;
    if (scrollable_) {
        availableWidth_ = whites * whiteWidth_;
        contentWidth_ = availableWidth_ + trailing;
        if (contentWidth_ <= area.width) {  // the trailing half key was the only overflow
            scrollable_ = false;
            availableWidth_ = std::max(whites, area.width - trailing);
            contentWidth_ = area.width;
        }
    } else {
        availableWidth_ = std::max(whites, area.width - trailing);
        whiteWidth_ = availableWidth_ / whites;
        blackWidth_ = std::max(5, whiteWidth_ * 3 / 5);
        contentWidth_ = area.width;
    }
    clamp_scroll();
}

int PianoKeyboard::max_scroll() const noexcept {
    return scrollable_ ? std::max(0, contentWidth_ - area_.width) : 0;
}

void PianoKeyboard::clamp_scroll() noexcept { scroll_ = std::clamp(scroll_, 0, max_scroll()); }

void PianoKeyboard::set_scroll_offset(int offset) noexcept {
    scroll_ = offset;
    clamp_scroll();
}

UiRect PianoKeyboard::content_rect(int midi) const noexcept {
    const UiRect keys = keys_area();
    const int index = white_index(midi);
    if (!is_black_key(midi)) {
        const int left = white_left(index);
        return UiRect{left, 0, white_left(index + 1) - left, keys.height};
    }
    const int boundary = white_left(index);
    return UiRect{boundary - blackWidth_ / 2, 0, blackWidth_, black_key_height()};
}

std::optional<UiRect> PianoKeyboard::key_rect(int midi) const noexcept {
    if (midi < first_note() || midi > last_note()) return std::nullopt;
    UiRect rect = content_rect(midi);
    rect.x += area_.x - scroll_;
    rect.y += area_.y;
    return rect;
}

std::optional<int> PianoKeyboard::note_at(int x, int y) const noexcept {
    const UiRect keys = keys_area();
    if (!keys.contains(x, y)) return std::nullopt;
    const int cx = x - keys.x + scroll_;
    const int cy = y - keys.y;
    // Black keys sit on top, so they are tested first.
    if (cy < black_key_height()) {
        for (int note = first_note(); note <= last_note(); ++note) {
            if (!is_black_key(note)) continue;
            const UiRect rect = content_rect(note);
            if (cx >= rect.x && cx < rect.x + rect.width) return note;
        }
    }
    for (int note = first_note(); note <= last_note(); ++note) {
        if (is_black_key(note)) continue;
        const UiRect rect = content_rect(note);
        if (cx >= rect.x && cx < rect.x + rect.width) return note;
    }
    return std::nullopt;
}

std::vector<PianoKey> PianoKeyboard::visible_keys() const {
    std::vector<PianoKey> keys;
    const UiRect view = keys_area();
    const int viewRight = view.x + view.width;
    keys.reserve(static_cast<std::size_t>(keyCount_));
    for (int pass = 0; pass < 2; ++pass) {
        for (int note = first_note(); note <= last_note(); ++note) {
            const bool black = is_black_key(note);
            if (black != (pass == 1)) continue;
            UiRect rect = *key_rect(note);
            const int left = std::max(rect.x, view.x);
            const int right = std::min(rect.x + rect.width, viewRight);
            if (right <= left) continue;
            rect.x = left;
            rect.width = right - left;
            keys.push_back(PianoKey{note, black, rect});
        }
    }
    return keys;
}

UiRect PianoKeyboard::scrollbar_track() const noexcept {
    if (!scrollable_) return UiRect{};
    return UiRect{area_.x, area_.y + area_.height - kPianoScrollbarHeight + 2, area_.width,
                  kPianoScrollbarHeight - 2};
}

UiRect PianoKeyboard::scrollbar_thumb() const noexcept {
    const UiRect track = scrollbar_track();
    if (track.width <= 0 || contentWidth_ <= 0) return UiRect{};
    const int thumbWidth = std::clamp(track.width * area_.width / contentWidth_, 24, track.width);
    const int travel = track.width - thumbWidth;
    const int maxScroll = std::max(1, max_scroll());
    return UiRect{track.x + travel * scroll_ / maxScroll, track.y, thumbWidth, track.height};
}

bool PianoKeyboard::wheel(float steps) noexcept {
    if (!scrollable_ || steps == 0.0F) return false;
    const int before = scroll_;
    set_scroll_offset(scroll_ - static_cast<int>(std::lround(steps * static_cast<float>(whiteWidth_ * 2))));
    return scroll_ != before;
}

void PianoKeyboard::ensure_visible(int midi) noexcept {
    if (!scrollable_ || midi < first_note() || midi > last_note()) return;
    const UiRect rect = content_rect(midi);
    if (rect.x < scroll_) set_scroll_offset(rect.x - whiteWidth_);
    else if (rect.x + rect.width > scroll_ + area_.width)
        set_scroll_offset(rect.x + rect.width - area_.width + whiteWidth_);
}

bool PianoKeyboard::pointer_down(int x, int y) noexcept {
    const UiRect track = scrollbar_track();
    if (track.width <= 0) return false;
    // Accept the whole reserved strip, not only the drawn track.
    const UiRect strip{area_.x, area_.y + area_.height - kPianoScrollbarHeight, area_.width, kPianoScrollbarHeight};
    if (!strip.contains(x, y)) return false;
    const UiRect thumb = scrollbar_thumb();
    if (x >= thumb.x && x < thumb.x + thumb.width) {
        dragging_ = true;
        dragAnchorX_ = x;
        dragAnchorScroll_ = scroll_;
    } else {
        set_scroll_offset(scroll_ + (x < thumb.x ? -area_.width : area_.width));
    }
    return true;
}

bool PianoKeyboard::pointer_drag(int x) noexcept {
    if (!dragging_) return false;
    const UiRect track = scrollbar_track();
    const UiRect thumb = scrollbar_thumb();
    const int travel = std::max(1, track.width - thumb.width);
    set_scroll_offset(dragAnchorScroll_ + (x - dragAnchorX_) * max_scroll() / travel);
    return true;
}

}  // namespace dve::editor
