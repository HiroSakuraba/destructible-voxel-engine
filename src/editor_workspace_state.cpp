#include "dve/editor_workspace_state.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dve::editor {
namespace detail {
bool replace_workspace_file(const std::filesystem::path& temporary, const std::filesystem::path& destination);
}
namespace {
constexpr std::size_t maximumBytes = 32U * 1024U;
bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}
bool relative_reference(const std::filesystem::path& path) {
    const auto text = path.generic_string();
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() || text.size() > 2048U ||
        text.find_first_of("\r\n") != std::string::npos || text.find('\0') != std::string::npos) return false;
    for (const auto& part : path) if (part == "..") return false;
    return true;
}
bool valid_camera(const EditorCamera& c) {
    const auto bounded = [](Float3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
               std::abs(v.x) <= 1.0e7F && std::abs(v.y) <= 1.0e7F && std::abs(v.z) <= 1.0e7F;
    };
    if (!bounded(c.position) || !bounded(c.target) || !bounded(c.worldUp) ||
        !std::isfinite(c.orthographicHeight) || c.orthographicHeight < 0.001F ||
        c.orthographicHeight > 1.0e7F) return false;
    const double dx = c.target.x - c.position.x, dy = c.target.y - c.position.y,
                 dz = c.target.z - c.position.z;
    const double ux = c.worldUp.x, uy = c.worldUp.y, uz = c.worldUp.z;
    const double view2 = dx*dx + dy*dy + dz*dz, up2 = ux*ux + uy*uy + uz*uz;
    const double dot = dx*ux + dy*uy + dz*uz;
    return view2 > 1.0e-8 && up2 > 1.0e-8 && view2*up2 - dot*dot > view2*up2*1.0e-8 &&
        (c.projection == EditorProjection::Perspective || c.projection == EditorProjection::Orthographic);
}
bool end_of_entry(std::istringstream& in) { in >> std::ws; return in.eof(); }
} // namespace

std::string EditorWorkspaceState::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "DVE_WORKSPACE 1\n" << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (const auto& [id, visible] : panels)
        if (id <= PanelId::Assistant) out << "panel " << static_cast<unsigned>(id) << ' ' << visible << '\n';
    if (bottomTab && *bottomTab <= 5U) out << "bottom_tab " << *bottomTab << '\n';
    if (camera && valid_camera(*camera)) {
        const auto& c = *camera;
        out << "camera " << c.position.x << ' ' << c.position.y << ' ' << c.position.z << ' '
            << c.target.x << ' ' << c.target.y << ' ' << c.target.z << ' '
            << c.worldUp.x << ' ' << c.worldUp.y << ' ' << c.worldUp.z << ' '
            << static_cast<unsigned>(c.projection) << ' ' << c.orthographicHeight << '\n';
    }
    if (relative_reference(selectedAsset)) out << "selected_asset " << std::quoted(selectedAsset.generic_string()) << '\n';
    if (relative_reference(openSprite)) out << "open_sprite " << std::quoted(openSprite.generic_string()) << '\n';
    return out.str();
}

std::optional<EditorWorkspaceState> EditorWorkspaceState::parse(std::string_view text, std::string* error) {
    if (text.size() > maximumBytes) { fail(error, "workspace record exceeds 32 KiB"); return {}; }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(input, line)) { fail(error, "empty workspace record"); return {}; }
    std::istringstream header(line);
    std::string magic; unsigned version{};
    if (!(header >> magic >> version) || magic != "DVE_WORKSPACE" || version != 1U || !end_of_entry(header)) {
        fail(error, "unsupported workspace record version"); return {};
    }
    EditorWorkspaceState state;
    while (std::getline(input, line)) {
        if (line.size() > 4096U) continue;
        std::istringstream entry(line);
        entry.imbue(std::locale::classic());
        std::string key;
        entry >> key;
        if (key == "panel") {
            unsigned id{}, visible{};
            if (entry >> id >> visible && id <= static_cast<unsigned>(PanelId::Assistant) && visible <= 1U && end_of_entry(entry))
                state.panels.insert_or_assign(static_cast<PanelId>(id), visible != 0);
        } else if (key == "bottom_tab") {
            unsigned tab{};
            if (entry >> tab && tab <= 5U && end_of_entry(entry)) state.bottomTab = tab;
        } else if (key == "camera") {
            EditorCamera c; unsigned projection{};
            if (entry >> c.position.x >> c.position.y >> c.position.z >> c.target.x >> c.target.y >> c.target.z >>
                c.worldUp.x >> c.worldUp.y >> c.worldUp.z >> projection >> c.orthographicHeight && projection <= 1U && end_of_entry(entry)) {
                c.projection = static_cast<EditorProjection>(projection);
                if (valid_camera(c)) state.camera = c;
            }
        } else if (key == "selected_asset" || key == "open_sprite") {
            std::string reference;
            if (entry >> std::quoted(reference) && end_of_entry(entry) && relative_reference(reference))
                (key == "selected_asset" ? state.selectedAsset : state.openSprite) = reference;
        }
        // Unknown keys and invalid entries do not block restoration of other fields.
    }
    return state;
}

bool EditorWorkspaceState::save(const std::filesystem::path& path, std::string* error) const {
    const std::string text = serialize();
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create workspace-state directory: " + ec.message());
    auto temporary = path; temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) { std::filesystem::remove(temporary, ec); return fail(error, "could not write workspace record"); }
    }
    const bool replaced = detail::replace_workspace_file(temporary, path);
    if (!replaced) { std::filesystem::remove(temporary, ec); return fail(error, "could not replace workspace record"); }
    return true;
}

std::optional<EditorWorkspaceState> EditorWorkspaceState::load(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { fail(error, "could not open workspace record"); return {}; }
    // Bound the actual read, rather than trusting a file size queried before opening.
    std::string text(maximumBytes + 1U, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    if (in.bad()) { fail(error, "could not read workspace record"); return {}; }
    return parse(text, error);
}

std::optional<std::filesystem::path> resolve_workspace_asset(
    const std::filesystem::path& projectRoot, const std::filesystem::path& reference) {
    if (!relative_reference(reference)) return {};
    std::error_code ec;
    const auto root = std::filesystem::canonical(projectRoot, ec);
    if (ec) return {};
    const auto resolved = std::filesystem::canonical(root / reference, ec);
    if (ec || !std::filesystem::is_regular_file(resolved, ec) || ec) return {};
    const auto relative = resolved.lexically_relative(root);
    if (!relative_reference(relative)) return {};
    return resolved;
}

} // namespace dve::editor
