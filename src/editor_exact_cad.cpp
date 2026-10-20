#include "dve/editor_exact_cad.hpp"
#include "dve/collision_convex_set.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#include "dve/ai/json.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dve::editor {
namespace {

using dve::ai::JsonValue;

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

void append_bounded(std::string& out, std::string_view chunk, std::uint64_t limit) {
    if (out.size() >= limit) return;
    out.append(chunk.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), limit - out.size()))));
}

// UTF-8 text of a path for brepkernel's argv. path::string() on Windows converts through the
// ANSI code page and throws or garbles non-ASCII paths; the Windows runner widens argv as UTF-8.
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// A stored exact source must stay inside the project: relative, no root, no "..".
bool contained_relative(const std::filesystem::path& relative) {
    if (relative.empty() || relative.has_root_name() || relative.has_root_directory()) return false;
    for (const auto& part : relative) if (part == "..") return false;
    return true;
}

bool on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
#ifdef _WIN32
    const char separator = ';';
    const std::array<std::string, 3> suffixes{".exe", ".cmd", ".bat"};
#else
    const char separator = ':';
    const std::array<std::string, 1> suffixes{""};
#endif
    std::stringstream stream(path);
    std::string dir;
    while (std::getline(stream, dir, separator)) {
        if (dir.empty()) continue;
        for (const auto& suffix : suffixes) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(std::filesystem::path(dir) / (name + suffix), ec)) return true;
        }
    }
    return false;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> serial{};
        const auto stamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("dve_exact_cad_" + std::to_string(stamp) + "_" + std::to_string(serial.fetch_add(1)));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
        ok_ = !ec;
    }
    ~TemporaryDirectory() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
private:
    std::filesystem::path path_;
    bool ok_{};
};

// Shortest text that reads back to the same double (exact for the placement, short for display).
std::string format_double(double value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
    return written.ec == std::errc{} ? std::string(buffer, written.ptr) : std::string("nan");
}

// Six significant digits, for messages.
std::string display_number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(6);
    out << value;
    return out.str();
}

std::string short_output(const std::string& text) {
    std::string out;
    std::istringstream lines(text);
    std::string line;
    std::vector<std::string> kept;
    while (std::getline(lines, line))
        if (!line.empty() && line.find("pxr") == std::string::npos) kept.push_back(line);
    const std::size_t first = kept.size() > 3 ? kept.size() - 3 : 0;
    for (std::size_t i = first; i < kept.size(); ++i) out += (out.empty() ? "" : " | ") + kept[i];
    return out;
}

struct ToolRun {
    std::optional<JsonValue> report;
    int exitCode{-1};
    std::string error;
};

ToolRun run_tool(const ExactCadSettings& settings, const ExactCadRunner& runner, std::vector<std::string> arguments,
                 const std::filesystem::path& reportPath) {
    ToolRun run;
    auto argv = settings.command.empty() ? ExactCadSettings::from_environment().command : settings.command;
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    argv.push_back("--report");
    argv.push_back(utf8(reportPath));
    const auto result = runner ? runner(argv, settings.timeout, settings.maximumOutputBytes)
                               : run_exact_cad_process(argv, settings.timeout, settings.maximumOutputBytes);
    run.exitCode = result.exitCode;
    if (!result.launched) {
        run.error = "Could not start brepkernel (" + argv.front() + "): " + result.error +
            ". Install brepkernel (pip install . in its checkout) or set DVE_BREPKERNEL";
        return run;
    }
    if (result.timedOut) {
        run.error = "brepkernel did not finish within " + std::to_string(settings.timeout.count()) + " s";
        return run;
    }
    std::ifstream in(reportPath, std::ios::binary);
    if (!in) {
        run.error = "brepkernel exited with code " + std::to_string(result.exitCode) + " and wrote no report";
        const auto tail = short_output(result.output);
        if (!tail.empty()) run.error += ": " + tail;
        return run;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = dve::ai::parse_json(text);
    if (!parsed.value || !parsed.value->is_object()) {
        run.error = "brepkernel report is not valid JSON: " + parsed.error;
        return run;
    }
    const auto* schema = parsed.value->find("schema");
    if (!schema || schema->as_string() != "brepkernel.cli/1") {
        run.error = "brepkernel report has an unexpected schema (need brepkernel.cli/1)";
        return run;
    }
    run.report = std::move(parsed.value);
    return run;
}

std::string json_string(const JsonValue& root, std::initializer_list<std::string_view> path) {
    const JsonValue* node = &root;
    for (const auto key : path) {
        node = node->find(key);
        if (!node) return {};
    }
    return std::string(node->as_string());
}

double json_number(const JsonValue& root, std::initializer_list<std::string_view> path, double fallback = 0.0) {
    const JsonValue* node = &root;
    for (const auto key : path) {
        node = node->find(key);
        if (!node) return fallback;
    }
    return node->as_number(fallback);
}

std::string sanitized_stem(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) || c == '-' || c == '_') ? c : '_';
    }
    if (out.empty()) out = "part";
    if (out.size() > 48) out.resize(48);
    return out;
}

// Move a freshly written .brep into sources/exact/, named by content hash.
std::optional<ExactBrepSource> adopt_brep(const std::filesystem::path& projectRoot, const std::filesystem::path& written,
                                          const std::string& sha256, std::string_view stem, std::string origin,
                                          std::string* error) {
    if (sha256.size() < 12) { if (error) *error = "brepkernel report lacks the result hash"; return std::nullopt; }
    const auto relative = std::filesystem::path("sources") / "exact" /
        (sanitized_stem(stem) + "-" + sha256.substr(0, 12) + ".brep");
    const auto target = projectRoot / relative;
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) { if (error) *error = "Cannot create " + utf8(target.parent_path()) + ": " + ec.message(); return std::nullopt; }
    if (!std::filesystem::exists(target, ec)) {
        std::filesystem::rename(written, target, ec);
        if (ec) {                                    // different volume: copy
            ec.clear();
            std::filesystem::copy_file(written, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { if (error) *error = "Cannot store " + utf8(target) + ": " + ec.message(); return std::nullopt; }
        }
    }
    return ExactBrepSource{relative.generic_string(), sha256, std::move(origin)};
}

std::optional<CookedPolygonAsset> cook_glb(const std::filesystem::path& glb, EditorObjectId id, std::string* error) {
    PolygonCookOptions options;
    options.objectId = id;
    options.generateMissingNormals = false;     // brepkernel writes exact surface normals
    options.maximumVertices = 1000000;
    options.maximumIndices = 6000000;
    std::vector<ImportDiagnostic> diagnostics;
    auto mesh = cook_polygon_model(glb, {}, options, &diagnostics);
    mesh.objectId = id;
    const auto valid = validate_polygon_asset(mesh);
    if (!valid || mesh.indices.empty()) {
        if (error) *error = "Exact mesh did not cook: " + (valid ? std::string("no triangles") : valid.message);
        return std::nullopt;
    }
    return mesh;
}

void mesh_options(const ExactCadSettings& settings, std::vector<std::string>& args) {
    if (settings.deflectionMeters > 0.0) { args.push_back("--deflection"); args.push_back(format_double(settings.deflectionMeters)); }
    args.push_back("--angular");
    args.push_back(format_double(settings.angularRadians));
}

struct DMat3 { double m[3][3]; };

DMat3 rotation_matrix(Quaternion q) {
    const double n = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
    const double x = q.x / n, y = q.y / n, z = q.z / n, w = q.w / n;
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

} // namespace

ExactCadSettings ExactCadSettings::from_environment() {
    ExactCadSettings settings;
    if (const char* custom = std::getenv("DVE_BREPKERNEL"); custom && *custom) {
        std::istringstream words(custom);
        std::string word;
        while (words >> word) settings.command.push_back(word);
    }
    if (settings.command.empty()) {
        if (on_path("brepkernel")) settings.command = {"brepkernel"};
#ifdef _WIN32
        else settings.command = {"python", "-m", "brepkernel"};
#else
        else settings.command = {"python3", "-m", "brepkernel"};
#endif
    }
    return settings;
}

bool is_exact_cad_source(const std::filesystem::path& path) {
    const auto extension = lower(utf8(path.extension()));
    return extension == ".step" || extension == ".stp" || extension == ".brep" || extension == ".usda";
}

std::optional<ExactBrepSource> exact_brep_source(const EditorObject& object) {
    for (const auto& component : object.components) {
        if (component.type != kExactBrepComponent || !component.enabled) continue;
        const auto get = [&](std::string_view key) -> std::string {
            const auto it = component.properties.find(key);
            if (it == component.properties.end()) return {};
            if (const auto* text = std::get_if<std::string>(&it->second)) return *text;
            return {};
        };
        const auto number = [&](std::string_view key) -> double {
            const auto it = component.properties.find(key);
            if (it == component.properties.end()) return 0.0;
            if (const auto* value = std::get_if<double>(&it->second)) return std::isfinite(*value) && *value > 0.0 ? *value : 0.0;
            return 0.0;
        };
        ExactBrepSource source{get("source"), get("sha256"), get("origin"), number("deflection"), number("angular")};
        if (source.relativePath.empty()) return std::nullopt;
        return source;
    }
    return std::nullopt;
}

Component make_exact_brep_component(const ExactBrepSource& source) {
    Component component;
    component.type = std::string(kExactBrepComponent);
    component.properties["source"] = source.relativePath.generic_string();
    component.properties["sha256"] = source.sha256;
    component.properties["origin"] = source.origin;
    if (source.deflectionMeters > 0.0) component.properties["deflection"] = source.deflectionMeters;
    if (source.angularRadians > 0.0) component.properties["angular"] = source.angularRadians;
    return component;
}

std::optional<std::array<double, 12>> exact_relative_placement(const EditorDocument& document, EditorObjectId a,
                                                               EditorObjectId b) {
    const auto* objectA = document.find_object(a);
    const auto* objectB = document.find_object(b);
    if (!objectA || !objectB) return std::nullopt;
    const auto poseA = document.world_transform(a).value_or(objectA->transform);
    const auto poseB = document.world_transform(b).value_or(objectB->transform);
    const Float3 sA = objectA->voxelScale, sB = objectB->voxelScale;
    if (!(sA.x != 0 && sA.y != 0 && sA.z != 0 && sB.x != 0 && sB.y != 0 && sB.z != 0)) return std::nullopt;
    const auto RA = rotation_matrix(poseA.rotation), RB = rotation_matrix(poseB.rotation);
    const double invA[3] = {1.0 / sA.x, 1.0 / sA.y, 1.0 / sA.z};
    const double scaleB[3] = {sB.x, sB.y, sB.z};
    const double dt[3] = {double(poseB.position.x) - poseA.position.x, double(poseB.position.y) - poseA.position.y,
                          double(poseB.position.z) - poseA.position.z};
    std::array<double, 12> m{};
    for (int i = 0; i < 3; ++i) {
        // row i of diag(1/sA) * RA^T * RB * diag(sB), then the translation column
        for (int j = 0; j < 3; ++j) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += RA.m[k][i] * RB.m[k][j];
            m[std::size_t(i * 4 + j)] = invA[i] * v * scaleB[j];
        }
        double t = 0;
        for (int k = 0; k < 3; ++k) t += RA.m[k][i] * dt[k];
        m[std::size_t(i * 4 + 3)] = invA[i] * t;
    }
    for (const double v : m) if (!std::isfinite(v)) return std::nullopt;
    return m;
}

ExactCollisionResult exact_convex_collision(const std::filesystem::path& projectRoot, const ExactBrepSource& source,
                                            const ExactCadSettings& settings, const ExactCadRunner& runner) {
    ExactCollisionResult result;
    TemporaryDirectory temp;
    if (!temp.ok()) { result.note = "no convex collision: cannot create a temporary folder"; return result; }
    const auto pieces = temp.path() / "pieces.json";
    std::vector<std::string> args{"convex-pieces", utf8(projectRoot / source.relativePath), "-o", utf8(pieces),
                                  "--max-error", format_double(settings.collisionMaxError)};
    auto run = run_tool(settings, runner, std::move(args), temp.path() / "report.json");
    if (!run.report) { result.note = "no convex collision: " + run.error; return result; }
    const auto& report = *run.report;
    if (run.exitCode == 2) {
        result.refused = true;
        const auto message = json_string(report, {"certificate", "refusal", "message"});
        result.note = "collision stays one hull: " + (message.empty() ? std::string("brepkernel refused") : message);
        return result;
    }
    if (run.exitCode != 0) {
        result.note = "no convex collision: " + json_string(report, {"error"});
        return result;
    }
    std::ifstream in(pieces, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = dve::ai::parse_json(text);
    const JsonValue* list = parsed.value ? parsed.value->find("pieces") : nullptr;
    if (!list || !list->is_array()) { result.note = "no convex collision: unreadable pieces file"; return result; }
    std::vector<ConvexPrism> prisms;
    const auto ring = [](const JsonValue* value, std::vector<Float3>& out) {
        if (!value || !value->is_array()) return false;
        for (const auto& p : value->as_array()) {
            if (!p.is_array() || p.as_array().size() != 3) return false;
            const auto& c = p.as_array();
            out.push_back({float(c[0].as_number()), float(c[1].as_number()), float(c[2].as_number())});
        }
        return true;
    };
    for (const auto& item : list->as_array()) {
        ConvexPrism prism;
        if (!item.is_object() || !ring(item.find("bottom"), prism.bottom) || !ring(item.find("top"), prism.top)) {
            result.note = "no convex collision: malformed piece";
            return result;
        }
        prisms.push_back(std::move(prism));
    }
    if (!validate_convex_set(prisms)) { result.note = "no convex collision: pieces failed validation"; return result; }
    Component component;
    component.type = std::string(kConvexSetComponent);
    component.properties["pieces"] = encode_convex_set(prisms);
    component.properties["source_sha256"] = source.sha256;
    component.properties["max_error"] = settings.collisionMaxError;
    std::ostringstream summary;
    summary.imbue(std::locale::classic());
    double worst = 0;
    if (const auto* slabs = report.find("certificate") ? report.find("certificate")->find("slabs") : nullptr;
        slabs && slabs->is_array())
        for (const auto& slab : slabs->as_array())
            worst = std::max(worst, json_number(slab, {"deviation", "reference_outside_sim"}));
    summary << "certified " << prisms.size() << " convex pieces (inside the part, at most "
            << display_number(worst) << " m short of it)";
    component.properties["summary"] = summary.str();
    result.note = "exact collision: " + summary.str();
    result.component = std::move(component);
    return result;
}

ExactCadImportResult import_exact_cad(const std::filesystem::path& projectRoot, const std::filesystem::path& cadFile,
                                      EditorObjectId id, const ExactCadSettings& settings, const ExactCadRunner& runner) {
    ExactCadImportResult result;
    if (!is_exact_cad_source(cadFile)) { result.error = "Exact CAD import reads STEP, BREP and OpenUSD B-rep (.usda) files"; return result; }
    TemporaryDirectory temp;
    if (!temp.ok()) { result.error = "Cannot create a temporary folder"; return result; }
    const auto brep = temp.path() / "part.brep";
    const auto glb = temp.path() / "part.glb";
    std::vector<std::string> args{"import", utf8(cadFile), "-o", utf8(brep), "--mesh", utf8(glb)};
    mesh_options(settings, args);
    auto run = run_tool(settings, runner, std::move(args), temp.path() / "report.json");
    if (!run.report) { result.error = run.error; return result; }
    const auto& report = *run.report;
    if (run.exitCode != 0) {
        const auto message = json_string(report, {"error"});
        result.error = "Exact CAD import failed: " + (message.empty() ? "exit code " + std::to_string(run.exitCode) : message);
        return result;
    }
    std::string error;
    auto mesh = cook_glb(glb, id, &error);
    if (!mesh) { result.error = error; return result; }
    auto source = adopt_brep(projectRoot, brep, json_string(report, {"output", "sha256"}), utf8(cadFile.stem()),
                             utf8(cadFile.filename()), &error);
    if (!source) { result.error = error; return result; }
    result.mesh = std::move(*mesh);
    result.source = std::move(*source);
    result.source.deflectionMeters = json_number(report, {"mesh", "linear_deflection"});
    result.source.angularRadians = json_number(report, {"mesh", "angular_deflection"});
    if (settings.exactCollision) result.collision = exact_convex_collision(projectRoot, result.source, settings, runner);
    std::ostringstream summary;
    summary.imbue(std::locale::classic());
    summary << "Imported exact " << json_string(report, {"input", "format"}) << " (scale "
            << json_number(report, {"frame", "scale"}, 1.0) << ", " << json_string(report, {"frame", "up"}) << "-up -> Y-up), "
            << static_cast<std::uint64_t>(json_number(report, {"mesh", "triangles"})) << " triangles";
    if (!result.collision.note.empty()) summary << "; " << result.collision.note;
    result.summary = summary.str();
    return result;
}

ExactCadRemeshResult remesh_exact_cad(const std::filesystem::path& projectRoot, const ExactBrepSource& source, EditorObjectId id,
                                      double deflectionMeters, const ExactCadSettings& settings, const ExactCadRunner& runner) {
    ExactCadRemeshResult result;
    if (!contained_relative(source.relativePath)) { result.error = "The exact source path is not inside the project"; return result; }
    const auto brep = projectRoot / source.relativePath;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(brep, ec)) { result.error = "The exact source is missing: " + utf8(source.relativePath); return result; }
    if (!(deflectionMeters >= 0.0) || !std::isfinite(deflectionMeters)) { result.error = "The mesh accuracy must be a positive length"; return result; }
    TemporaryDirectory temp;
    if (!temp.ok()) { result.error = "Cannot create a temporary folder"; return result; }
    const auto glb = temp.path() / "detail.glb";
    ExactCadSettings detail = settings;
    detail.deflectionMeters = deflectionMeters;
    std::vector<std::string> args{"mesh", utf8(brep), "-o", utf8(glb)};
    mesh_options(detail, args);
    auto run = run_tool(detail, runner, std::move(args), temp.path() / "report.json");
    if (!run.report) { result.error = run.error; return result; }
    const auto& report = *run.report;
    if (run.exitCode != 0) {
        const auto message = json_string(report, {"error"});
        result.error = "Re-meshing failed: " + (message.empty() ? "exit code " + std::to_string(run.exitCode) : message);
        return result;
    }
    std::string error;
    auto mesh = cook_glb(glb, id, &error);
    if (!mesh) { result.error = error; return result; }
    result.mesh = std::move(*mesh);
    result.deflectionMeters = json_number(report, {"mesh", "linear_deflection"}, deflectionMeters);
    result.angularRadians = json_number(report, {"mesh", "angular_deflection"}, settings.angularRadians);
    result.triangles = static_cast<std::uint64_t>(json_number(report, {"mesh", "triangles"}, double(result.mesh.indices.size() / 3)));
    return result;
}

std::optional<double> exact_mesh_world_error(const EditorObject& object) {
    const auto source = exact_brep_source(object);
    if (!source || !(source->deflectionMeters > 0.0)) return std::nullopt;
    const double scale = std::max({std::abs(double(object.voxelScale.x)), std::abs(double(object.voxelScale.y)),
                                   std::abs(double(object.voxelScale.z))});
    return source->deflectionMeters * scale;
}

std::uint64_t exact_operand_stamp(const EditorDocument& document, EditorObjectId a, EditorObjectId b) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::uint64_t x) { h ^= x; h *= 1099511628211ULL; };
    const auto mix_text = [&mix](std::string_view text) { for (const char c : text) mix(static_cast<unsigned char>(c)); mix(0xFF); };
    for (const auto id : {a, b}) {
        const auto* o = document.find_object(id);
        if (!o) { mix(0); continue; }
        mix(o->polygon ? o->polygon->contentHash : 0);
        mix(o->flags.locked);
        mix(o->flags.visible);
        const auto source = exact_brep_source(*o);
        mix_text(source ? source->relativePath.generic_string() : std::string());
        const auto p = document.world_transform(id).value_or(o->transform);
        for (const float f : {p.position.x, p.position.y, p.position.z, p.rotation.x, p.rotation.y, p.rotation.z,
                              p.rotation.w, o->voxelScale.x, o->voxelScale.y, o->voxelScale.z})
            mix(std::bit_cast<std::uint32_t>(f));
    }
    return h;
}

ExactBooleanRequest prepare_exact_boolean(const std::filesystem::path& projectRoot, const EditorDocument& document,
                                          EditorObjectId a, EditorObjectId b, MeshBoolean operation) {
    ExactBooleanRequest request;
    request.a = a;
    request.b = b;
    request.operation = operation;
    const auto fail = [&request](std::string message) { request.error = std::move(message); return std::move(request); };
    if (a == b) return fail("Choose two different polygon operands");
    const auto* first = document.find_object(a);
    const auto* second = document.find_object(b);
    if (!first || !second || !first->polygon || !second->polygon) return fail("Select two polygon objects");
    for (const auto* object : {first, second}) {
        if (!object->flags.visible || object->flags.locked) return fail("Boolean operands must be visible and unlocked");
        if (!exact_brep_source(*object))
            return fail(object->name + " has no exact CAD source; import it with Polygon > Import Exact CAD");
    }
    for (const auto* object : {first, second})
        if (!contained_relative(exact_brep_source(*object)->relativePath))
            return fail(object->name + " has an exact CAD source outside the project; import it again");
    request.brepA = projectRoot / exact_brep_source(*first)->relativePath;
    request.brepB = projectRoot / exact_brep_source(*second)->relativePath;
    std::error_code ec;
    for (const auto& p : {request.brepA, request.brepB})
        if (!std::filesystem::is_regular_file(p, ec)) return fail("Exact CAD source is missing: " + utf8(p));
    const auto placement = exact_relative_placement(document, a, b);
    if (!placement) return fail("Operand transforms cannot be combined (zero scale?)");
    request.placement = *placement;
    request.nameA = first->name;
    const auto& m = *first->polygon;
    request.materialsFromA.materials = m.materials;
    request.materialsFromA.materialBindings = m.materialBindings;
    request.materialsFromA.images = m.images;
    request.materialsFromA.samplers = m.samplers;
    request.materialsFromA.textures = m.textures;
    request.operandStamp = exact_operand_stamp(document, a, b);
    request.worldVolumeScale = std::abs(double(first->voxelScale.x) * first->voxelScale.y * first->voxelScale.z);
    return request;
}

ExactBooleanResult run_exact_boolean(const std::filesystem::path& projectRoot, const ExactBooleanRequest& request,
                                     const ExactCadSettings& settings, const ExactCadRunner& runner) {
    ExactBooleanResult result;
    const auto fail = [&result](std::string message) { result.edit.error = std::move(message); return std::move(result); };
    if (!request.error.empty()) return fail(request.error);
    TemporaryDirectory temp;
    if (!temp.ok()) return fail("Cannot create a temporary folder");
    const auto brep = temp.path() / "result.brep";
    const auto glb = temp.path() / "result.glb";
    std::string matrix;
    for (std::size_t i = 0; i < request.placement.size(); ++i) matrix += (i ? "," : "") + format_double(request.placement[i]);
    const bool reverse = request.operation == MeshBoolean::ReverseDifference;
    const char* op = request.operation == MeshBoolean::Union ? "union"
        : request.operation == MeshBoolean::Intersection ? "intersection" : "difference";
    std::vector<std::string> args{"boolean", utf8(request.brepA), utf8(request.brepB), "--op", op, "-o", utf8(brep),
                                  "--b-transform", matrix, "--mesh", utf8(glb)};
    if (reverse) args.push_back("--reverse");
    if (settings.advisory) args.push_back("--advisory");
    mesh_options(settings, args);
    auto run = run_tool(settings, runner, std::move(args), temp.path() / "report.json");
    if (!run.report) return fail(run.error);
    const auto& report = *run.report;
    if (run.exitCode == 2) {
        result.refused = true;
        result.refusalKind = json_string(report, {"refusal", "kind"});
        const auto* advisory = report.find("advisory");
        if (advisory && advisory->is_object()) {
            std::ostringstream text;
            text.imbue(std::locale::classic());
            const auto triage = json_string(*advisory, {"triage"});
            text << "Uncertified second opinions:";
            if (const auto* engines = advisory->find("engines"); engines && engines->is_object())
                for (const auto& [name, engine] : engines->as_object()) {
                    const auto* volume = engine.find("volume");
                    text << " " << name << " " << (volume ? display_number(volume->as_number()) : json_string(engine, {"status"}));
                }
            if (!triage.empty()) text << " (" << triage << ")";
            result.advisory = text.str();
        }
        return fail("brepkernel refused the exact Boolean (" + result.refusalKind + "): " +
                    json_string(report, {"refusal", "message"}).substr(0, 240));
    }
    if (run.exitCode != 0) {
        const auto message = json_string(report, {"error"});
        return fail("Exact Boolean failed: " + (message.empty() ? "exit code " + std::to_string(run.exitCode) : message));
    }
    if (json_string(report, {"status"}) == "accepted_empty") {
        result.empty = true;
        return fail("The exact Boolean is empty (certified): no material is left; nothing was changed");
    }
    std::string error;
    auto mesh = cook_glb(glb, request.a, &error);
    if (!mesh) return fail(error);
    // A's materials carry over; the exact result is one surface group in A's first material.
    if (!request.materialsFromA.materials.empty()) {
        mesh->materials = request.materialsFromA.materials;
        mesh->materialBindings = request.materialsFromA.materialBindings;
        mesh->images = request.materialsFromA.images;
        mesh->samplers = request.materialsFromA.samplers;
        mesh->textures = request.materialsFromA.textures;
        for (auto& submesh : mesh->submeshes) submesh.materialIndex = 0;
    }
    mesh->contentHash = polygon_asset_content_hash(*mesh);
    if (const auto valid = validate_polygon_asset(*mesh); !valid) return fail("Exact Boolean mesh is invalid: " + valid.message);
    auto source = adopt_brep(projectRoot, brep, json_string(report, {"result", "sha256"}), request.nameA,
                             std::string("boolean:") + (reverse ? "reverse_difference" : op), &error);
    if (!source) return fail(error);
    result.edit.mesh = std::move(*mesh);
    result.edit.selection = {request.a, PolygonSelectionMode::Face, {}};
    result.source = std::move(*source);
    result.source.deflectionMeters = json_number(report, {"mesh", "linear_deflection"});
    result.source.angularRadians = json_number(report, {"mesh", "angular_deflection"});
    if (settings.exactCollision) result.collision = exact_convex_collision(projectRoot, result.source, settings, runner);
    std::ostringstream summary;
    summary.imbue(std::locale::classic());
    summary.precision(9);
    summary << "Certified exact Boolean: volume " << json_number(report, {"result", "volume"}) * request.worldVolumeScale
            << " m3 (exact, world scale), "
            << static_cast<std::uint64_t>(json_number(report, {"mesh", "triangles"})) << " triangles";
    if (!result.collision.note.empty()) summary << "; " << result.collision.note;
    result.summary = summary.str();
    return result;
}

ExactBooleanResult exact_boolean_editor_polygons(const std::filesystem::path& projectRoot, const EditorDocument& document,
                                                 EditorObjectId a, EditorObjectId b, MeshBoolean operation,
                                                 const ExactCadSettings& settings, const ExactCadRunner& runner) {
    return run_exact_boolean(projectRoot, prepare_exact_boolean(projectRoot, document, a, b, operation), settings, runner);
}

#ifndef _WIN32
ExactCadProcessResult run_exact_cad_process(const std::vector<std::string>& argv, std::chrono::seconds timeout,
                                            std::uint64_t maximumOutputBytes) {
    ExactCadProcessResult result;
    if (argv.empty()) { result.error = "empty command"; return result; }
    int pipefd[2]{};
    if (pipe(pipefd) != 0) { result.error = "could not create an output pipe"; return result; }
    int execPipe[2]{};                       // reports exec failure to the parent
    if (pipe(execPipe) != 0) { close(pipefd[0]); close(pipefd[1]); result.error = "could not create a pipe"; return result; }
    (void)fcntl(execPipe[1], F_SETFD, FD_CLOEXEC);
    std::vector<std::string> storage = argv;
    std::vector<char*> args;
    for (auto& value : storage) args.push_back(value.data());
    args.push_back(nullptr);
    const auto started = std::chrono::steady_clock::now();
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]); close(execPipe[0]); close(execPipe[1]);
        result.error = "could not start a process";
        return result;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]); close(execPipe[0]);
        execvp(args[0], args.data());
        const int code = errno;
        (void)!write(execPipe[1], &code, sizeof(code));
        _exit(127);
    }
    (void)setpgid(pid, pid);
    close(pipefd[1]);
    close(execPipe[1]);
    int execError = 0;
    const auto got = read(execPipe[0], &execError, sizeof(execError));
    close(execPipe[0]);
    if (got == sizeof(execError)) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        close(pipefd[0]);
        result.error = std::string("cannot run ") + argv.front() + ": " + std::strerror(execError);
        return result;
    }
    result.launched = true;
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    std::array<char, 8192> buffer{};
    int status = 0;
    for (;;) {
        for (;;) {
            const ssize_t count = read(pipefd[0], buffer.data(), buffer.size());
            if (count > 0) append_bounded(result.output, std::string_view(buffer.data(), std::size_t(count)), maximumOutputBytes);
            else break;
        }
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        if (std::chrono::steady_clock::now() - started >= timeout) {
            result.timedOut = true;
            (void)kill(-pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (waitpid(pid, &status, WNOHANG) == 0) { (void)kill(-pid, SIGKILL); (void)waitpid(pid, &status, 0); }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (;;) {
        const ssize_t count = read(pipefd[0], buffer.data(), buffer.size());
        if (count > 0) append_bounded(result.output, std::string_view(buffer.data(), std::size_t(count)), maximumOutputBytes);
        else break;
    }
    close(pipefd[0]);
    if (result.timedOut) { result.exitCode = -1; return result; }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    return result;
}
#else
namespace {
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(std::size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}
// Quote one argument for CommandLineToArgvW / the MSVC runtime.
std::wstring quote_argument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++slashes; }
        if (i == arg.size()) { out.append(slashes * 2, L'\\'); break; }
        if (arg[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); }
        else { out.append(slashes, L'\\'); out.push_back(arg[i]); }
    }
    out.push_back(L'"');
    return out;
}
} // namespace

ExactCadProcessResult run_exact_cad_process(const std::vector<std::string>& argv, std::chrono::seconds timeout,
                                            std::uint64_t maximumOutputBytes) {
    ExactCadProcessResult result;
    if (argv.empty()) { result.error = "empty command"; return result; }
    TemporaryDirectory temp;
    if (!temp.ok()) { result.error = "could not create a temporary folder"; return result; }
    const auto logPath = temp.path() / "output.txt";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (log == INVALID_HANDLE_VALUE) { result.error = "could not create an output file"; return result; }
    std::wstring commandLine;
    for (const auto& arg : argv) commandLine += (commandLine.empty() ? L"" : L" ") + quote_argument(widen(arg));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(log);
        result.error = "cannot run " + argv.front() + " (Windows error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    result.launched = true;
    const DWORD wait = WaitForSingleObject(pi.hProcess, DWORD(std::min<long long>(timeout.count() * 1000LL, 0x7FFFFFFF)));
    if (wait == WAIT_TIMEOUT) { result.timedOut = true; TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(log);
    std::ifstream in(logPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    append_bounded(result.output, text, maximumOutputBytes);
    result.exitCode = result.timedOut ? -1 : int(code);
    return result;
}
#endif

} // namespace dve::editor

namespace dve::editor {
namespace {
ExactPoint json_point(const JsonValue* node) {
    ExactPoint p{};
    if (!node || !node->is_array()) return p;
    const auto& a = node->as_array();
    for (std::size_t i = 0; i < 3 && i < a.size(); ++i) p[i] = a[i].as_number(0.0);
    return p;
}
double field(const JsonValue& node, std::string_view key) {
    const JsonValue* v = node.find(key);
    return v ? v->as_number(0.0) : 0.0;
}
std::string inspect_error(const std::string& command, const ToolRun& run, bool& unsupported) {
    if (run.error.find("invalid choice") != std::string::npos) {
        unsupported = true;
        return "The installed brepkernel has no `" + command + "` command: update brepkernel for exact measuring, snapping and STEP export";
    }
    if (!run.report) return run.error;
    const auto message = json_string(*run.report, {"error"});
    return "brepkernel " + command + " failed: " + (message.empty() ? "exit code " + std::to_string(run.exitCode) : message);
}
bool source_file(const std::filesystem::path& root, const ExactBrepSource& source, std::filesystem::path& out, std::string& error) {
    if (!contained_relative(source.relativePath)) { error = "The exact source path is not inside the project"; return false; }
    out = root / source.relativePath;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(out, ec)) { error = "The exact source is missing: " + utf8(source.relativePath); return false; }
    return true;
}
}  // namespace

ExactCadDescription describe_exact_cad(const std::filesystem::path& projectRoot, const ExactBrepSource& source,
                                       const ExactCadSettings& settings, const ExactCadRunner& runner) {
    ExactCadDescription result;
    std::filesystem::path brep;
    if (!source_file(projectRoot, source, brep, result.error)) return result;
    TemporaryDirectory temp;
    if (!temp.ok()) { result.error = "Cannot create a temporary folder"; return result; }
    const auto run = run_tool(settings, runner, {"describe", utf8(brep)}, temp.path() / "report.json");
    const JsonValue* d = run.report && run.exitCode == 0 ? run.report->find("description") : nullptr;
    if (!d || !d->is_object()) { result.error = inspect_error("describe", run, result.unsupported); return result; }
    result.volume = field(*d, "volume");
    result.area = field(*d, "area");
    if (const JsonValue* faces = d->find("faces"); faces && faces->is_array())
        for (const JsonValue& f : faces->as_array()) {
            ExactCadFace face;
            face.index = static_cast<int>(field(f, "index"));
            if (const JsonValue* t = f.find("type")) face.type = std::string(t->as_string());
            face.area = field(f, "area");
            face.origin = json_point(f.find("origin"));
            face.normal = json_point(f.find("normal"));
            face.axisOrigin = json_point(f.find("axis_origin"));
            face.axis = json_point(f.find("axis"));
            face.centre = json_point(f.find("centre"));
            face.radius = field(f, "radius");
            face.majorRadius = field(f, "major_radius");
            face.minorRadius = field(f, "minor_radius");
            face.halfAngle = field(f, "half_angle");
            if (const JsonValue* h = f.find("hole")) face.hole = h->as_bool(false);
            result.faces.push_back(std::move(face));
        }
    if (const JsonValue* vertices = d->find("vertices"); vertices && vertices->is_array())
        for (const JsonValue& v : vertices->as_array()) result.vertices.push_back(json_point(&v));
    if (const JsonValue* edges = d->find("edges"); edges && edges->is_array())   // newer brepkernel only
        for (const JsonValue& e : edges->as_array()) {
            ExactCadEdge edge;
            if (const JsonValue* t = e.find("type")) edge.type = std::string(t->as_string());
            edge.start = json_point(e.find("start"));
            edge.end = json_point(e.find("end"));
            edge.mid = json_point(e.find("mid"));
            edge.length = field(e, "length");
            result.edges.push_back(std::move(edge));
        }
    if (const JsonValue* circles = d->find("circles"); circles && circles->is_array())
        for (const JsonValue& c : circles->as_array()) {
            ExactCadCircle circle;
            circle.centre = json_point(c.find("centre"));
            circle.axis = json_point(c.find("axis"));
            circle.radius = field(c, "radius");
            if (const JsonValue* full = c.find("full")) circle.full = full->as_bool(false);
            result.circles.push_back(circle);
        }
    return result;
}

ExactCadQueryResult query_exact_cad(const std::filesystem::path& projectRoot, const ExactBrepSource& source,
                                    const std::vector<ExactPoint>& points, const ExactCadSettings& settings,
                                    const ExactCadRunner& runner) {
    ExactCadQueryResult result;
    std::filesystem::path brep;
    if (!source_file(projectRoot, source, brep, result.error)) return result;
    if (points.empty()) { result.error = "No points to look up"; return result; }
    TemporaryDirectory temp;
    if (!temp.ok()) { result.error = "Cannot create a temporary folder"; return result; }
    std::vector<std::string> args{"query", utf8(brep)};
    for (const ExactPoint& p : points) {
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) { result.error = "A point is not finite"; return result; }
        args.insert(args.end(), {"--point", format_double(p[0]), format_double(p[1]), format_double(p[2])});
    }
    const auto run = run_tool(settings, runner, std::move(args), temp.path() / "report.json");
    const JsonValue* hits = run.report && run.exitCode == 0 ? run.report->find("results") : nullptr;
    if (!hits || !hits->is_array() || hits->as_array().size() != points.size()) {
        result.error = run.report && run.exitCode == 0 ? "brepkernel query returned the wrong number of results"
                                                       : inspect_error("query", run, result.unsupported);
        return result;
    }
    for (const JsonValue& h : hits->as_array()) {
        ExactCadQueryHit hit;
        if (const JsonValue* found = h.find("found")) hit.found = found->as_bool(false);
        hit.closest = json_point(h.find("closest"));
        hit.normal = json_point(h.find("normal"));
        hit.distance = field(h, "distance");
        hit.face = h.find("face") && h.find("face")->is_number() ? static_cast<int>(field(h, "face")) : -1;
        result.hits.push_back(hit);
    }
    return result;
}

std::string describe_exact_face(const ExactCadFace& face, double scale) {
    char buffer[96];
    const auto mm = [&](double metres) { return metres * scale * 1000.0; };
    if (face.type == "plane") return "Plane";
    if (face.type == "cylinder") {
        std::snprintf(buffer, sizeof buffer, "%s \xC3\x98 %.3f mm", face.hole ? "Hole" : "Cylinder", mm(2.0 * face.radius));
        return buffer;
    }
    if (face.type == "sphere") { std::snprintf(buffer, sizeof buffer, "Sphere \xC3\x98 %.3f mm", mm(2.0 * face.radius)); return buffer; }
    if (face.type == "cone") {
        std::snprintf(buffer, sizeof buffer, "Cone %.2f\xC2\xB0 (included %.2f\xC2\xB0)", face.halfAngle * 57.29577951308232,
                      2.0 * face.halfAngle * 57.29577951308232);
        return buffer;
    }
    if (face.type == "torus") {
        std::snprintf(buffer, sizeof buffer, "Torus R %.3f mm, r %.3f mm", mm(face.majorRadius), mm(face.minorRadius));
        return buffer;
    }
    if (face.type == "bspline" || face.type == "bezier") return "Freeform surface (NURBS)";
    return "Surface";
}

}  // namespace dve::editor
