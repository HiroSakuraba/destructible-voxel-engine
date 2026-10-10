#pragma once

// UTF-8 handling for editor text backends whose fonts are indexed by single bytes (the X11
// core-font fallback, e.g. misc-fixed "fixed"/6x13, which is ISO 8859-1). Editor strings are
// UTF-8 ("R 15.0°", "m/s²", user asset names, the "…" ellipsis); handing those bytes straight
// to XDrawString draws each byte as its own Latin-1 glyph, so "°" (C2 B0) shows up as "Â°".
// These helpers decode UTF-8 to code points and re-encode for such a font: code points the
// font has a glyph for (U+0000..U+00FF) become that single byte, common typographic symbols
// outside it get a readable ASCII fallback ("×" -> "x", "→" -> "->", "…" -> "..."), and
// anything else, including malformed UTF-8, becomes '?'. Scalable backends (Xft, SDL_ttf)
// take UTF-8 directly and do not need this.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace dve::editor {

inline constexpr char32_t kUtf8ReplacementCodePoint = U'\uFFFD';

// Decodes the code point starting at value[offset] and advances offset past it. Malformed,
// overlong, surrogate, or truncated sequences yield kUtf8ReplacementCodePoint and advance by
// one byte so decoding always makes progress.
[[nodiscard]] char32_t decode_utf8_code_point(std::string_view value, std::size_t& offset) noexcept;

// Number of code points (glyphs, for the editor's fixed-pitch fonts) in a UTF-8 string.
[[nodiscard]] std::size_t utf8_code_point_count(std::string_view value) noexcept;

// ASCII stand-in for common non-Latin-1 symbols (and for the Latin-1 symbols the editor uses,
// for fonts that lack them); empty when there is none.
[[nodiscard]] std::string_view ascii_fallback_for_code_point(char32_t codePoint) noexcept;

// Predicate: does the single-byte font have a glyph for this byte (Latin-1 code point)?
using SingleByteGlyphPredicate = std::function<bool(unsigned char)>;

// Re-encodes UTF-8 for a single-byte (ISO 8859-1 indexed) font. A null predicate means the
// font covers all of Latin-1. ASCII bytes are always passed through unchanged.
[[nodiscard]] std::string utf8_to_single_byte_font_text(std::string_view utf8,
                                                        const SingleByteGlyphPredicate& hasGlyph = {});

} // namespace dve::editor
