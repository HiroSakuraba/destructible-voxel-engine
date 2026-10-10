#include "dve/editor_text_encoding.hpp"

namespace dve::editor {

char32_t decode_utf8_code_point(std::string_view value, std::size_t& offset) noexcept {
    if (offset >= value.size()) return kUtf8ReplacementCodePoint;
    const auto lead = static_cast<unsigned char>(value[offset]);
    if (lead < 0x80U) { ++offset; return static_cast<char32_t>(lead); }
    std::size_t length = 0;
    char32_t codePoint = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0U) == 0xC0U) { length = 2; codePoint = lead & 0x1FU; minimum = 0x80; }
    else if ((lead & 0xF0U) == 0xE0U) { length = 3; codePoint = lead & 0x0FU; minimum = 0x800; }
    else if ((lead & 0xF8U) == 0xF0U) { length = 4; codePoint = lead & 0x07U; minimum = 0x10000; }
    else { ++offset; return kUtf8ReplacementCodePoint; }  // stray continuation or invalid lead
    if (offset + length > value.size()) { ++offset; return kUtf8ReplacementCodePoint; }
    for (std::size_t i = 1; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(value[offset + i]);
        if ((byte & 0xC0U) != 0x80U) { ++offset; return kUtf8ReplacementCodePoint; }
        codePoint = (codePoint << 6U) | (byte & 0x3FU);
    }
    if (codePoint < minimum || codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
        ++offset;
        return kUtf8ReplacementCodePoint;
    }
    offset += length;
    return codePoint;
}

std::size_t utf8_code_point_count(std::string_view value) noexcept {
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < value.size(); ++count) (void)decode_utf8_code_point(value, offset);
    return count;
}

std::string_view ascii_fallback_for_code_point(char32_t codePoint) noexcept {
    switch (codePoint) {
    // Latin-1 symbols, for fonts that lack the glyph.
    case U'\u00A0': return " ";
    case U'\u00B0': return "deg";
    case U'\u00B1': return "+/-";
    case U'\u00B2': return "2";
    case U'\u00B3': return "3";
    case U'\u00B5': return "u";
    case U'\u00B7': return ".";
    case U'\u00D7': return "x";
    case U'\u00F7': return "/";
    // Outside Latin-1.
    case U'\u2010': case U'\u2011': case U'\u2012': case U'\u2013': case U'\u2014': case U'\u2015':
    case U'\u2212': return "-";
    case U'\u2018': case U'\u2019': case U'\u201A': case U'\u2032': return "'";
    case U'\u201C': case U'\u201D': case U'\u201E': case U'\u2033': return "\"";
    case U'\u2022': case U'\u2219': return "*";
    case U'\u2026': return "...";
    case U'\u2190': return "<-";
    case U'\u2192': return "->";
    case U'\u2191': return "^";
    case U'\u2193': return "v";
    case U'\u2194': return "<->";
    case U'\u21D2': return "=>";
    case U'\u2248': return "~";
    case U'\u2260': return "!=";
    case U'\u2264': return "<=";
    case U'\u2265': return ">=";
    case U'\u221E': return "inf";
    case U'\u03BC': return "u";      // Greek mu, often used for micro
    case U'\u2126': return "Ohm";
    case U'\u2713': case U'\u2714': return "v";
    case U'\u2715': case U'\u2716': case U'\u2717': case U'\u2718': return "x";
    case U'\u25B2': return "^";
    case U'\u25BC': return "v";
    case U'\u25B6': case U'\u25BA': return ">";
    case U'\u25C0': case U'\u25C4': return "<";
    case U'\u2318': return "Cmd";
    case U'\u21E7': return "Shift";
    case U'\u2325': return "Alt";
    case U'\u200B': case U'\uFEFF': return "";  // zero-width; handled by caller (dropped)
    default: return {};
    }
}

std::string utf8_to_single_byte_font_text(std::string_view utf8, const SingleByteGlyphPredicate& hasGlyph) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t offset = 0; offset < utf8.size();) {
        const char32_t codePoint = decode_utf8_code_point(utf8, offset);
        if (codePoint < 0x80U) { out.push_back(static_cast<char>(codePoint)); continue; }
        if (codePoint == U'\u200B' || codePoint == U'\uFEFF') continue;  // zero-width: draw nothing
        if (codePoint >= 0xA0U && codePoint <= 0xFFU) {
            const auto byte = static_cast<unsigned char>(codePoint);
            if (!hasGlyph || hasGlyph(byte)) { out.push_back(static_cast<char>(byte)); continue; }
        }
        const std::string_view fallback = ascii_fallback_for_code_point(codePoint);
        if (!fallback.empty()) out.append(fallback);
        else out.push_back('?');
    }
    return out;
}

} // namespace dve::editor
