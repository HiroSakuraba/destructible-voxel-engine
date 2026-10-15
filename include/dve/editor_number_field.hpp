#pragma once

// Typed values for the editor's numeric fields (inspector Position / Rotation / Voxel size, the
// viewport tool strip's Brush radius / Surface offset): units, arithmetic and relative edits.
//
//   "1.5"  "150cm"  "2 ft"  "6in"  "1'6\""  "90deg"  "0.25turn"  "(pi/2)rad"  "2*0.25"  "(1+2)/4"
//   "+=0.5"  "-=10cm"  "*=2"  "/=2"   (relative to the field's current value)
//
// A bare number is in the field's display unit (meters, degrees, voxels). Results are not
// clamped here; callers clamp to their own ranges.

#include <optional>
#include <string>
#include <string_view>

namespace dve::editor {

enum class NumberFieldUnit : unsigned char {
    Meters,   // accepts mm, cm, m, km, in, ft, ' and "
    Degrees,  // accepts deg, °, rad, turn
    Count,    // plain numbers (voxels); no units
};

struct NumberFieldParse {
    bool ok{};
    double value{};      // the final value (relative edits already applied to `current`)
    std::string error;   // why the text was refused
};

[[nodiscard]] NumberFieldParse parse_number_field(std::string_view text, NumberFieldUnit unit, double current);

// Compact display: up to 4 decimals for meters, 2 for degrees, trailing zeros trimmed, "-0" → "0".
[[nodiscard]] std::string format_number_field(double value, NumberFieldUnit unit);

}  // namespace dve::editor
