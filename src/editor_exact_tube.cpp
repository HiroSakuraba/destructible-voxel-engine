#include "dve/editor_exact_tube.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dve::editor {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxLength = 100.0;     // metres: a sanity bound against typos like "6000 m"
constexpr double kMinWall = 1.0e-4;      // 0.1 mm
constexpr double kMinLength = 1.0e-3;

std::string lower_copy(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Shortest readable number (up to six significant digits), classic locale.
std::string number_text(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(6) << value;
    return out.str();
}

std::string mm_text(double metres) { return number_text(metres * 1000.0); }

std::string sanitize_record(std::string text) {
    for (auto& c : text) if (c == ';' || c == '\n' || c == '\r') c = ',';
    return text;
}

const char* axis_name(char axis) { return axis == 'x' ? "X" : axis == 'y' ? "Y" : "Z"; }

// A length with an optional unit, in metres. Plain numbers are millimetres.
std::optional<double> parse_length(std::string_view token) {
    std::string text = lower_copy(std::string(token));
    double scale = 0.001;
    const auto strip = [&](std::string_view unit, double factor) {
        if (text.size() > unit.size() && text.ends_with(unit)) { text.resize(text.size() - unit.size()); scale = factor; return true; }
        return false;
    };
    if (!(strip("mm", 0.001) || strip("cm", 0.01) || strip("in", 0.0254) || strip("m", 1.0))) {
        if (!text.empty() && text.back() == '"') { text.pop_back(); scale = 0.0254; }
    }
    if (text.empty()) return std::nullopt;
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value * scale;
}

// Splits "rect 80x40 3 600 z" into words; a lone 'x' between a number (or its unit) and a digit
// separates two sizes, a word that is just "x" is the axis.
std::vector<std::string> split_words(std::string_view text) {
    std::string flat;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xC3 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x97) { flat += 'x'; ++i; }  // U+00D7
        else if (c == ',' || c == ';' || c == '\t' || c == '\n' || c == '\r') flat += ' ';
        else flat += static_cast<char>(c);
    }
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&] { if (!word.empty()) { words.push_back(std::move(word)); word.clear(); } };
    for (std::size_t i = 0; i < flat.size(); ++i) {
        const char c = flat[i];
        if (c == ' ') { flush(); continue; }
        const bool separator = (c == 'x' || c == 'X') && !word.empty() && i + 1 < flat.size() &&
            (std::isdigit(static_cast<unsigned char>(flat[i + 1])) || flat[i + 1] == '.') &&
            (std::isdigit(static_cast<unsigned char>(word.back())) || word.back() == '.' || std::isalpha(static_cast<unsigned char>(word.back()))) &&
            std::any_of(word.begin(), word.end(), [](char w) { return std::isdigit(static_cast<unsigned char>(w)) != 0; });
        if (separator) { flush(); continue; }
        word += c;
    }
    flush();
    return words;
}

ExactPrimitive tube_outer_primitive(const TubeSpec& s, double grow) {
    ExactPrimitive p;
    const auto axes = tube_axes(s.axis);
    if (s.profile == TubeProfile::Round) {
        p.kind = ExactPrimitive::Kind::Cylinder;
        p.radius = s.width / 2 + grow;
        p.height = s.length + 2 * grow;
        p.axis = s.axis;
    } else {
        p.kind = ExactPrimitive::Kind::Box;
        p.size[std::size_t(axes[0])] = s.length + 2 * grow;
        p.size[std::size_t(axes[1])] = s.width + 2 * grow;
        p.size[std::size_t(axes[2])] = s.height + 2 * grow;
    }
    return p;
}

// The bore runs past both ends so the difference is a clean through-cut (no coplanar end faces).
ExactPrimitive tube_bore_primitive(const TubeSpec& s) {
    ExactPrimitive p;
    const auto axes = tube_axes(s.axis);
    const double margin = 0.25 * s.length + 0.005;
    if (s.profile == TubeProfile::Round) {
        p.kind = ExactPrimitive::Kind::Cylinder;
        p.radius = s.width / 2 - s.wall;
        p.height = s.length + 2 * margin;
        p.axis = s.axis;
    } else {
        p.kind = ExactPrimitive::Kind::Box;
        p.size[std::size_t(axes[0])] = s.length + 2 * margin;
        p.size[std::size_t(axes[1])] = s.width - 2 * s.wall;
        p.size[std::size_t(axes[2])] = s.height - 2 * s.wall;
    }
    return p;
}

bool nearly_uniform(const Float3& s) {
    const double a = std::abs(double(s.x)), b = std::abs(double(s.y)), c = std::abs(double(s.z));
    const double top = (std::max)({a, b, c});
    return top > 0.0 && (std::min)({a, b, c}) >= top * (1.0 - 1.0e-6);
}

std::string profile_name(TubeProfile p) { return p == TubeProfile::Round ? "round" : p == TubeProfile::Square ? "square" : "rect"; }

bool close_enough(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

} // namespace

std::array<int, 3> tube_axes(char axis) {
    switch (axis) {
    case 'x': return {0, 1, 2};
    case 'y': return {1, 0, 2};
    case 'z': return {2, 0, 1};
    default: return {-1, -1, -1};
    }
}

std::string validate_tube_spec(const TubeSpec& s) {
    if (tube_axes(s.axis)[0] < 0) return "The tube axis must be x, y or z";
    for (const double v : {s.width, s.height, s.wall, s.length})
        if (!std::isfinite(v) || !(v > 0.0)) return "Tube sizes must be positive numbers";
    if (s.width > kMaxLength || s.height > kMaxLength || s.length > kMaxLength) return "Tube sizes above 100 m are not accepted (check the units)";
    if (s.profile != TubeProfile::Rectangular && !close_enough(s.width, s.height, 1.0e-12)) return "Round and square tubes have one outer size";
    if (s.wall < kMinWall) return "The wall must be at least 0.1 mm";
    if (s.length < kMinLength) return "The tube must be at least 1 mm long";
    const double smallest = (std::min)(s.width, s.height);
    if (!(2.0 * s.wall < smallest * 0.999)) return "The wall is too thick: it must be less than half of the smallest outer size";
    if (smallest - 2.0 * s.wall < kMinWall) return "The bore would be narrower than 0.1 mm";
    return {};
}

ExactGapParse parse_fit_up_gap(std::string_view text) {
    ExactGapParse out;
    const auto words = split_words(text);
    if (words.size() != 1) { out.error = "Type one length for the fit-up gap, in mm (0 for none), e.g. 0.5 or 1.2mm"; return out; }
    const auto value = parse_length(words[0]);
    if (!value) { out.error = "Cannot read '" + words[0] + "' as a length (mm by default; units mm, cm, m, in)"; return out; }
    if (!(*value >= 0.0)) { out.error = "The fit-up gap cannot be negative"; return out; }
    out.meters = *value;
    return out;
}

TubeSpecParse parse_tube_spec(std::string_view text) {
    TubeSpecParse out;
    const auto words = split_words(text);
    if (words.empty()) { out.error = "Type a tube such as: round 50.8 3 600 y  (outer size, wall, length in mm; axis last)"; return out; }
    TubeSpec spec;
    const std::string profile = lower_copy(words[0]);
    std::size_t sizes = 0;
    if (profile == "round" || profile == "pipe" || profile == "circle" || profile == "circular") { spec.profile = TubeProfile::Round; sizes = 3; }
    else if (profile == "square" || profile == "sq") { spec.profile = TubeProfile::Square; sizes = 3; }
    else if (profile == "rect" || profile == "rectangular" || profile == "rectangle") { spec.profile = TubeProfile::Rectangular; sizes = 4; }
    else { out.error = "The profile must be round, square or rect (got '" + words[0] + "')"; return out; }
    std::size_t index = 1;
    std::vector<double> numbers;
    while (index < words.size() && numbers.size() < sizes) {
        const auto value = parse_length(words[index]);
        if (!value) { out.error = "Cannot read '" + words[index] + "' as a length (mm by default; units mm, cm, m, in)"; return out; }
        numbers.push_back(*value);
        ++index;
    }
    if (numbers.size() < sizes) {
        out.error = spec.profile == TubeProfile::Round ? "A round tube needs: outer diameter, wall, length (e.g. round 50.8 3 600)"
            : spec.profile == TubeProfile::Square ? "A square tube needs: outer size, wall, length (e.g. square 40 2 500)"
            : "A rectangular tube needs: width, height, wall, length (e.g. rect 80x40 3 600)";
        return out;
    }
    spec.axis = 'y';
    if (index < words.size()) {
        const std::string axis = lower_copy(words[index]);
        if (axis.size() == 1 && (axis[0] == 'x' || axis[0] == 'y' || axis[0] == 'z')) { spec.axis = axis[0]; ++index; }
    }
    if (index != words.size()) { out.error = "Unexpected '" + words[index] + "' (the axis letter x, y or z goes last)"; return out; }
    spec.width = numbers[0];
    if (spec.profile == TubeProfile::Rectangular) { spec.height = numbers[1]; spec.wall = numbers[2]; spec.length = numbers[3]; }
    else { spec.height = numbers[0]; spec.wall = numbers[1]; spec.length = numbers[2]; }
    out.error = validate_tube_spec(spec);
    if (out.error.empty()) out.spec = spec;
    return out;
}

std::string format_tube_spec(const TubeSpec& s) {
    std::string out = profile_name(s.profile) + " " + mm_text(s.width);
    if (s.profile == TubeProfile::Rectangular) out += "x" + mm_text(s.height);
    out += " " + mm_text(s.wall) + " " + mm_text(s.length) + " " + std::string(1, s.axis);
    return out;
}

std::string describe_tube_spec(const TubeSpec& s) {
    std::string out = s.profile == TubeProfile::Round ? "Round tube OD " + mm_text(s.width) + " mm"
        : s.profile == TubeProfile::Square ? "Square tube " + mm_text(s.width) + " mm"
        : "Rectangular tube " + mm_text(s.width) + " x " + mm_text(s.height) + " mm";
    return out + ", " + mm_text(s.wall) + " mm wall, " + mm_text(s.length) + " mm long, along " + axis_name(s.axis);
}

double tube_cross_section_area(const TubeSpec& s) {
    if (s.profile == TubeProfile::Round) {
        const double ro = s.width / 2, ri = ro - s.wall;
        return kPi * (ro * ro - ri * ri);
    }
    return s.width * s.height - (s.width - 2 * s.wall) * (s.height - 2 * s.wall);
}

double tube_volume(const TubeSpec& s) { return tube_cross_section_area(s) * s.length; }

Component make_exact_tube_component(const TubeSpec& s, const std::string& cuts) {
    Component component;
    component.type = std::string(kExactTubeComponent);
    component.properties["profile"] = profile_name(s.profile);
    component.properties["width"] = s.width;
    component.properties["height"] = s.height;
    component.properties["wall"] = s.wall;
    component.properties["length"] = s.length;
    component.properties["axis"] = std::string(1, s.axis);
    component.properties["cuts"] = cuts;
    return component;
}

std::optional<ExactTubeRecord> exact_tube_record(const EditorObject& object) {
    for (const auto& component : object.components) {
        if (component.type != kExactTubeComponent || !component.enabled) continue;
        const auto text = [&](std::string_view key) -> std::string {
            const auto it = component.properties.find(key);
            const auto* value = it == component.properties.end() ? nullptr : std::get_if<std::string>(&it->second);
            return value ? *value : std::string();
        };
        const auto number = [&](std::string_view key) -> double {
            const auto it = component.properties.find(key);
            const auto* value = it == component.properties.end() ? nullptr : std::get_if<double>(&it->second);
            return value ? *value : 0.0;
        };
        ExactTubeRecord record;
        const std::string profile = text("profile");
        if (profile == "round") record.spec.profile = TubeProfile::Round;
        else if (profile == "square") record.spec.profile = TubeProfile::Square;
        else if (profile == "rect") record.spec.profile = TubeProfile::Rectangular;
        else return std::nullopt;
        const std::string axis = text("axis");
        record.spec.axis = axis.size() == 1 ? axis[0] : '?';
        record.spec.width = number("width");
        record.spec.height = number("height");
        record.spec.wall = number("wall");
        record.spec.length = number("length");
        record.cuts = text("cuts");
        if (!validate_tube_spec(record.spec).empty()) return std::nullopt;
        return record;
    }
    return std::nullopt;
}

std::string append_tube_cut(const std::string& before, const std::string& cut) {
    const std::string clean = sanitize_record(cut);
    return before.empty() ? clean : before + "; " + clean;
}

ExactTubeResult create_exact_tube(const std::filesystem::path& projectRoot, const TubeSpec& spec, EditorObjectId id,
                                  const ExactCadSettings& settings, const ExactCadRunner& runner) {
    ExactTubeResult out;
    out.spec = spec;
    if (const auto problem = validate_tube_spec(spec); !problem.empty()) { out.error = problem; return out; }
    ExactBooleanRequest request;
    request.a = id;
    request.operation = MeshBoolean::Difference;
    request.primitiveA = tube_outer_primitive(spec, 0.0);
    request.primitiveB = tube_bore_primitive(spec);
    request.nameA = profile_name(spec.profile) + "_tube";
    request.origin = "tube:" + format_tube_spec(spec);
    request.summaryPrefix = "Certified exact tube";
    request.worldVolumeScale = 1.0;
    auto result = run_exact_boolean(projectRoot, request, settings, runner);
    if (!result) { out.error = "Exact tube failed: " + result.edit.error; return out; }
    if (result.solids != 1 || !result.meshWatertight) {
        out.error = "Exact tube failed: brepkernel returned " + std::to_string(result.solids) + " solids" +
            (result.meshWatertight ? "" : " and a mesh that is not watertight");
        return out;
    }
    // Independent check of the certified result against the analytic volume.
    const double expected = tube_volume(spec);
    if (result.exactVolume > 0.0 && std::abs(result.exactVolume - expected) > 1.0e-6 * expected) {
        out.error = "Exact tube failed: brepkernel's volume " + number_text(result.exactVolume) + " m3 differs from the analytic " +
            number_text(expected) + " m3; nothing was created";
        return out;
    }
    out.mesh = std::move(result.edit.mesh);
    out.source = std::move(result.source);
    out.collision = std::move(result.collision);
    out.exactVolume = result.exactVolume;
    out.summary = describe_tube_spec(spec) + "; " + result.summary;
    return out;
}

// --- shared preparation ------------------------------------------------------------------------

namespace {

std::string cope_record(const std::string& target, double gapMeshMeters, double scale) {
    return "cope to " + sanitize_record(target) + ", gap " + mm_text(gapMeshMeters * scale) + " mm";
}

// Checks shared by cope and miter that need only A.
std::string check_tube_operand(const EditorObject& object, const char* what) {
    if (!object.polygon) return std::string(what) + " must be a polygon object";
    if (!object.flags.visible || object.flags.locked) return "Operands must be visible and unlocked";
    if (!exact_brep_source(object)) return object.name + " has no exact CAD source";
    return {};
}

} // namespace

ExactTubeCutRequest prepare_exact_cope(const std::filesystem::path& projectRoot, const EditorDocument& document, EditorObjectId a,
                                       EditorObjectId b, const ExactCopeOptions& options) {
    ExactTubeCutRequest out;
    out.kind = "cope";
    const auto fail = [&out](std::string message) { out.error = std::move(message); return std::move(out); };
    const auto* objectA = document.find_object(a);
    const auto* objectB = document.find_object(b);
    if (a == b || !objectA || !objectB) return fail("Choose two different tubes: A is coped to B");
    for (const auto* object : {objectA, objectB})
        if (const auto problem = check_tube_operand(*object, "A tube"); !problem.empty()) return fail(problem);
    const auto tubeB = exact_tube_record(*objectB);
    if (!tubeB) return fail(objectB->name + " has no tube record: make it with Polygon > Create Exact Tube (it gives the outer body that is cut away)");
    out.tubeA = exact_tube_record(*objectA);
    if (!(options.gapWorldMeters >= 0.0) || !std::isfinite(options.gapWorldMeters) || options.gapWorldMeters > 0.1)
        return fail("The fit-up gap must be between 0 and 100 mm");
    double gapMesh = 0.0;
    if (options.gapWorldMeters > 0.0) {
        if (!nearly_uniform(objectB->voxelScale))
            return fail(objectB->name + " is stretched: a fit-up gap is only defined for a uniformly scaled tube (use gap 0)");
        gapMesh = options.gapWorldMeters / std::abs(double(objectB->voxelScale.x));
    }
    auto request = prepare_exact_boolean(projectRoot, document, a, b, MeshBoolean::Difference);
    if (!request.error.empty()) return fail(request.error);
    request.primitiveB = tube_outer_primitive(tubeB->spec, gapMesh);
    request.origin = "cope:" + sanitize_record(objectB->name) + ":gap_mm=" + mm_text(options.gapWorldMeters);
    request.summaryPrefix = "Certified exact cope";
    out.boolean = std::move(request);
    out.gapMeters = gapMesh;
    const double scaleB = nearly_uniform(objectB->voxelScale) ? std::abs(double(objectB->voxelScale.x)) : 1.0;
    out.record = cope_record(objectB->name, gapMesh, scaleB);
    out.target = objectB->name;
    out.cutterScale = scaleB;
    out.cutterTube = tubeB->spec;
    // Equal outer sizes touch tangentially at 90 degrees: when any outer size of A matches one of
    // the cutter's (within a few clearances) a refusal is a candidate for the documented retry.
    out.tangentClearanceMeters = (std::max)(0.0, options.tangentClearanceMeters);
    bool candidate = true;
    if (out.tubeA && nearly_uniform(objectA->voxelScale) && nearly_uniform(objectB->voxelScale)) {
        candidate = false;
        const double sa = std::abs(double(objectA->voxelScale.x));
        const double band = 4.0 * out.tangentClearanceMeters + 1.0e-9;
        for (const double da : {out.tubeA->spec.width, out.tubeA->spec.height})
            for (const double db : {tubeB->spec.width, tubeB->spec.height})
                candidate = candidate || close_enough(da * sa, db * scaleB + 2.0 * options.gapWorldMeters, band);
    }
    out.tangentRetry = options.retryTangent && candidate && out.tangentClearanceMeters > 0.0;
    return out;
}

std::string validate_miter_spec(const ExactMiterSpec& m) {
    if (!std::isfinite(m.angleDegrees) || std::abs(m.angleDegrees) > 85.0) return "The cut angle must be within 85 degrees of a square cut";
    if (!std::isfinite(m.positionWorldMeters) || std::abs(m.positionWorldMeters) > kMaxLength) return "The cut position is out of range";
    if (m.tilt != 'u' && m.tilt != 'v') return "The tilt direction must be u or v";
    return {};
}

ExactMiterParse parse_miter_spec(std::string_view text) {
    ExactMiterParse out;
    const auto words = split_words(text);
    if (words.empty()) { out.error = "Type the cut angle from a square cut in degrees, e.g. 47.5  (optional: high|low, u|v, at <mm>)"; return out; }
    ExactMiterSpec spec;
    auto angle = words[0];
    if (angle.size() > 3 && lower_copy(angle).ends_with("deg")) angle.resize(angle.size() - 3);
    else if (angle.ends_with("\xC2\xB0")) angle.resize(angle.size() - 2);
    double value = 0.0;
    const auto [end, ec] = std::from_chars(angle.data(), angle.data() + angle.size(), value);
    if (ec != std::errc{} || end != angle.data() + angle.size() || !std::isfinite(value)) { out.error = "Cannot read '" + words[0] + "' as an angle in degrees"; return out; }
    spec.angleDegrees = value;
    for (std::size_t i = 1; i < words.size(); ++i) {
        const std::string w = lower_copy(words[i]);
        if (w == "high" || w == "+") spec.keep = MiterKeep::High;
        else if (w == "low" || w == "-") spec.keep = MiterKeep::Low;
        else if (w == "u" || w == "v") spec.tilt = w[0];
        else if (w == "at" && i + 1 < words.size()) {
            const auto length = parse_length(words[++i]);
            if (!length) { out.error = "Cannot read '" + words[i] + "' as a position (mm from the tube centre)"; return out; }
            spec.positionWorldMeters = *length;
        } else { out.error = "Unexpected '" + words[i] + "' (use high|low, u|v, at <mm>)"; return out; }
    }
    out.error = validate_miter_spec(spec);
    if (out.error.empty()) out.spec = spec;
    return out;
}

std::string format_miter_spec(const ExactMiterSpec& m) {
    return number_text(m.angleDegrees) + (m.keep == MiterKeep::High ? " high " : " low ") + std::string(1, m.tilt) + " at " +
           mm_text(m.positionWorldMeters);
}

MiterPlane miter_plane(const TubeSpec& spec, const ExactMiterSpec& miter, double positionMeshMeters) {
    MiterPlane plane;
    const auto axes = tube_axes(spec.axis);
    const double a = miter.angleDegrees * kPi / 180.0;
    const int tilt = miter.tilt == 'u' ? axes[1] : axes[2];
    plane.point[std::size_t(axes[0])] = positionMeshMeters;
    std::array<double, 3> n{};
    n[std::size_t(axes[0])] = std::cos(a);
    n[std::size_t(tilt)] = std::sin(a);
    const double sign = miter.keep == MiterKeep::Low ? 1.0 : -1.0;   // the normal points to the removed side
    for (std::size_t i = 0; i < 3; ++i) plane.normal[i] = sign * n[i];
    return plane;
}

ExactTubeCutRequest prepare_exact_miter(const std::filesystem::path& projectRoot, const EditorDocument& document, EditorObjectId a,
                                        const ExactMiterSpec& miter) {
    ExactTubeCutRequest out;
    out.kind = "miter";
    const auto fail = [&out](std::string message) { out.error = std::move(message); return std::move(out); };
    const auto* object = document.find_object(a);
    if (!object) return fail("Select a tube to cut");
    if (const auto problem = check_tube_operand(*object, "The tube"); !problem.empty()) return fail(problem);
    const auto tube = exact_tube_record(*object);
    if (!tube) return fail(object->name + " has no tube record: make it with Polygon > Create Exact Tube");
    if (const auto problem = validate_miter_spec(miter); !problem.empty()) return fail(problem);
    if (!nearly_uniform(object->voxelScale)) return fail(object->name + " is stretched: cut angles are only true on a uniformly scaled tube");
    const double scale = std::abs(double(object->voxelScale.x));
    const double position = miter.positionWorldMeters / scale;
    const TubeSpec& spec = tube->spec;
    // The plane must cross the tube through its side walls only, never through an end face.
    const double lean = miter.tilt == 'u' ? spec.width : spec.height;
    const double reach = 0.5 * lean * std::abs(std::tan(miter.angleDegrees * kPi / 180.0));
    if (!(std::abs(position) + reach < 0.5 * spec.length * 0.999))
        return fail("The cut plane would leave through the end of the tube: it crosses the axis at " + mm_text(position * scale) +
                    " mm and spans " + mm_text(2 * reach * scale) + " mm along it, but the half length is " + mm_text(0.5 * spec.length * scale) + " mm");
    // prepare_exact_boolean needs two operands; the checks it makes for A are repeated for this single one.
    ExactBooleanRequest boolean;
    boolean.a = a;
    boolean.b = 0;
    boolean.operation = MeshBoolean::Difference;
    const auto source = *exact_brep_source(*object);
    const auto relative = source.relativePath;
    if (relative.empty() || relative.has_root_name() || relative.has_root_directory() ||
        std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
        return fail(object->name + " has an exact CAD source outside the project; import it again");
    boolean.brepA = projectRoot / relative;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(boolean.brepA, ec)) return fail("Exact CAD source is missing: " + boolean.brepA.generic_string());
    boolean.nameA = object->name;
    const auto& mesh = *object->polygon;
    boolean.materialsFromA.materials = mesh.materials;
    boolean.materialsFromA.materialBindings = mesh.materialBindings;
    boolean.materialsFromA.images = mesh.images;
    boolean.materialsFromA.samplers = mesh.samplers;
    boolean.materialsFromA.textures = mesh.textures;
    boolean.operandStamp = exact_operand_stamp(document, a, 0);
    boolean.worldVolumeScale = std::abs(double(object->voxelScale.x) * object->voxelScale.y * object->voxelScale.z);

    // The slab: a big cube whose face lies on the plane, on the removed side.
    const auto plane = miter_plane(spec, miter, position);
    const double diagonal = std::sqrt(spec.width * spec.width + spec.height * spec.height + spec.length * spec.length);
    const double side = 2.0 * (diagonal + 2.0 * std::abs(position)) + 0.05;
    const auto axes = tube_axes(spec.axis);
    std::array<double, 3> e1{}, e3 = plane.normal;
    e1[std::size_t(miter.tilt == 'u' ? axes[2] : axes[1])] = 1.0;   // the transverse axis the plane does not lean toward
    const std::array<double, 3> e2{e3[1] * e1[2] - e3[2] * e1[1], e3[2] * e1[0] - e3[0] * e1[2], e3[0] * e1[1] - e3[1] * e1[0]};
    const std::array<double, 3> t{plane.point[0] + 0.5 * side * e3[0], plane.point[1] + 0.5 * side * e3[1], plane.point[2] + 0.5 * side * e3[2]};
    for (std::size_t row = 0; row < 3; ++row) {
        boolean.placement[row * 4 + 0] = e1[row];
        boolean.placement[row * 4 + 1] = e2[row];
        boolean.placement[row * 4 + 2] = e3[row];
        boolean.placement[row * 4 + 3] = t[row];
    }
    boolean.primitiveB.kind = ExactPrimitive::Kind::Box;
    boolean.primitiveB.size = {side, side, side};
    const std::string recordText = "plane cut " + number_text(miter.angleDegrees) + " deg from square, tilt " + std::string(1, miter.tilt) +
        ", at " + mm_text(miter.positionWorldMeters) + " mm, keep " + (miter.keep == MiterKeep::Low ? "low" : "high") + " end";
    boolean.origin = "miter:" + number_text(miter.angleDegrees) + "deg";
    boolean.summaryPrefix = "Certified exact plane cut";
    out.boolean = std::move(boolean);
    out.record = recordText;
    out.tubeA = tube;
    return out;
}

ExactTubeCutResult run_exact_tube_cut(const std::filesystem::path& projectRoot, const ExactTubeCutRequest& request,
                                      const ExactCadSettings& settings, const ExactCadRunner& runner) {
    ExactTubeCutResult out;
    out.record = request.record;
    if (!request.error.empty()) { out.boolean.edit.error = request.error; return out; }
    out.boolean = run_exact_boolean(projectRoot, request.boolean, settings, runner);
    out.appliedGapMeters = request.gapMeters;
    if (out.boolean.refused && request.tangentRetry && request.cutterTube) {
        // Equal outer sizes touch tangentially, which brepkernel refuses at zero gap. Repeat once
        // with a tiny documented clearance; the result is certified for that geometry and says so.
        out.firstRefusal = out.boolean.edit.error;
        ExactBooleanRequest retry = request.boolean;
        const double gap = request.gapMeters + request.tangentClearanceMeters;
        retry.primitiveB = tube_outer_primitive(*request.cutterTube, gap);
        auto second = run_exact_boolean(projectRoot, retry, settings, runner);
        if (second) {
            out.boolean = std::move(second);
            out.clearanceApplied = true;
            out.appliedGapMeters = gap;
            out.record = cope_record(request.target, gap, request.cutterScale) + " (automatic tangent clearance)";
        } else {
            out.boolean.edit.error = out.firstRefusal + ". Retried with " + mm_text(request.tangentClearanceMeters * request.cutterScale) +
                " mm tangent clearance: " + second.edit.error;
            if (!second.advisory.empty()) out.boolean.advisory = second.advisory;
            return out;
        }
    }
    if (!out.boolean) return out;
    auto& result = out.boolean;
    const auto reject = [&out](std::string message) { out.boolean.edit.error = std::move(message); out.boolean.refused = false; };
    if (!result.meshWatertight) { reject("The cut result's mesh is not watertight; nothing was changed"); return out; }
    if (result.solids == 0) { reject("The cut left no material; nothing was changed"); return out; }
    if (result.volumeA > 0.0 && std::abs(result.exactVolume - result.volumeA) <= 1.0e-9 * result.volumeA) {
        reject(request.kind == "cope" ? "The tubes do not overlap: there is nothing to cope; nothing was changed"
                                      : "The cut plane does not remove any material; nothing was changed");
        return out;
    }
    if (result.volumeA > 0.0 && result.exactVolume > result.volumeA * (1.0 + 1.0e-9)) {
        reject("The cut made the tube bigger than it was (brepkernel volume " + number_text(result.exactVolume) + " > " +
               number_text(result.volumeA) + " m3); nothing was changed");
        return out;
    }
    if (result.solids > 1)
        out.warning = "the cut left " + std::to_string(result.solids) + " separate pieces (the cutter passes all the way through the tube)";
    std::string note;
    if (out.clearanceApplied)
        note = "Equal outer sizes touch tangentially and brepkernel refuses that at zero gap, so this cope uses " +
            mm_text(out.appliedGapMeters * request.cutterScale) + " mm of clearance: the mating tube is " +
            mm_text(2.0 * out.appliedGapMeters * request.cutterScale) + " mm larger across than drawn. It is certified for that geometry, not for zero gap";
    result.summary += (note.empty() ? "" : "; " + note) + (out.warning.empty() ? "" : "; WARNING: " + out.warning);
    return out;
}

} // namespace dve::editor
