// UTF-8 decoding for single-byte (Latin-1 indexed) editor fonts, e.g. the X11 core-font
// fallback of dve_native_editor_x11: "R 15.0°" must draw the degree glyph, not "Â°".
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_text_encoding.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dve::editor;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Fixed-pitch canvas modelled on the X11 core-font path: every string is re-encoded for a
// Latin-1 font, then measured/drawn one byte per glyph cell.
class FixedLatin1Canvas final : public IEditorCanvas {
public:
    static constexpr int kCell = 6;
    explicit FixedLatin1Canvas(SingleByteGlyphPredicate hasGlyph = {}) : hasGlyph_(std::move(hasGlyph)) {}
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int = 1) const override {}
    void text(int, int, std::string_view value, EditorColor) const override {
        drawn.push_back(utf8_to_single_byte_font_text(value, hasGlyph_));
    }
    [[nodiscard]] std::string_view ellipsis() const override { return "..."; }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(utf8_to_single_byte_font_text(value, hasGlyph_).size()) * kCell;
    }
    mutable std::vector<std::string> drawn;
private:
    SingleByteGlyphPredicate hasGlyph_;
};

void test_decode() {
    const std::string_view degree = "15.0\u00B0";
    require(degree.size() == 6U, "fixture: \"15.0°\" is 6 UTF-8 bytes");
    require(utf8_code_point_count(degree) == 5U, "\"15.0°\" is 5 code points");
    std::size_t offset = 4;
    require(decode_utf8_code_point(degree, offset) == U'\u00B0' && offset == 6U, "decodes C2 B0 as U+00B0");
    offset = 0;
    require(decode_utf8_code_point("\u2192", offset) == U'\u2192' && offset == 3U, "3-byte sequence");
    offset = 0;
    require(decode_utf8_code_point("\xF0\x9F\x99\x82", offset) == U'\U0001F642' && offset == 4U, "4-byte sequence");
    // Malformed input: each bad byte becomes one replacement and decoding always advances.
    require(utf8_code_point_count("\xC2") == 1U, "truncated sequence counts once");
    require(utf8_code_point_count("\xB0\xB0") == 2U, "stray continuation bytes count individually");
    offset = 0;
    require(decode_utf8_code_point("\xC0\xAF", offset) == kUtf8ReplacementCodePoint && offset == 1U, "overlong rejected");
    offset = 0;
    require(decode_utf8_code_point("\xED\xA0\x80", offset) == kUtf8ReplacementCodePoint, "surrogate rejected");
}

void test_latin1_font() {
    const std::string bytes = utf8_to_single_byte_font_text("R 15.0\u00B0");
    require(bytes == std::string("R 15.0\xB0"), "degree sign maps to the single Latin-1 byte B0");
    require(bytes.find('\xC2') == std::string::npos, "no stray \xC3\x82 (C2) byte");
    require(utf8_to_single_byte_font_text("m/s\u00B2") == std::string("m/s\xB2"), "superscript two is Latin-1");
    require(utf8_to_single_byte_font_text("5\u00B5s") == std::string("5\xB5s"), "micro sign is Latin-1");
    require(utf8_to_single_byte_font_text("2\u00D73") == std::string("2\xD7" "3"), "multiplication sign is Latin-1");
    require(utf8_to_single_byte_font_text("plain ASCII") == "plain ASCII", "ASCII unchanged");
    // Outside Latin-1: readable ASCII fallbacks instead of mojibake.
    require(utf8_to_single_byte_font_text("A \u2192 B") == "A -> B", "right arrow falls back to ->");
    require(utf8_to_single_byte_font_text("Wait\u2026") == "Wait...", "ellipsis falls back to ...");
    require(utf8_to_single_byte_font_text("a\u2014b") == "a-b", "em dash falls back to -");
    require(utf8_to_single_byte_font_text("\u2264 \u2265") == "<= >=", "comparison signs");
    require(utf8_to_single_byte_font_text("x\u200By") == "xy", "zero-width space dropped");
    require(utf8_to_single_byte_font_text("\xE6\x97\xA5") == "?", "unmapped CJK becomes one ?");
    require(utf8_to_single_byte_font_text("a\xC2" "b") == "a?b", "malformed byte becomes ?");
    require(utf8_to_single_byte_font_text("\xC2\x85") == "?", "C1 control is not drawn as a glyph");
}

void test_font_without_glyph() {
    const auto asciiOnly = [](unsigned char byte) { return byte < 0x80U; };
    require(utf8_to_single_byte_font_text("R 15.0\u00B0", asciiOnly) == "R 15.0deg", "degree falls back to deg");
    require(utf8_to_single_byte_font_text("2\u00D73", asciiOnly) == "2x3", "times falls back to x");
    require(utf8_to_single_byte_font_text("5\u00B5s", asciiOnly) == "5us", "micro falls back to u");
    require(utf8_to_single_byte_font_text("\u00E9", asciiOnly) == "?", "unmapped Latin-1 letter becomes ?");
}

void test_keysym_text() {
    require(utf8_from_keysym(0x61UL) == "a" && utf8_from_keysym(0x41UL) == "A", "ASCII keysyms");
    require(utf8_from_keysym(0xE9UL) == "\xC3\xA9", "eacute (Latin-1 keysym) is U+00E9");
    require(utf8_from_keysym(0xFCUL) == "\xC3\xBC", "udiaeresis is U+00FC");
    require(utf8_from_keysym(0x01000416UL) == "\xD0\x96", "Unicode keysym U+0416");
    require(utf8_from_keysym(0x010020ACUL) == "\xE2\x82\xAC", "Unicode keysym U+20AC");
    require(utf8_from_keysym(0x0100F600UL).empty() == false, "private-use code point is still text");
    require(utf8_from_keysym(0xFFB5UL) == "5" && utf8_from_keysym(0xFFAEUL) == ".", "keypad digits and point");
    for (const unsigned long keysym : {0xFF0DUL /* Return */, 0xFF08UL /* BackSpace */, 0xFFE1UL /* Shift_L */,
                                       0xFF51UL /* Left */, 0xFFBEUL /* F1 */, 0x0UL, 0x1FUL, 0x7FUL, 0x80UL, 0x9FUL})
        require(utf8_from_keysym(keysym).empty(), "non-text keysym types nothing");
    require(utf8_from_keysym(0x01000000UL + 0xD800UL).empty(), "surrogate keysym types nothing");
    require(utf8_from_keysym(0x01110000UL).empty(), "beyond U+10FFFF types nothing");

    std::string text;
    require(append_utf8_code_point(text, U'a') && append_utf8_code_point(text, U'\u00E9') &&
            append_utf8_code_point(text, U'\u20AC') && append_utf8_code_point(text, U'\U0001F600'),
            "append 1..4 byte code points");
    require(text == "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", "encoded bytes");
    require(!append_utf8_code_point(text, 0xD800U) && !append_utf8_code_point(text, 0x110000U) &&
            text.size() == 10U, "invalid code points append nothing");
    std::size_t offset = 0;
    require(decode_utf8_code_point(text, offset) == U'a' && decode_utf8_code_point(text, offset) == U'\u00E9' &&
            decode_utf8_code_point(text, offset) == U'\u20AC' && decode_utf8_code_point(text, offset) == U'\U0001F600' &&
            offset == text.size(), "round trip");

    require(is_printable_utf8("Tube-A \xC3\xA9 \xC3\xBC <b>"), "mixed ASCII and accents are printable");
    require(!is_printable_utf8("") && !is_printable_utf8("\n") && !is_printable_utf8("a\tb") && !is_printable_utf8("\x7F") &&
            !is_printable_utf8("\xC2\x85") && !is_printable_utf8("\xC3") && !is_printable_utf8("\xEF\xBF\xBD"),
            "controls, DEL, C1, truncated and replacement characters are not text");
}

void test_canvas_measure_and_draw() {
    FixedLatin1Canvas canvas;
    require(canvas.text_width("15.0\u00B0") == 5 * FixedLatin1Canvas::kCell, "\"15.0°\" measures as 5 glyphs");
    canvas.text(0, 0, "R 15.0\u00B0", 0xFFFFFFU);
    require(canvas.drawn.size() == 1U, "one string drawn");
    require(canvas.drawn[0].size() == 7U && static_cast<unsigned char>(canvas.drawn[0].back()) == 0xB0U,
            "draws the degree glyph");
    require(canvas.drawn[0].find('\xC2') == std::string::npos, "no stray \xC3\x82 drawn");
    // Eliding at code point boundaries keeps multi-byte characters whole.
    const std::string elided = elide_text_to_width(canvas, "Rotate 15\u00B0 \u2192 30\u00B0", 10 * FixedLatin1Canvas::kCell);
    require(canvas.text_width(elided) <= 10 * FixedLatin1Canvas::kCell, "elided text fits");
    require(utf8_to_single_byte_font_text(elided).find('?') == std::string::npos, "elision never splits a sequence");
}

} // namespace

int main() {
    try {
        test_decode();
        test_latin1_font();
        test_font_without_glyph();
        test_keysym_text();
        test_canvas_measure_and_draw();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "dve_editor_text_encoding_tests passed\n";
    return 0;
}
