#include "dve/editor_control_rig.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace dve::editor {
namespace {

float point_segment_distance(float px, float py, float ax, float ay, float bx, float by) noexcept {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared < 0.0001F) return std::hypot(px - ax, py - ay);
    const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / lengthSquared, 0.0F, 1.0F);
    return std::hypot(px - ax - t * dx, py - ay - t * dy);
}

std::string node_kind_name(ControlRigNodeKind kind) {
    switch (kind) {
        case ControlRigNodeKind::SetBoneTransform: return "Set Bone Transform";
        case ControlRigNodeKind::CopyBoneTransform: return "Copy Bone Transform";
        case ControlRigNodeKind::ParentConstraint: return "Parent Constraint";
        case ControlRigNodeKind::AimConstraint: return "Aim Constraint";
        case ControlRigNodeKind::TwoBoneIk: return "Two Bone IK";
        case ControlRigNodeKind::Fabrik: return "FABRIK";
    }
    return "Rig Node";
}

std::string phase_name(ControlRigSolvePhase phase) {
    switch (phase) {
        case ControlRigSolvePhase::PreSolve: return "Pre Solve";
        case ControlRigSolvePhase::ForwardSolve: return "Forward Solve";
        case ControlRigSolvePhase::PostSolve: return "Post Solve";
    }
    return "Solve";
}

std::string control_kind_name(ControlRigControlKind kind) {
    switch (kind) {
        case ControlRigControlKind::Transform: return "Transform Control";
        case ControlRigControlKind::Translation: return "Translation Control";
        case ControlRigControlKind::Rotation: return "Rotation Control";
    }
    return "Control";
}

std::string control_space_name(ControlRigSpace space) {
    switch (space) {
        case ControlRigSpace::Model: return "Model";
        case ControlRigSpace::Bone: return "Bone";
        case ControlRigSpace::Control: return "Control";
    }
    return "Space";
}

std::string control_shape_name(ControlRigControlShape shape) {
    switch (shape) {
        case ControlRigControlShape::Cross: return "Cross";
        case ControlRigControlShape::Box: return "Box";
        case ControlRigControlShape::Circle: return "Circle";
        case ControlRigControlShape::Sphere: return "Sphere";
        case ControlRigControlShape::Arrow: return "Arrow";
    }
    return "Shape";
}

std::string compact_float(float value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    std::string result = stream.str();
    while (result.size() > 1U && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result;
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

int cycle_index(int current, int count, int direction) noexcept {
    if (count <= 0) return 0;
    const int delta = direction < 0 ? -1 : 1;
    return (current + delta + count) % count;
}

bool overlaps(UiRect a, UiRect b) noexcept {
    return a.x < b.x + b.width && a.x + a.width > b.x &&
           a.y < b.y + b.height && a.y + a.height > b.y;
}

std::uint64_t next_control_id(const ControlRigAsset& rig) {
    std::uint64_t result = 1U;
    for (const auto& control : rig.controls) result = std::max(result, control.id + 1U);
    return result;
}

std::uint64_t next_node_id(const ControlRigAsset& rig) {
    std::uint64_t result = 1U;
    for (const auto& node : rig.nodes) result = std::max(result, node.id + 1U);
    return result;
}

std::filesystem::path transaction_sibling(const std::filesystem::path& path, std::string_view suffix) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return path.parent_path() / (path.filename().string() + "." + std::string(suffix) + "." + std::to_string(ticks));
}

bool rename_file(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) return true;
    error = "could not rename " + from.filename().string() + " to " + to.filename().string() + ": " + ec.message();
    return false;
}

void remove_file(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

ControlRigPairSaveResult save_control_rig_pair_transactional(
    const ControlRigAssetPaths& paths, const SkeletonAsset& skeleton,
    const ControlRigAuthoringDocument& document, ControlRigPairSaveOptions options) {
    ControlRigPairSaveResult result;
    if (paths.rig.empty() || paths.layout.empty() || paths.rig == paths.layout) {
        result.error = "control rig runtime and layout paths must be distinct";
        return result;
    }
    ControlRigAsset compiled;
    if (!document.compile(skeleton, compiled, &result.error)) return result;
    ControlRigAuthoringDocument persisted = document;
    persisted.rig = compiled;
    persisted.selectedControls.clear();
    persisted.selectedNodes.clear();
    const auto rigStage = transaction_sibling(paths.rig, "dve_pair_stage");
    const auto layoutStage = transaction_sibling(paths.layout, "dve_pair_stage");
    const auto rigBackup = transaction_sibling(paths.rig, "dve_pair_backup");
    const auto layoutBackup = transaction_sibling(paths.layout, "dve_pair_backup");
    const auto cleanup = [&]() {
        remove_file(rigStage); remove_file(layoutStage);
        remove_file(rigBackup); remove_file(layoutBackup);
    };
    std::error_code directoryError;
    if (!paths.rig.parent_path().empty()) std::filesystem::create_directories(paths.rig.parent_path(), directoryError);
    if (!directoryError && !paths.layout.parent_path().empty())
        std::filesystem::create_directories(paths.layout.parent_path(), directoryError);
    if (directoryError) {
        result.error = "could not create control rig asset directory: " + directoryError.message();
        cleanup();
        return result;
    }
    if (!write_dvecontrolrig(rigStage, skeleton, compiled, &result.error) ||
        !write_control_rig_authoring_layout(layoutStage, persisted, &result.error)) {
        cleanup();
        return result;
    }
    const ControlRigReadResult verifiedRig = read_dvecontrolrig(rigStage);
    ControlRigAuthoringDocument verifiedLayout;
    if (!verifiedRig || verifiedRig.asset.contentHash != compiled.contentHash ||
        !read_control_rig_authoring_layout(layoutStage, verifiedRig.asset, verifiedLayout, &result.error)) {
        if (result.error.empty()) result.error = verifiedRig ? "staged control rig hash is invalid" : verifiedRig.error;
        cleanup();
        return result;
    }
    const bool hadRig = std::filesystem::exists(paths.rig);
    const bool hadLayout = std::filesystem::exists(paths.layout);
    std::string commitError;
    if ((hadRig && !rename_file(paths.rig, rigBackup, commitError)) ||
        (hadLayout && !rename_file(paths.layout, layoutBackup, commitError))) {
        bool restored = true;
        std::string restoreError;
        if (hadRig && std::filesystem::exists(rigBackup))
            restored = rename_file(rigBackup, paths.rig, restoreError) && restored;
        if (hadLayout && std::filesystem::exists(layoutBackup))
            restored = rename_file(layoutBackup, paths.layout, restoreError) && restored;
        remove_file(rigStage); remove_file(layoutStage);
        if (restored) { remove_file(rigBackup); remove_file(layoutBackup); }
        result.error = commitError;
        if (!restored) result.error += "; rollback failed; backup files were retained: " + restoreError;
        return result;
    }
    bool rigCommitted = rename_file(rigStage, paths.rig, commitError);
    bool layoutCommitted = false;
    if (rigCommitted && !options.failAfterRuntimeCommit)
        layoutCommitted = rename_file(layoutStage, paths.layout, commitError);
    else if (rigCommitted) commitError = "injected failure after runtime rig commit";
    if (!rigCommitted || !layoutCommitted) {
        remove_file(paths.rig);
        remove_file(paths.layout);
        std::string restoreError;
        bool restored = true;
        if (hadRig) restored = rename_file(rigBackup, paths.rig, restoreError) && restored;
        if (hadLayout) restored = rename_file(layoutBackup, paths.layout, restoreError) && restored;
        remove_file(rigStage); remove_file(layoutStage);
        if (restored) { remove_file(rigBackup); remove_file(layoutBackup); }
        result.error = commitError;
        if (!restored) result.error += "; rollback failed; backup files were retained: " + restoreError;
        return result;
    }
    cleanup();
    result.success = true;
    result.rigHash = compiled.contentHash;
    return result;
}

bool load_control_rig_pair(const ControlRigAssetPaths& paths, const SkeletonAsset& skeleton,
                           ControlRigAuthoringDocument& document, std::string* error,
                           bool layoutOptional) {
    const ControlRigReadResult loadedRig = read_dvecontrolrig(paths.rig);
    if (!loadedRig) {
        if (error) *error = loadedRig.error;
        return false;
    }
    const AnimationValidationResult validation = validate_control_rig(skeleton, loadedRig.asset);
    if (!validation) {
        if (error) *error = validation.message;
        return false;
    }
    ControlRigAuthoringDocument loaded;
    loaded.rig = loadedRig.asset;
    if (!paths.layout.empty() && std::filesystem::exists(paths.layout)) {
        if (!read_control_rig_authoring_layout(paths.layout, loadedRig.asset, loaded, error)) return false;
    } else if (!layoutOptional) {
        if (error) *error = "control rig layout file is missing";
        return false;
    } else {
        std::size_t index{};
        for (const auto& control : loaded.rig.controls) {
            loaded.controlLayouts[control.id].position = {30.0F, 40.0F + static_cast<float>(index++) * 150.0F};
            loaded.controlVisuals[control.id] = {};
        }
        index = 0U;
        for (const auto& node : loaded.rig.nodes)
            loaded.nodeLayouts[node.id].position = {380.0F, 40.0F + static_cast<float>(index++) * 150.0F};
    }
    loaded.revision = 0U;
    loaded.selectedControls.clear();
    loaded.selectedNodes.clear();
    document = std::move(loaded);
    return true;
}

EditorControlRigPanel::EditorControlRigPanel() {
    auto initial = std::make_unique<DocumentState>();
    documents_.push_back(std::move(initial));
}

EditorControlRigPanel::DocumentState& EditorControlRigPanel::active_document() noexcept {
    return *documents_[activeDocument_];
}

const EditorControlRigPanel::DocumentState& EditorControlRigPanel::active_document() const noexcept {
    return *documents_[activeDocument_];
}

ControlRigAuthoringSession& EditorControlRigPanel::session() noexcept { return active_document().session; }
const ControlRigAuthoringSession& EditorControlRigPanel::session() const noexcept { return active_document().session; }
const SkeletonAsset& EditorControlRigPanel::skeleton() const noexcept { return active_document().skeleton; }

void EditorControlRigPanel::open_document(SkeletonAsset skeleton, ControlRigAuthoringDocument document) {
    documents_.clear();
    auto state = std::make_unique<DocumentState>();
    state->skeleton = std::move(skeleton);
    state->session = ControlRigAuthoringSession(std::move(document));
    state->savedRevision = state->session.document().revision;
    state->autosavedRevision = state->savedRevision;
    documents_.push_back(std::move(state));
    activeDocument_ = 0U;
    zoom_ = 1.0F;
    pan_ = {24.0F, 24.0F};
    capture_ = Capture::NoCapture;
    contextOpen_ = false;
    previewSelection_.reset();
    cancel_inspector_edit();
    open_ = true;
    set_status("Control Rig document opened");
}

void EditorControlRigPanel::add_recent(const std::filesystem::path& path) {
    if (path.empty()) return;
    const auto normalized = path.lexically_normal();
    recentDocuments_.erase(std::remove(recentDocuments_.begin(), recentDocuments_.end(), normalized),
                           recentDocuments_.end());
    recentDocuments_.insert(recentDocuments_.begin(), normalized);
    if (recentDocuments_.size() > 12U) recentDocuments_.resize(12U);
}

ControlRigAssetPaths EditorControlRigPanel::autosave_paths(const ControlRigAssetPaths& paths) {
    ControlRigAssetPaths result = paths;
    if (!paths.rig.empty()) result.rig = paths.rig.parent_path() / (paths.rig.filename().string() + ".autosave");
    if (!paths.layout.empty()) result.layout = paths.layout.parent_path() / (paths.layout.filename().string() + ".autosave");
    return result;
}

bool EditorControlRigPanel::open_asset(ControlRigAssetPaths paths, bool recoverIfNewer, std::string* error) {
    paths.rig = paths.rig.lexically_normal();
    if (paths.layout.empty()) {
        paths.layout = paths.rig;
        paths.layout.replace_extension(".dverigui");
    }
    paths.layout = paths.layout.lexically_normal();
    if (paths.skeleton.empty()) {
        paths.skeleton = paths.rig;
        paths.skeleton.replace_extension(".dveskeleton");
    }
    paths.skeleton = paths.skeleton.lexically_normal();
    for (std::size_t index = 0; index < documents_.size(); ++index) {
        if (!documents_[index]->paths.rig.empty() && documents_[index]->paths.rig == paths.rig) {
            (void)switch_tab(index);
            open_ = true;
            add_recent(paths.rig);
            set_status("Control Rig tab activated");
            return true;
        }
    }
    const SkeletonReadResult loadedSkeleton = read_dveskeleton(paths.skeleton);
    if (!loadedSkeleton) {
        if (error) *error = loadedSkeleton.error.empty() ? "could not load control rig skeleton" : loadedSkeleton.error;
        return false;
    }
    ControlRigAuthoringDocument document;
    if (!load_control_rig_pair(paths, loadedSkeleton.asset, document, error, true)) return false;
    bool recovered = false;
    if (recoverIfNewer) {
        const ControlRigAssetPaths recovery = autosave_paths(paths);
        std::error_code timeError;
        if (std::filesystem::exists(recovery.rig) && std::filesystem::exists(recovery.layout)) {
            const auto recoveryTime = std::filesystem::last_write_time(recovery.rig, timeError);
            const auto savedTime = std::filesystem::last_write_time(paths.rig, timeError);
            if (!timeError && recoveryTime > savedTime) {
                ControlRigAuthoringDocument recoveredDocument;
                std::string recoveryError;
                if (load_control_rig_pair(recovery, loadedSkeleton.asset, recoveredDocument, &recoveryError, false)) {
                    document = std::move(recoveredDocument);
                    recovered = true;
                }
            }
        }
    }
    auto state = std::make_unique<DocumentState>();
    state->skeleton = loadedSkeleton.asset;
    state->session = ControlRigAuthoringSession(std::move(document));
    state->paths = std::move(paths);
    state->savedRevision = state->session.document().revision;
    state->autosavedRevision = state->savedRevision;
    state->forceDirty = recovered;
    state->recovered = recovered;
    const bool replaceInitial = documents_.size() == 1U && documents_.front()->paths.rig.empty() &&
        documents_.front()->session.document().rig.controls.empty() &&
        documents_.front()->session.document().rig.nodes.empty();
    if (replaceInitial) documents_.front() = std::move(state);
    else documents_.push_back(std::move(state));
    activeDocument_ = replaceInitial ? 0U : documents_.size() - 1U;
    add_recent(active_document().paths.rig);
    zoom_ = 1.0F;
    pan_ = {24.0F, 24.0F};
    capture_ = Capture::NoCapture;
    contextOpen_ = false;
    previewSelection_.reset();
    cancel_inspector_edit();
    open_ = true;
    set_status(recovered ? "Recovered newer Control Rig autosave" : "Control Rig asset opened");
    return true;
}

std::vector<ControlRigEditorTabView> EditorControlRigPanel::tab_views() const {
    std::vector<ControlRigEditorTabView> result;
    result.reserve(documents_.size());
    for (std::size_t index = 0; index < documents_.size(); ++index) {
        const auto& state = *documents_[index];
        std::string title = state.paths.rig.empty() ? state.session.document().rig.name
                                                    : state.paths.rig.filename().string();
        if (title.empty()) title = "Untitled Rig";
        result.push_back({index, std::move(title), {}, index == activeDocument_, dirty(index), state.recovered});
    }
    return result;
}

bool EditorControlRigPanel::switch_tab(std::size_t index) noexcept {
    if (index >= documents_.size()) return false;
    activeDocument_ = index;
    capture_ = Capture::NoCapture;
    dragPin_.reset();
    contextOpen_ = false;
    previewSelection_.reset();
    cancel_inspector_edit();
    set_status("Control Rig tab activated");
    return true;
}

bool EditorControlRigPanel::dirty(std::size_t index) const noexcept {
    if (index >= documents_.size()) return false;
    const auto& state = *documents_[index];
    return state.forceDirty || state.session.document().revision != state.savedRevision;
}

bool EditorControlRigPanel::close_tab(std::size_t index, bool discardChanges, std::string* error) {
    if (index >= documents_.size()) {
        if (error) *error = "control rig tab index is invalid";
        return false;
    }
    if (dirty(index) && !discardChanges) {
        if (error) *error = "control rig tab has unsaved changes";
        return false;
    }
    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
    if (documents_.empty()) {
        documents_.push_back(std::make_unique<DocumentState>());
        activeDocument_ = 0U;
        open_ = false;
    } else if (activeDocument_ >= documents_.size()) activeDocument_ = documents_.size() - 1U;
    else if (index < activeDocument_) --activeDocument_;
    cancel_inspector_edit();
    set_status("Control Rig tab closed");
    return true;
}

bool EditorControlRigPanel::save_active(std::string* error, ControlRigPairSaveOptions options) {
    if (active_document().paths.rig.empty() || active_document().paths.layout.empty()) {
        if (error) *error = "control rig tab has no asset paths; use Save As";
        return false;
    }
    return save_active_as(active_document().paths, error, options);
}

bool EditorControlRigPanel::save_active_as(ControlRigAssetPaths paths, std::string* error,
                                           ControlRigPairSaveOptions options) {
    if (paths.layout.empty() && !paths.rig.empty()) {
        paths.layout = paths.rig;
        paths.layout.replace_extension(".dverigui");
    }
    if (paths.skeleton.empty()) paths.skeleton = active_document().paths.skeleton;
    const auto saved = save_control_rig_pair_transactional(paths, skeleton(), session().document(), options);
    if (!saved.success) {
        if (error) *error = saved.error;
        set_status(saved.error);
        return false;
    }
    auto& state = active_document();
    state.paths = std::move(paths);
    state.savedRevision = state.session.document().revision;
    state.autosavedRevision = state.savedRevision;
    state.forceDirty = false;
    state.recovered = false;
    add_recent(state.paths.rig);
    const auto recovery = autosave_paths(state.paths);
    remove_file(recovery.rig);
    remove_file(recovery.layout);
    set_status("Control Rig pair saved");
    return true;
}

bool EditorControlRigPanel::autosave_active(std::string* error) {
    auto& state = active_document();
    if (!dirty(activeDocument_)) return true;
    if (state.paths.rig.empty() || state.paths.layout.empty()) {
        if (error) *error = "untitled control rig cannot be autosaved";
        return false;
    }
    const auto saved = save_control_rig_pair_transactional(
        autosave_paths(state.paths), state.skeleton, state.session.document());
    if (!saved.success) {
        if (error) *error = saved.error;
        set_status("Autosave failed: " + saved.error);
        return false;
    }
    state.autosavedRevision = state.session.document().revision;
    set_status("Control Rig autosaved");
    return true;
}

void EditorControlRigPanel::update(float elapsedSeconds) {
    if (!open_ || documents_.empty()) return;
    autosaveElapsed_ += std::max(0.0F, elapsedSeconds);
    if (autosaveElapsed_ < autosaveIntervalSeconds_) return;
    autosaveElapsed_ = 0.0F;
    if (dirty(activeDocument_) && active_document().session.document().revision != active_document().autosavedRevision)
        (void)autosave_active(nullptr);
}

void EditorControlRigPanel::open_demo() {
    open_document(make_control_rig_editor_demo_skeleton(), make_control_rig_editor_demo_document());
}

void EditorControlRigPanel::close() noexcept {
    open_ = false;
    capture_ = Capture::NoCapture;
    contextOpen_ = false;
    dragPin_.reset();
    cancel_inspector_edit();
}

void EditorControlRigPanel::toggle() {
    if (open_) close();
    else if (skeleton().bones.empty()) open_demo();
    else open_ = true;
}

void EditorControlRigPanel::resize(int width, int height) {
    width_ = std::max(640, width);
    height_ = std::max(480, height);
}

ControlRigGraphPoint EditorControlRigPanel::screen_to_graph(int x, int y) const noexcept {
    const ControlRigEditorFrame view = frame();
    return {(static_cast<float>(x - view.canvas.x) / zoom_) - pan_.x,
            (static_cast<float>(y - view.canvas.y) / zoom_) - pan_.y};
}

std::pair<int, int> EditorControlRigPanel::graph_to_screen(ControlRigGraphPoint point) const noexcept {
    const int margin = 28;
    const int titleHeight = 36;
    const int tabHeight = 28;
    const int toolbarHeight = 34;
    const int canvasX = margin + 154;
    const int canvasY = margin + titleHeight + tabHeight + toolbarHeight;
    return {canvasX + static_cast<int>((point.x + pan_.x) * zoom_),
            canvasY + static_cast<int>((point.y + pan_.y) * zoom_)};
}

ControlRigEditorFrame EditorControlRigPanel::frame() const {
    ControlRigEditorFrame result;
    const int margin = 28;
    result.panel = {margin, margin, std::max(1, width_ - margin * 2), std::max(1, height_ - margin * 2)};
    result.titleBar = {result.panel.x, result.panel.y, result.panel.width, 36};
    result.closeButton = {result.panel.x + result.panel.width - 31, result.panel.y + 6, 24, 24};
    result.tabBar = {result.panel.x, result.titleBar.y + result.titleBar.height, result.panel.width, 28};
    result.toolbar = {result.panel.x, result.tabBar.y + result.tabBar.height, result.panel.width, 34};
    result.tabs = tab_views();
    if (!result.tabs.empty()) {
        const int tabWidth = std::clamp(result.tabBar.width / static_cast<int>(result.tabs.size()), 110, 220);
        for (std::size_t index = 0; index < result.tabs.size(); ++index)
            result.tabs[index].rect = {result.tabBar.x + static_cast<int>(index) * tabWidth,
                                       result.tabBar.y, tabWidth, result.tabBar.height};
    }
    const int paletteWidth = 154;
    const int inspectorWidth = std::clamp(result.panel.width / 4, 230, 330);
    result.canvas = {result.panel.x + paletteWidth, result.toolbar.y + result.toolbar.height,
                     std::max(1, result.panel.width - paletteWidth - inspectorWidth),
                     std::max(1, result.panel.height - result.titleBar.height - result.toolbar.height)};
    result.inspector = {result.canvas.x + result.canvas.width, result.canvas.y,
                        inspectorWidth, result.canvas.height};
    const int previewHeight = std::clamp(result.inspector.height / 3, 150, 230);
    result.preview = {result.inspector.x + 8, result.inspector.y + 34,
                      std::max(1, result.inspector.width - 16), previewHeight};
    result.zoom = zoom_;
    result.status = status_;

    std::map<std::pair<int, std::uint64_t>, std::pair<ControlRigDiagnosticSeverity, std::size_t>> diagnosticSummary;
    for (const auto& diagnostic : session().document().diagnostics(skeleton())) {
        const auto key = std::make_pair(static_cast<int>(diagnostic.entityKind), diagnostic.entity);
        auto& summary = diagnosticSummary[key];
        if (summary.second == 0U || static_cast<int>(diagnostic.severity) > static_cast<int>(summary.first))
            summary.first = diagnostic.severity;
        ++summary.second;
    }

    auto make_card = [&](ControlRigGraphEntityKind kind, std::uint64_t id,
                         const ControlRigGraphNodeLayout& layout, std::string title,
                         std::string subtitle, const std::vector<ControlRigGraphPinSpec>& pins) {
        ControlRigGraphPoint position = layout.position;
        if (capture_ == Capture::MoveCard && kind == dragEntityKind_ && id == dragEntity_) {
            position.x += dragDelta_.x;
            position.y += dragDelta_.y;
        }
        const auto screen = graph_to_screen(position);
        ControlRigEditorCardView card;
        card.kind = kind;
        card.id = id;
        card.rect = {screen.first, screen.second,
                     std::max(120, static_cast<int>(layout.size.width * zoom_)),
                     std::max(54, static_cast<int>(layout.size.height * zoom_))};
        card.title = std::move(title);
        card.subtitle = std::move(subtitle);
        card.selected = kind == ControlRigGraphEntityKind::Control
            ? session().document().selectedControls.contains(id)
            : session().document().selectedNodes.contains(id);
        card.hovered = hovered_.kind == Hit::Kind::Card && hovered_.entityKind == kind && hovered_.id == id;
        if (const auto found = diagnosticSummary.find({static_cast<int>(kind), id}); found != diagnosticSummary.end()) {
            card.diagnostic = found->second.first;
            card.diagnosticCount = found->second.second;
        }
        std::vector<ControlRigGraphPinSpec> inputs;
        std::vector<ControlRigGraphPinSpec> outputs;
        for (const auto& pin : pins)
            (pin.direction == ControlRigGraphPinDirection::Input ? inputs : outputs).push_back(pin);
        auto append_pin = [&](const ControlRigGraphPinSpec& pin, std::size_t index, std::size_t count) {
            const int spacing = std::max(15, (card.rect.height - 30) / static_cast<int>(std::max<std::size_t>(1U, count)));
            const int centerY = card.rect.y + 28 + static_cast<int>(index) * spacing;
            const int centerX = pin.direction == ControlRigGraphPinDirection::Input
                ? card.rect.x : card.rect.x + card.rect.width;
            ControlRigEditorPinView view;
            view.endpoint = {kind, id, pin.name};
            view.direction = pin.direction;
            view.type = pin.type;
            view.rect = {centerX - 5, centerY - 5, 11, 11};
            view.label = pin.name;
            card.pins.push_back(std::move(view));
        };
        for (std::size_t index = 0; index < inputs.size(); ++index) append_pin(inputs[index], index, inputs.size());
        for (std::size_t index = 0; index < outputs.size(); ++index) append_pin(outputs[index], index, outputs.size());
        result.cards.push_back(std::move(card));
    };

    for (const auto& control : session().document().rig.controls) {
        const auto layout = session().document().controlLayouts.find(control.id);
        if (layout == session().document().controlLayouts.end()) continue;
        make_card(ControlRigGraphEntityKind::Control, control.id, layout->second, control.name,
                  control_kind_name(control.kind), control_rig_graph_pins(control));
    }
    for (const auto& node : session().document().rig.nodes) {
        const auto layout = session().document().nodeLayouts.find(node.id);
        if (layout == session().document().nodeLayouts.end()) continue;
        make_card(ControlRigGraphEntityKind::Node, node.id, layout->second, node.name,
                  phase_name(node.phase) + " / " + node_kind_name(node.kind), control_rig_graph_pins(node));
    }

    auto find_pin = [&](const ControlRigGraphEndpoint& endpoint) -> const ControlRigEditorPinView* {
        for (const auto& card : result.cards)
            for (const auto& pin : card.pins)
                if (pin.endpoint.kind == endpoint.kind && pin.endpoint.entity == endpoint.entity &&
                    pin.endpoint.pin == endpoint.pin) return &pin;
        return nullptr;
    };
    for (const auto& link : session().document().links) {
        const auto* from = find_pin(link.from);
        const auto* to = find_pin(link.to);
        if (!from || !to) continue;
        result.links.push_back({link.id, from->type,
            from->rect.x + from->rect.width / 2, from->rect.y + from->rect.height / 2,
            to->rect.x + to->rect.width / 2, to->rect.y + to->rect.height / 2,
            hovered_.kind == Hit::Kind::Link && hovered_.id == link.id});
    }
    for (const auto& comment : session().document().comments) {
        const auto screen = graph_to_screen(comment.position);
        result.comments.push_back({comment.id,
            {screen.first, screen.second, static_cast<int>(comment.size.width * zoom_),
             static_cast<int>(comment.size.height * zoom_)}, comment.text, comment.color});
    }
    if (dragPin_) {
        result.draggedPin = dragPin_;
        result.draggedPinX = pointerX_;
        result.draggedPinY = pointerY_;
    }
    if (capture_ == Capture::Marquee) {
        result.marquee = UiRect{std::min(pointerDownX_, pointerX_), std::min(pointerDownY_, pointerY_),
            std::abs(pointerX_ - pointerDownX_), std::abs(pointerY_ - pointerDownY_)};
    }

    ControlRigAsset compiled;
    std::string ignored;
    LocalPose pose;
    std::map<ControlRigControlId, RigidTransform> models;
    if (session().document().compile(skeleton(), compiled, &ignored) &&
        evaluate_control_rig(skeleton(), compiled, make_bind_pose(skeleton()), {}, pose, &models, &ignored)) {
        if (capture_ == Capture::Gizmo && previewSelection_) {
            const Float3 delta = subtract(gizmoPreview_.position, gizmoStart_.position);
            auto selected = models.find(*previewSelection_);
            if (selected != models.end()) selected->second.position = add(selected->second.position, delta);
        }
        const auto lines = build_control_rig_viewport_lines(session().document(), models);
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        for (const auto& line : lines) {
            minX = std::min({minX, line.from.x, line.to.x}); maxX = std::max({maxX, line.from.x, line.to.x});
            minY = std::min({minY, line.from.y, line.to.y}); maxY = std::max({maxY, line.from.y, line.to.y});
        }
        const float rangeX = std::max(0.25F, maxX - minX);
        const float rangeY = std::max(0.25F, maxY - minY);
        const float scale = 0.82F * std::min(static_cast<float>(result.preview.width) / rangeX,
                                             static_cast<float>(result.preview.height) / rangeY);
        const float centerX = (minX + maxX) * 0.5F;
        const float centerY = (minY + maxY) * 0.5F;
        auto project = [&](Float3 value) {
            return std::pair<int, int>{result.preview.x + result.preview.width / 2 + static_cast<int>((value.x - centerX) * scale),
                                       result.preview.y + result.preview.height / 2 - static_cast<int>((value.y - centerY) * scale)};
        };
        for (const auto& line : lines) {
            const auto a = project(line.from);
            const auto b = project(line.to);
            result.previewLines.push_back({a.first, a.second, b.first, b.second, line.color, line.control,
                                           previewSelection_ && *previewSelection_ == line.control});
        }
        if (previewSelection_) {
            if (const auto selected = models.find(*previewSelection_); selected != models.end()) {
                RigidTransform model = selected->second;
                if (capture_ == Capture::Gizmo)
                    model.position = add(model.position, subtract(gizmoPreview_.position, gizmoStart_.position));
                const auto origin = project(model.position);
                result.gizmoOriginX = origin.first;
                result.gizmoOriginY = origin.second;
                result.gizmoAxis = gizmoAxis_;
                result.gizmoCaptured = capture_ == Capture::Gizmo;
            }
        }
    }
    result.previewSelection = previewSelection_;

    const int propertyTop = result.preview.y + result.preview.height + 28;
    const int propertyBottom = result.inspector.y + result.inspector.height - 34;
    const int propertyHeight = 24;
    int propertyY = propertyTop;
    auto append_property = [&](std::string id, std::string label, std::string value,
                               bool editable, bool mixed = false) {
        if (propertyY + propertyHeight > propertyBottom) return;
        ControlRigInspectorPropertyView property;
        property.id = std::move(id);
        property.label = std::move(label);
        property.value = editingProperty_ == property.id ? inspectorTextBuffer_ : std::move(value);
        property.row = {result.inspector.x + 8, propertyY, result.inspector.width - 16, propertyHeight};
        property.decrementButton = {property.row.x + property.row.width - 48, propertyY + 2, 22, 20};
        property.incrementButton = {property.row.x + property.row.width - 24, propertyY + 2, 22, 20};
        property.editableText = editable;
        property.mixed = mixed;
        property.activeEdit = editingProperty_ == property.id;
        result.inspectorProperties.push_back(std::move(property));
        propertyY += propertyHeight;
    };
    const auto& document = session().document();
    if (!document.selectedControls.empty() && document.selectedNodes.empty()) {
        std::vector<const ControlRigControl*> controls;
        for (ControlRigControlId id : document.selectedControls) {
            const auto found = std::find_if(document.rig.controls.begin(), document.rig.controls.end(),
                [id](const auto& control) { return control.id == id; });
            if (found != document.rig.controls.end()) controls.push_back(&*found);
        }
        auto common = [&](const auto& getter) {
            std::vector<std::string> values;
            for (const auto* control : controls) values.push_back(getter(*control));
            const bool mixed = !values.empty() && std::any_of(values.begin() + 1, values.end(),
                [&](const auto& value) { return value != values.front(); });
            return std::pair<std::string, bool>{mixed ? "Multiple" : (values.empty() ? "" : values.front()), mixed};
        };
        if (controls.size() == 1U) append_property("name", "Name", controls.front()->name, true);
        auto [kind, kindMixed] = common([](const auto& value) { return control_kind_name(value.kind); });
        append_property("kind", "Kind", kind, false, kindMixed);
        auto [space, spaceMixed] = common([](const auto& value) { return control_space_name(value.space); });
        append_property("space", "Space", space, false, spaceMixed);
        if (controls.size() == 1U && controls.front()->space == ControlRigSpace::Bone) {
            const BoneIndex bone = controls.front()->spaceBone;
            append_property("space_bone", "Space Bone",
                bone < skeleton().bones.size() ? skeleton().bones[bone].name : "None", true);
        }
        if (controls.size() == 1U && controls.front()->space == ControlRigSpace::Control) {
            const auto parent = std::find_if(document.rig.controls.begin(), document.rig.controls.end(),
                [&](const auto& value) { return value.id == controls.front()->parentControl; });
            append_property("parent_control", "Parent Control",
                parent == document.rig.controls.end() ? "None" : parent->name, true);
        }
        auto visual_value = [&](ControlRigControlId id) {
            const auto found = document.controlVisuals.find(id);
            return found == document.controlVisuals.end() ? ControlRigControlVisual{} : found->second;
        };
        auto [shape, shapeMixed] = common([&](const auto& value) { return control_shape_name(visual_value(value.id).shape); });
        append_property("shape", "Shape", shape, false, shapeMixed);
        auto [size, sizeMixed] = common([&](const auto& value) { return compact_float(visual_value(value.id).sizeMeters); });
        append_property("shape_size", "Shape Size", size + (sizeMixed ? "" : " m"), false, sizeMixed);
        auto [visible, visibleMixed] = common([&](const auto& value) { return visual_value(value.id).visible ? "Yes" : "No"; });
        append_property("visible", "Visible", visible, false, visibleMixed);
        auto [translationLimits, translationMixed] = common([](const auto& value) { return value.limits.translationEnabled ? "On" : "Off"; });
        append_property("translation_limits", "Translation Limits", translationLimits, false, translationMixed);
        auto [rotationLimits, rotationMixed] = common([](const auto& value) { return value.limits.rotationEnabled ? "On" : "Off"; });
        append_property("rotation_limits", "Rotation Limits", rotationLimits, false, rotationMixed);
        if (controls.size() == 1U) {
            append_property("default_x", "Default Position X", compact_float(controls.front()->defaultLocal.position.x), false);
            append_property("default_y", "Default Position Y", compact_float(controls.front()->defaultLocal.position.y), false);
            append_property("default_z", "Default Position Z", compact_float(controls.front()->defaultLocal.position.z), false);
        }
    } else if (document.selectedControls.empty() && !document.selectedNodes.empty()) {
        std::vector<const ControlRigNode*> nodes;
        for (ControlRigNodeId id : document.selectedNodes) {
            const auto found = std::find_if(document.rig.nodes.begin(), document.rig.nodes.end(),
                [id](const auto& node) { return node.id == id; });
            if (found != document.rig.nodes.end()) nodes.push_back(&*found);
        }
        auto common = [&](const auto& getter) {
            std::vector<std::string> values;
            for (const auto* node : nodes) values.push_back(getter(*node));
            const bool mixed = !values.empty() && std::any_of(values.begin() + 1, values.end(),
                [&](const auto& value) { return value != values.front(); });
            return std::pair<std::string, bool>{mixed ? "Multiple" : (values.empty() ? "" : values.front()), mixed};
        };
        if (nodes.size() == 1U) append_property("name", "Name", nodes.front()->name, true);
        auto [kind, kindMixed] = common([](const auto& value) { return node_kind_name(value.kind); });
        append_property("kind", "Kind", kind, false, kindMixed);
        auto [phase, phaseMixed] = common([](const auto& value) { return phase_name(value.phase); });
        append_property("phase", "Phase", phase, false, phaseMixed);
        auto [enabled, enabledMixed] = common([](const auto& value) { return value.enabled ? "Yes" : "No"; });
        append_property("enabled", "Enabled", enabled, false, enabledMixed);
        auto [weight, weightMixed] = common([](const auto& value) { return compact_float(value.weight); });
        append_property("weight", "Weight", weight, false, weightMixed);
        if (nodes.size() == 1U) {
            auto bone_name = [&](BoneIndex bone) { return bone < skeleton().bones.size() ? skeleton().bones[bone].name : "None"; };
            append_property("bone", "Bone", bone_name(nodes.front()->bone), true);
            if (nodes.front()->kind == ControlRigNodeKind::CopyBoneTransform)
                append_property("source_bone", "Source Bone", bone_name(nodes.front()->sourceBone), true);
            if (nodes.front()->kind == ControlRigNodeKind::TwoBoneIk) {
                append_property("middle_bone", "Middle Bone", bone_name(nodes.front()->middleBone), true);
                append_property("end_bone", "End Bone", bone_name(nodes.front()->endBone), true);
            }
            auto control_name = [&](ControlRigControlId id) {
                const auto found = std::find_if(document.rig.controls.begin(), document.rig.controls.end(),
                    [id](const auto& control) { return control.id == id; });
                return found == document.rig.controls.end() ? std::string("None") : found->name;
            };
            if (nodes.front()->kind == ControlRigNodeKind::AimConstraint ||
                nodes.front()->kind == ControlRigNodeKind::TwoBoneIk || nodes.front()->kind == ControlRigNodeKind::Fabrik)
                append_property("target_control", "Target Control", control_name(nodes.front()->targetControl), true);
            if (nodes.front()->kind == ControlRigNodeKind::TwoBoneIk)
                append_property("pole_control", "Pole Control", control_name(nodes.front()->poleControl), true);
        }
        auto [affectTranslation, affectTranslationMixed] = common([](const auto& value) { return value.affectTranslation ? "Yes" : "No"; });
        append_property("affect_translation", "Affect Translation", affectTranslation, false, affectTranslationMixed);
        auto [affectRotation, affectRotationMixed] = common([](const auto& value) { return value.affectRotation ? "Yes" : "No"; });
        append_property("affect_rotation", "Affect Rotation", affectRotation, false, affectRotationMixed);
        auto [stretch, stretchMixed] = common([](const auto& value) { return value.allowStretch ? "Yes" : "No"; });
        append_property("allow_stretch", "Allow Stretch", stretch, false, stretchMixed);
        auto [iterations, iterationsMixed] = common([](const auto& value) { return std::to_string(value.maximumIterations); });
        append_property("iterations", "Max Iterations", iterations, false, iterationsMixed);
        auto [tolerance, toleranceMixed] = common([](const auto& value) { return compact_float(value.toleranceMeters); });
        append_property("tolerance", "Tolerance", tolerance + (toleranceMixed ? "" : " m"), false, toleranceMixed);
    }

    if (contextOpen_) {
        static constexpr std::pair<std::string_view, std::string_view> actions[] = {
            {"add.transform_control", "Add Transform Control"},
            {"add.set_bone", "Add Set Bone Node"},
            {"add.two_bone_ik", "Add Two Bone IK"},
            {"add.comment", "Add Comment"},
            {"delete.selection", "Delete Selection"},
        };
        const int rowHeight = 25;
        for (std::size_t index = 0; index < std::size(actions); ++index) {
            UiRect rect{contextX_, contextY_ + static_cast<int>(index) * rowHeight, 190, rowHeight};
            result.contextActions.push_back({std::string(actions[index].first), std::string(actions[index].second), rect,
                                             rect.contains(pointerX_, pointerY_)});
        }
    }
    return result;
}

EditorControlRigPanel::Hit EditorControlRigPanel::hit_test(int x, int y) const {
    const ControlRigEditorFrame view = frame();
    if (previewSelection_ && view.preview.contains(x, y)) {
        const int ox = view.gizmoOriginX;
        const int oy = view.gizmoOriginY;
        if (point_segment_distance(static_cast<float>(x), static_cast<float>(y), static_cast<float>(ox),
                                   static_cast<float>(oy), static_cast<float>(ox + 42), static_cast<float>(oy)) <= 7.0F)
            return {Hit::Kind::GizmoAxis, ControlRigGraphEntityKind::Control, *previewSelection_, {}, 1};
        if (point_segment_distance(static_cast<float>(x), static_cast<float>(y), static_cast<float>(ox),
                                   static_cast<float>(oy), static_cast<float>(ox), static_cast<float>(oy - 42)) <= 7.0F)
            return {Hit::Kind::GizmoAxis, ControlRigGraphEntityKind::Control, *previewSelection_, {}, 2};
        if (point_segment_distance(static_cast<float>(x), static_cast<float>(y), static_cast<float>(ox),
                                   static_cast<float>(oy), static_cast<float>(ox - 30), static_cast<float>(oy + 30)) <= 7.0F)
            return {Hit::Kind::GizmoAxis, ControlRigGraphEntityKind::Control, *previewSelection_, {}, 3};
    }
    if (view.preview.contains(x, y)) {
        float best = 10.0F;
        ControlRigControlId bestControl{};
        for (const auto& line : view.previewLines) {
            const float distance = point_segment_distance(static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(line.fromX), static_cast<float>(line.fromY),
                static_cast<float>(line.toX), static_cast<float>(line.toY));
            if (distance < best && line.control != kInvalidControlRigControlId) {
                best = distance;
                bestControl = line.control;
            }
        }
        if (bestControl != kInvalidControlRigControlId)
            return {Hit::Kind::PreviewControl, ControlRigGraphEntityKind::Control, bestControl, {}, 0};
    }
    for (auto card = view.cards.rbegin(); card != view.cards.rend(); ++card) {
        for (const auto& pin : card->pins)
            if (pin.rect.contains(x, y)) return {Hit::Kind::Pin, card->kind, card->id, pin};
        if (card->rect.contains(x, y)) return {Hit::Kind::Card, card->kind, card->id, {}, 0};
    }
    for (const auto& link : view.links) {
        const int midX = (link.fromX + link.toX) / 2;
        const float distance = std::min({
            point_segment_distance(static_cast<float>(x), static_cast<float>(y), static_cast<float>(link.fromX), static_cast<float>(link.fromY), static_cast<float>(midX), static_cast<float>(link.fromY)),
            point_segment_distance(static_cast<float>(x), static_cast<float>(y), static_cast<float>(midX), static_cast<float>(link.fromY), static_cast<float>(midX), static_cast<float>(link.toY)),
            point_segment_distance(static_cast<float>(x), static_cast<float>(y), static_cast<float>(midX), static_cast<float>(link.toY), static_cast<float>(link.toX), static_cast<float>(link.toY))});
        if (distance <= 6.0F) return {Hit::Kind::Link, ControlRigGraphEntityKind::Node, link.id, {}, 0};
    }
    for (auto comment = view.comments.rbegin(); comment != view.comments.rend(); ++comment)
        if (comment->rect.contains(x, y)) return {Hit::Kind::Comment, ControlRigGraphEntityKind::Node, comment->id, {}, 0};
    return {};
}

void EditorControlRigPanel::clear_selection() noexcept {
    cancel_inspector_edit();
    session().document().selectedControls.clear();
    session().document().selectedNodes.clear();
    selectedComment_.reset();
}

void EditorControlRigPanel::select_entity(ControlRigGraphEntityKind kind, std::uint64_t id, bool additive) {
    if (!additive) clear_selection();
    if (kind == ControlRigGraphEntityKind::Control) {
        if (additive && session().document().selectedControls.contains(id)) session().document().selectedControls.erase(id);
        else session().document().selectedControls.insert(id);
    } else {
        if (additive && session().document().selectedNodes.contains(id)) session().document().selectedNodes.erase(id);
        else session().document().selectedNodes.insert(id);
    }
}

bool EditorControlRigPanel::pointer_move(int x, int y, std::uint32_t) {
    if (!open_) return false;
    pointerX_ = x;
    pointerY_ = y;
    if (capture_ == Capture::Pan) {
        pan_.x += static_cast<float>(x - pointerDownX_) / zoom_;
        pan_.y += static_cast<float>(y - pointerDownY_) / zoom_;
        pointerDownX_ = x;
        pointerDownY_ = y;
    } else if (capture_ == Capture::MoveCard) {
        const ControlRigGraphPoint current = screen_to_graph(x, y);
        dragDelta_ = {current.x - dragPointerStart_.x, current.y - dragPointerStart_.y};
    } else if (capture_ == Capture::Gizmo) {
        const float scale = 0.01F;
        const float dx = static_cast<float>(x - pointerDownX_) * scale;
        const float dy = static_cast<float>(y - pointerDownY_) * scale;
        gizmoPreview_ = gizmoStart_;
        if (gizmoAxis_ == 1) gizmoPreview_.position.x += dx;
        else if (gizmoAxis_ == 2) gizmoPreview_.position.y -= dy;
        else if (gizmoAxis_ == 3) gizmoPreview_.position.z += (dx - dy) * 0.5F;
    }
    hovered_ = hit_test(x, y);
    return true;
}

bool EditorControlRigPanel::pointer_down(int button, int x, int y, std::uint32_t modifiers) {
    if (!open_) return false;
    pointerX_ = pointerDownX_ = x;
    pointerY_ = pointerDownY_ = y;
    const ControlRigEditorFrame view = frame();
    if (button == 1 && view.closeButton.contains(x, y)) { close(); return true; }
    if (!view.panel.contains(x, y)) return true;
    if (button == 1 && view.tabBar.contains(x, y)) {
        for (const auto& tab : view.tabs)
            if (tab.rect.contains(x, y)) return switch_tab(tab.index);
        return true;
    }
    if (button == 1) {
        for (const auto& property : view.inspectorProperties) {
            if (property.decrementButton.contains(x, y) || property.incrementButton.contains(x, y)) {
                cancel_inspector_edit();
                std::string error;
                const int direction = property.decrementButton.contains(x, y) ? -1 : 1;
                const bool changed = adjust_inspector_property(property.id, direction, &error);
                set_status(changed ? property.label + " updated" : error);
                return true;
            }
            if (property.row.contains(x, y)) {
                if (property.editableText) return begin_inspector_edit(property);
                return true;
            }
        }
    }
    if (contextOpen_) {
        if (button == 1) {
            for (const auto& action : view.contextActions) {
                if (action.rect.contains(x, y)) {
                    contextOpen_ = false;
                    return activate_context_action(action.id);
                }
            }
        }
        contextOpen_ = false;
        return true;
    }
    if (button == 3 && view.canvas.contains(x, y)) {
        contextOpen_ = true;
        contextX_ = std::min(x, view.panel.x + view.panel.width - 194);
        contextY_ = std::min(y, view.panel.y + view.panel.height - 129);
        contextGraphPosition_ = screen_to_graph(x, y);
        return true;
    }
    if (button == 2 || (button == 1 && (modifiers & 4U) != 0U)) {
        capture_ = Capture::Pan;
        return true;
    }
    if (button != 1) return true;
    hovered_ = hit_test(x, y);
    if (hovered_.kind == Hit::Kind::GizmoAxis && previewSelection_) {
        const auto control = std::find_if(session().document().rig.controls.begin(), session().document().rig.controls.end(),
            [&](const auto& item) { return item.id == *previewSelection_; });
        if (control != session().document().rig.controls.end()) {
            gizmoAxis_ = hovered_.axis;
            gizmoStart_ = control->defaultLocal;
            gizmoPreview_ = gizmoStart_;
            capture_ = Capture::Gizmo;
            set_status("Transform gizmo captured");
        }
        return true;
    }
    if (hovered_.kind == Hit::Kind::PreviewControl) {
        previewSelection_ = static_cast<ControlRigControlId>(hovered_.id);
        select_entity(ControlRigGraphEntityKind::Control, hovered_.id, (modifiers & 1U) != 0U);
        set_status("Viewport control selected");
        return true;
    }
    if (hovered_.kind == Hit::Kind::Pin && hovered_.pin) {
        dragPin_ = hovered_.pin;
        capture_ = Capture::Link;
        set_status("Drag to a compatible pin");
        return true;
    }
    if (hovered_.kind == Hit::Kind::Link) {
        if ((modifiers & 1U) != 0U && session().disconnect(hovered_.id)) set_status("Link disconnected");
        else set_status("Shift-click a link to disconnect it");
        return true;
    }
    if (hovered_.kind == Hit::Kind::Card) {
        select_entity(hovered_.entityKind, hovered_.id, (modifiers & 1U) != 0U);
        dragEntityKind_ = hovered_.entityKind;
        dragEntity_ = hovered_.id;
        const auto& layouts = hovered_.entityKind == ControlRigGraphEntityKind::Control
            ? session().document().controlLayouts : session().document().nodeLayouts;
        const auto found = layouts.find(hovered_.id);
        if (found != layouts.end()) dragEntityStart_ = found->second.position;
        dragPointerStart_ = screen_to_graph(x, y);
        dragDelta_ = {};
        capture_ = Capture::MoveCard;
        return true;
    }
    if (view.canvas.contains(x, y)) {
        if ((modifiers & 1U) == 0U) clear_selection();
        capture_ = Capture::Marquee;
        return true;
    }
    return true;
}

bool EditorControlRigPanel::pointer_up(int button, int x, int y, std::uint32_t modifiers) {
    if (!open_) return false;
    pointerX_ = x;
    pointerY_ = y;
    if (button != 1 && button != 2) return true;
    if (capture_ == Capture::MoveCard && (std::abs(dragDelta_.x) > 0.01F || std::abs(dragDelta_.y) > 0.01F)) {
        const ControlRigGraphPoint target{dragEntityStart_.x + dragDelta_.x, dragEntityStart_.y + dragDelta_.y};
        const bool moved = dragEntityKind_ == ControlRigGraphEntityKind::Control
            ? session().move_control(dragEntity_, target) : session().move_node(dragEntity_, target);
        set_status(moved ? "Graph item moved" : "Could not move graph item");
    } else if (capture_ == Capture::Link && dragPin_) {
        const Hit target = hit_test(x, y);
        if (target.kind == Hit::Kind::Pin && target.pin) {
            ControlRigGraphEndpoint from = dragPin_->endpoint;
            ControlRigGraphEndpoint to = target.pin->endpoint;
            if (dragPin_->direction == ControlRigGraphPinDirection::Input) std::swap(from, to);
            std::string error;
            const auto link = session().connect(std::move(from), std::move(to), &error);
            set_status(link != 0U ? "Pins connected" : error);
        } else set_status("Connection cancelled");
    } else if (capture_ == Capture::Marquee) {
        const UiRect selection{std::min(pointerDownX_, x), std::min(pointerDownY_, y),
                               std::abs(x - pointerDownX_), std::abs(y - pointerDownY_)};
        if ((modifiers & 1U) == 0U) clear_selection();
        for (const auto& card : frame().cards) {
            if (!overlaps(selection, card.rect)) continue;
            if (card.kind == ControlRigGraphEntityKind::Control) session().document().selectedControls.insert(card.id);
            else session().document().selectedNodes.insert(card.id);
        }
        set_status("Marquee selection updated");
    } else if (capture_ == Capture::Gizmo && previewSelection_) {
        std::string error;
        const bool committed = session().set_control_default(*previewSelection_, gizmoPreview_, &error);
        set_status(committed ? "Control transform committed" : error);
    }
    capture_ = Capture::NoCapture;
    dragPin_.reset();
    dragDelta_ = {};
    hovered_ = hit_test(x, y);
    return true;
}

bool EditorControlRigPanel::pointer_wheel(float steps, int x, int y, std::uint32_t) {
    if (!open_) return false;
    const ControlRigEditorFrame view = frame();
    if (!view.canvas.contains(x, y)) return true;
    const ControlRigGraphPoint before = screen_to_graph(x, y);
    zoom_ = std::clamp(zoom_ * (steps > 0.0F ? 1.12F : 0.8928571F), 0.35F, 2.5F);
    pan_.x = static_cast<float>(x - view.canvas.x) / zoom_ - before.x;
    pan_.y = static_cast<float>(y - view.canvas.y) / zoom_ - before.y;
    set_status("Graph zoom " + std::to_string(static_cast<int>(zoom_ * 100.0F)) + "%");
    return true;
}

void EditorControlRigPanel::delete_selection() {
    std::vector<std::uint64_t> links;
    for (const auto& link : session().document().links) {
        const bool fromSelected = link.from.kind == ControlRigGraphEntityKind::Control
            ? session().document().selectedControls.contains(link.from.entity)
            : session().document().selectedNodes.contains(link.from.entity);
        const bool toSelected = link.to.kind == ControlRigGraphEntityKind::Control
            ? session().document().selectedControls.contains(link.to.entity)
            : session().document().selectedNodes.contains(link.to.entity);
        if (fromSelected || toSelected) links.push_back(link.id);
    }
    for (const auto link : links) (void)session().disconnect(link);
    const auto controls = session().document().selectedControls;
    const auto nodes = session().document().selectedNodes;
    for (const auto node : nodes) (void)session().remove_node(node);
    for (const auto control : controls) (void)session().remove_control(control);
    if (selectedComment_) (void)session().remove_comment(*selectedComment_);
    clear_selection();
    previewSelection_.reset();
    set_status("Selection deleted");
}

void EditorControlRigPanel::duplicate_selection() {
    const auto controls = session().document().selectedControls;
    const auto nodes = session().document().selectedNodes;
    clear_selection();
    std::string error;
    for (const auto id : controls) {
        const auto found = std::find_if(session().document().rig.controls.begin(), session().document().rig.controls.end(),
            [&](const auto& item) { return item.id == id; });
        if (found == session().document().rig.controls.end()) continue;
        ControlRigControl copy = *found;
        copy.id = kInvalidControlRigControlId;
        copy.name += " Copy";
        const auto position = session().document().controlLayouts.at(id).position;
        const auto created = session().add_control(std::move(copy), {position.x + 36.0F, position.y + 36.0F}, &error);
        if (created != 0U) session().document().selectedControls.insert(created);
    }
    for (const auto id : nodes) {
        const auto found = std::find_if(session().document().rig.nodes.begin(), session().document().rig.nodes.end(),
            [&](const auto& item) { return item.id == id; });
        if (found == session().document().rig.nodes.end()) continue;
        ControlRigNode copy = *found;
        copy.id = kInvalidControlRigNodeId;
        copy.name += " Copy";
        const auto position = session().document().nodeLayouts.at(id).position;
        const auto created = session().add_node(std::move(copy), {position.x + 36.0F, position.y + 36.0F}, &error);
        if (created != 0U) session().document().selectedNodes.insert(created);
    }
    set_status(error.empty() ? "Selection duplicated" : error);
}

void EditorControlRigPanel::frame_selection() {
    std::vector<ControlRigGraphPoint> points;
    for (const auto id : session().document().selectedControls)
        if (const auto found = session().document().controlLayouts.find(id); found != session().document().controlLayouts.end())
            points.push_back(found->second.position);
    for (const auto id : session().document().selectedNodes)
        if (const auto found = session().document().nodeLayouts.find(id); found != session().document().nodeLayouts.end())
            points.push_back(found->second.position);
    if (points.empty()) {
        for (const auto& [id, layout] : session().document().controlLayouts) { (void)id; points.push_back(layout.position); }
        for (const auto& [id, layout] : session().document().nodeLayouts) { (void)id; points.push_back(layout.position); }
    }
    if (points.empty()) return;
    Float3 center{};
    for (const auto point : points) { center.x += point.x; center.y += point.y; }
    center.x /= static_cast<float>(points.size());
    center.y /= static_cast<float>(points.size());
    const auto view = frame();
    pan_.x = static_cast<float>(view.canvas.width) * 0.5F / zoom_ - center.x - 100.0F;
    pan_.y = static_cast<float>(view.canvas.height) * 0.5F / zoom_ - center.y - 60.0F;
    set_status("Framed graph selection");
}

bool EditorControlRigPanel::key_down(std::string_view key, bool control, bool shift, bool alt) {
    if (!open_) return false;
    if (!editingProperty_.empty()) {
        if (key == "escape") { cancel_inspector_edit(); set_status("Inspector edit cancelled"); return true; }
        if (key == "enter" || key == "return") return commit_inspector_edit();
        if (key == "backspace") {
            if (replaceInspectorText_) inspectorTextBuffer_.clear(), replaceInspectorText_ = false;
            else if (!inspectorTextBuffer_.empty()) inspectorTextBuffer_.pop_back();
            return true;
        }
        return true;
    }
    if (key == "escape") {
        if (capture_ == Capture::Gizmo) set_status("Control transform cancelled");
        capture_ = Capture::NoCapture;
        dragPin_.reset();
        contextOpen_ = false;
        return true;
    }
    if (control && key == "z") {
        const bool changed = shift ? session().redo() : session().undo();
        set_status(changed ? (shift ? "Redo" : "Undo") : "Nothing to undo or redo");
        return true;
    }
    if (control && key == "y") { set_status(session().redo() ? "Redo" : "Nothing to redo"); return true; }
    if (control && key == "a") {
        clear_selection();
        for (const auto& item : session().document().rig.controls) session().document().selectedControls.insert(item.id);
        for (const auto& node : session().document().rig.nodes) session().document().selectedNodes.insert(node.id);
        set_status("Selected all graph items");
        return true;
    }
    if (control && key == "d") { duplicate_selection(); return true; }
    if (control && key == "s") {
        std::string error;
        set_status(save_active(&error) ? "Control Rig pair saved" : error);
        return true;
    }
    if (control && key == "w") {
        std::string error;
        if (!close_tab(activeDocument_, false, &error)) set_status(error);
        return true;
    }
    if (key == "delete" || key == "backspace") { delete_selection(); return true; }
    if (!control && !alt && key == "f") { frame_selection(); return true; }
    return true;
}

bool EditorControlRigPanel::text_input(std::string_view text) {
    if (!open_ || editingProperty_.empty()) return false;
    if (replaceInspectorText_) inspectorTextBuffer_.clear(), replaceInspectorText_ = false;
    for (char rawCharacter : text) {
        const unsigned char character = static_cast<unsigned char>(rawCharacter);
        if (character >= 32U && character != 127U && inspectorTextBuffer_.size() < 255U)
            inspectorTextBuffer_.push_back(static_cast<char>(character));
    }
    return true;
}

bool EditorControlRigPanel::begin_inspector_edit(const ControlRigInspectorPropertyView& property) {
    editingProperty_ = property.id;
    inspectorTextBuffer_ = property.mixed ? std::string{} : property.value;
    replaceInspectorText_ = true;
    set_status(property.id == "name" ? "Type a name and press Enter" :
        "Type a bone/control name or None and press Enter");
    return true;
}

void EditorControlRigPanel::cancel_inspector_edit() noexcept {
    editingProperty_.clear();
    inspectorTextBuffer_.clear();
    replaceInspectorText_ = false;
}

bool EditorControlRigPanel::adjust_inspector_property(
    std::string_view propertyId, int direction, std::string* error) {
    if (!open_ || documents_.empty()) {
        if (error) *error = "control rig inspector is not open";
        return false;
    }
    const auto& selectedControls = session().document().selectedControls;
    const auto& selectedNodes = session().document().selectedNodes;
    if (!selectedControls.empty() && selectedNodes.empty()) {
        if (propertyId == "shape" || propertyId == "shape_size" || propertyId == "visible") {
            std::map<ControlRigControlId, ControlRigControlVisual> changes;
            for (ControlRigControlId id : selectedControls) {
                const auto found = session().document().controlVisuals.find(id);
                ControlRigControlVisual visual = found == session().document().controlVisuals.end()
                    ? ControlRigControlVisual{} : found->second;
                if (propertyId == "shape")
                    visual.shape = static_cast<ControlRigControlShape>(cycle_index(
                        static_cast<int>(visual.shape), 5, direction));
                else if (propertyId == "shape_size")
                    visual.sizeMeters = std::clamp(visual.sizeMeters + (direction < 0 ? -0.01F : 0.01F), 0.01F, 1000.0F);
                else visual.visible = direction >= 0;
                changes.emplace(id, visual);
            }
            return session().set_control_visuals(changes, error);
        }
        std::vector<ControlRigControl> changes;
        for (ControlRigControlId id : selectedControls) {
            const auto found = std::find_if(session().document().rig.controls.begin(), session().document().rig.controls.end(),
                [id](const auto& control) { return control.id == id; });
            if (found == session().document().rig.controls.end()) continue;
            changes.push_back(*found);
        }
        for (auto& control : changes) {
            if (propertyId == "kind")
                control.kind = static_cast<ControlRigControlKind>(cycle_index(static_cast<int>(control.kind), 3, direction));
            else if (propertyId == "space") {
                control.space = static_cast<ControlRigSpace>(cycle_index(static_cast<int>(control.space), 3, direction));
                control.spaceBone = control.space == ControlRigSpace::Bone && !skeleton().bones.empty() ? 0U : kInvalidBoneIndex;
                control.parentControl = kInvalidControlRigControlId;
                if (control.space == ControlRigSpace::Control) {
                    const auto parent = std::find_if(session().document().rig.controls.begin(), session().document().rig.controls.end(),
                        [&](const auto& candidate) { return candidate.id != control.id; });
                    if (parent != session().document().rig.controls.end()) control.parentControl = parent->id;
                }
            } else if (propertyId == "space_bone") {
                const int count = static_cast<int>(skeleton().bones.size()) + 1;
                int current = control.spaceBone < skeleton().bones.size() ? static_cast<int>(control.spaceBone) + 1 : 0;
                current = cycle_index(current, count, direction);
                control.spaceBone = current == 0 ? kInvalidBoneIndex : static_cast<BoneIndex>(current - 1);
            } else if (propertyId == "parent_control") {
                std::vector<ControlRigControlId> candidates{kInvalidControlRigControlId};
                for (const auto& candidate : session().document().rig.controls)
                    if (candidate.id != control.id) candidates.push_back(candidate.id);
                const auto current = std::find(candidates.begin(), candidates.end(), control.parentControl);
                const int index = current == candidates.end() ? 0 : static_cast<int>(current - candidates.begin());
                control.parentControl = candidates[static_cast<std::size_t>(cycle_index(index, static_cast<int>(candidates.size()), direction))];
            } else if (propertyId == "translation_limits") control.limits.translationEnabled = direction >= 0;
            else if (propertyId == "rotation_limits") control.limits.rotationEnabled = direction >= 0;
            else if (propertyId == "default_x") control.defaultLocal.position.x += direction < 0 ? -0.05F : 0.05F;
            else if (propertyId == "default_y") control.defaultLocal.position.y += direction < 0 ? -0.05F : 0.05F;
            else if (propertyId == "default_z") control.defaultLocal.position.z += direction < 0 ? -0.05F : 0.05F;
            else {
                if (error) *error = "control inspector property cannot be adjusted";
                return false;
            }
        }
        return session().update_controls(skeleton(), changes, error);
    }
    if (selectedControls.empty() && !selectedNodes.empty()) {
        std::vector<ControlRigNode> changes;
        for (ControlRigNodeId id : selectedNodes) {
            const auto found = std::find_if(session().document().rig.nodes.begin(), session().document().rig.nodes.end(),
                [id](const auto& node) { return node.id == id; });
            if (found != session().document().rig.nodes.end()) changes.push_back(*found);
        }
        auto cycle_bone = [&](BoneIndex& bone) {
            const int count = static_cast<int>(skeleton().bones.size()) + 1;
            int current = bone < skeleton().bones.size() ? static_cast<int>(bone) + 1 : 0;
            current = cycle_index(current, count, direction);
            bone = current == 0 ? kInvalidBoneIndex : static_cast<BoneIndex>(current - 1);
        };
        auto cycle_control = [&](ControlRigControlId& id) {
            std::vector<ControlRigControlId> candidates{kInvalidControlRigControlId};
            for (const auto& control : session().document().rig.controls) candidates.push_back(control.id);
            const auto current = std::find(candidates.begin(), candidates.end(), id);
            const int index = current == candidates.end() ? 0 : static_cast<int>(current - candidates.begin());
            id = candidates[static_cast<std::size_t>(cycle_index(index, static_cast<int>(candidates.size()), direction))];
        };
        for (auto& node : changes) {
            if (propertyId == "kind")
                node.kind = static_cast<ControlRigNodeKind>(cycle_index(static_cast<int>(node.kind), 6, direction));
            else if (propertyId == "phase")
                node.phase = static_cast<ControlRigSolvePhase>(cycle_index(static_cast<int>(node.phase), 3, direction));
            else if (propertyId == "enabled") node.enabled = direction >= 0;
            else if (propertyId == "weight") node.weight = std::clamp(node.weight + (direction < 0 ? -0.05F : 0.05F), 0.0F, 1.0F);
            else if (propertyId == "bone") cycle_bone(node.bone);
            else if (propertyId == "source_bone") cycle_bone(node.sourceBone);
            else if (propertyId == "middle_bone") cycle_bone(node.middleBone);
            else if (propertyId == "end_bone") cycle_bone(node.endBone);
            else if (propertyId == "target_control") cycle_control(node.targetControl);
            else if (propertyId == "pole_control") cycle_control(node.poleControl);
            else if (propertyId == "affect_translation") node.affectTranslation = direction >= 0;
            else if (propertyId == "affect_rotation") node.affectRotation = direction >= 0;
            else if (propertyId == "allow_stretch") node.allowStretch = direction >= 0;
            else if (propertyId == "iterations") {
                if (direction < 0 && node.maximumIterations > 1U) --node.maximumIterations;
                else if (direction >= 0 && node.maximumIterations < 1024U) ++node.maximumIterations;
            } else if (propertyId == "tolerance")
                node.toleranceMeters = std::clamp(node.toleranceMeters * (direction < 0 ? 0.5F : 2.0F), 0.000001F, 1.0F);
            else {
                if (error) *error = "node inspector property cannot be adjusted";
                return false;
            }
        }
        return session().update_nodes(skeleton(), changes, error);
    }
    if (error) *error = "select only controls or only nodes to edit common properties";
    return false;
}

bool EditorControlRigPanel::commit_inspector_edit() {
    const std::string property = editingProperty_;
    const std::string value = inspectorTextBuffer_;
    std::string error;
    bool committed{};
    auto resolve_bone = [&](std::string_view query, BoneIndex& output) {
        const std::string wanted = lowercase(query);
        if (wanted.empty() || wanted == "none") { output = kInvalidBoneIndex; return true; }
        std::optional<BoneIndex> match;
        for (std::size_t index = 0; index < skeleton().bones.size(); ++index) {
            const std::string name = lowercase(skeleton().bones[index].name);
            if (name == wanted) { output = static_cast<BoneIndex>(index); return true; }
            if (name.find(wanted) != std::string::npos) {
                if (match) return false;
                match = static_cast<BoneIndex>(index);
            }
        }
        if (!match) return false;
        output = *match;
        return true;
    };
    auto resolve_control = [&](std::string_view query, ControlRigControlId& output) {
        const std::string wanted = lowercase(query);
        if (wanted.empty() || wanted == "none") { output = kInvalidControlRigControlId; return true; }
        std::optional<ControlRigControlId> match;
        for (const auto& control : session().document().rig.controls) {
            const std::string name = lowercase(control.name);
            if (name == wanted) { output = control.id; return true; }
            if (name.find(wanted) != std::string::npos) {
                if (match) return false;
                match = control.id;
            }
        }
        if (!match) return false;
        output = *match;
        return true;
    };
    if (!session().document().selectedControls.empty() && session().document().selectedNodes.empty()) {
        std::vector<ControlRigControl> changes;
        for (ControlRigControlId id : session().document().selectedControls) {
            const auto found = std::find_if(session().document().rig.controls.begin(), session().document().rig.controls.end(),
                [id](const auto& control) { return control.id == id; });
            if (found != session().document().rig.controls.end()) changes.push_back(*found);
        }
        if (changes.size() != 1U) error = "text properties require one selected control";
        else if (property == "name") changes.front().name = value;
        else if (property == "space_bone" && !resolve_bone(value, changes.front().spaceBone)) error = "bone search is ambiguous or has no match";
        else if (property == "parent_control" && !resolve_control(value, changes.front().parentControl)) error = "control search is ambiguous or has no match";
        else if (property != "name" && property != "space_bone" && property != "parent_control") error = "property is not text editable";
        if (error.empty()) committed = session().update_controls(skeleton(), changes, &error);
    } else if (session().document().selectedControls.empty() && !session().document().selectedNodes.empty()) {
        std::vector<ControlRigNode> changes;
        for (ControlRigNodeId id : session().document().selectedNodes) {
            const auto found = std::find_if(session().document().rig.nodes.begin(), session().document().rig.nodes.end(),
                [id](const auto& node) { return node.id == id; });
            if (found != session().document().rig.nodes.end()) changes.push_back(*found);
        }
        if (changes.size() != 1U) error = "text properties require one selected node";
        else if (property == "name") changes.front().name = value;
        else if (property == "bone" && !resolve_bone(value, changes.front().bone)) error = "bone search is ambiguous or has no match";
        else if (property == "source_bone" && !resolve_bone(value, changes.front().sourceBone)) error = "bone search is ambiguous or has no match";
        else if (property == "middle_bone" && !resolve_bone(value, changes.front().middleBone)) error = "bone search is ambiguous or has no match";
        else if (property == "end_bone" && !resolve_bone(value, changes.front().endBone)) error = "bone search is ambiguous or has no match";
        else if (property == "target_control" && !resolve_control(value, changes.front().targetControl)) error = "control search is ambiguous or has no match";
        else if (property == "pole_control" && !resolve_control(value, changes.front().poleControl)) error = "control search is ambiguous or has no match";
        else if (property != "name" && property != "bone" && property != "source_bone" &&
                 property != "middle_bone" && property != "end_bone" && property != "target_control" &&
                 property != "pole_control") error = "property is not text editable";
        if (error.empty()) committed = session().update_nodes(skeleton(), changes, &error);
    } else error = "select one control or node to edit text properties";
    if (committed) cancel_inspector_edit();
    set_status(committed ? "Inspector property committed" : error);
    return true;
}

bool EditorControlRigPanel::activate_context_action(std::string_view actionId) {
    std::string error;
    if (actionId == "add.transform_control") {
        ControlRigControl control;
        control.id = kInvalidControlRigControlId;
        control.name = "Control " + std::to_string(next_control_id(session().document().rig));
        const auto id = session().add_control(std::move(control), contextGraphPosition_, &error);
        if (id != 0U) { clear_selection(); session().document().selectedControls.insert(id); }
        set_status(id != 0U ? "Transform control added" : error);
        return id != 0U;
    }
    if (actionId == "add.set_bone") {
        ControlRigNode node;
        node.name = "Set Bone " + std::to_string(next_node_id(session().document().rig));
        node.bone = skeleton().bones.empty() ? kInvalidBoneIndex : 0U;
        const auto id = session().add_node(std::move(node), contextGraphPosition_, &error);
        if (id != 0U) { clear_selection(); session().document().selectedNodes.insert(id); }
        set_status(id != 0U ? "Set Bone node added" : error);
        return id != 0U;
    }
    if (actionId == "add.two_bone_ik") {
        ControlRigNode node;
        node.name = "Two Bone IK " + std::to_string(next_node_id(session().document().rig));
        node.kind = ControlRigNodeKind::TwoBoneIk;
        if (skeleton().bones.size() >= 3U) {
            node.bone = 0U; node.middleBone = 1U; node.endBone = 2U;
        }
        const auto id = session().add_node(std::move(node), contextGraphPosition_, &error);
        if (id != 0U) { clear_selection(); session().document().selectedNodes.insert(id); }
        set_status(id != 0U ? "Two Bone IK node added" : error);
        return id != 0U;
    }
    if (actionId == "add.comment") {
        const auto id = session().add_comment("Rig note", contextGraphPosition_, {360.0F, 220.0F}, &error);
        selectedComment_ = id == 0U ? std::nullopt : std::optional<std::uint64_t>(id);
        set_status(id != 0U ? "Comment added" : error);
        return id != 0U;
    }
    if (actionId == "delete.selection") { delete_selection(); return true; }
    return false;
}

SkeletonAsset make_control_rig_editor_demo_skeleton() {
    SkeletonAsset skeleton;
    skeleton.name = "Control Rig Preview Skeleton";
    skeleton.bones = {
        {"root", -1, make_rigid_transform({}, {})},
        {"upper", 0, make_rigid_transform({0.8F, 0.0F, 0.0F}, {})},
        {"lower", 1, make_rigid_transform({0.8F, 0.0F, 0.0F}, {})},
        {"hand", 2, make_rigid_transform({0.55F, 0.0F, 0.0F}, {})},
    };
    return skeleton;
}

ControlRigAuthoringDocument make_control_rig_editor_demo_document() {
    ControlRigAuthoringDocument document;
    document.rig.name = "Native Control Rig";
    ControlRigAuthoringSession session(std::move(document));
    std::string error;
    ControlRigControl target;
    target.name = "Hand Target";
    target.defaultLocal = make_rigid_transform({2.0F, 0.8F, 0.0F}, {});
    const auto targetId = session.add_control(std::move(target), {30.0F, 70.0F}, &error);
    ControlRigControl pole;
    pole.name = "Elbow Pole";
    pole.kind = ControlRigControlKind::Translation;
    pole.defaultLocal = make_rigid_transform({0.8F, 0.0F, 0.8F}, {});
    const auto poleId = session.add_control(std::move(pole), {30.0F, 260.0F}, &error);
    ControlRigNode ik;
    ik.name = "Arm IK";
    ik.kind = ControlRigNodeKind::TwoBoneIk;
    ik.bone = 1U; ik.middleBone = 2U; ik.endBone = 3U;
    const auto ikId = session.add_node(std::move(ik), {390.0F, 90.0F}, &error);
    if (targetId != 0U && ikId != 0U)
        (void)session.connect({ControlRigGraphEntityKind::Control, targetId, "Position"},
                              {ControlRigGraphEntityKind::Node, ikId, "Target"}, &error);
    if (poleId != 0U && ikId != 0U)
        (void)session.connect({ControlRigGraphEntityKind::Control, poleId, "Position"},
                              {ControlRigGraphEntityKind::Node, ikId, "Pole"}, &error);
    (void)session.add_comment("Forward Solve: procedural arm controls", {340.0F, 35.0F}, {340.0F, 260.0F}, &error);
    session.clear_history();
    return session.document();
}

} // namespace dve::editor
