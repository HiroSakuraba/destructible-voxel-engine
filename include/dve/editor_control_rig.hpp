#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/control_rig_authoring.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

struct ControlRigEditorPinView {
    ControlRigGraphEndpoint endpoint;
    ControlRigGraphPinDirection direction{ControlRigGraphPinDirection::Input};
    ControlRigGraphPinType type{ControlRigGraphPinType::Execute};
    UiRect rect{};
    std::string label;
};

struct ControlRigEditorCardView {
    ControlRigGraphEntityKind kind{ControlRigGraphEntityKind::Node};
    std::uint64_t id{};
    UiRect rect{};
    std::string title;
    std::string subtitle;
    bool selected{};
    bool hovered{};
    ControlRigDiagnosticSeverity diagnostic{ControlRigDiagnosticSeverity::Info};
    std::size_t diagnosticCount{};
    std::vector<ControlRigEditorPinView> pins;
};

struct ControlRigEditorLinkView {
    std::uint64_t id{};
    ControlRigGraphPinType type{ControlRigGraphPinType::Execute};
    int fromX{};
    int fromY{};
    int toX{};
    int toY{};
    bool hovered{};
};

struct ControlRigEditorCommentView {
    std::uint64_t id{};
    UiRect rect{};
    std::string text;
    ControlRigEditorColor color{};
};

struct ControlRigEditorPreviewLineView {
    int fromX{};
    int fromY{};
    int toX{};
    int toY{};
    ControlRigEditorColor color{};
    ControlRigControlId control{kInvalidControlRigControlId};
    bool selected{};
};

struct ControlRigEditorContextActionView {
    std::string id;
    std::string label;
    UiRect rect{};
    bool hovered{};
};

struct ControlRigEditorTabView {
    std::size_t index{};
    std::string title;
    UiRect rect{};
    bool active{};
    bool dirty{};
    bool recovered{};
};

struct ControlRigInspectorPropertyView {
    std::string id;
    std::string label;
    std::string value;
    UiRect row{};
    UiRect decrementButton{};
    UiRect incrementButton{};
    bool editableText{};
    bool mixed{};
    bool activeEdit{};
};

struct ControlRigEditorFrame {
    UiRect panel{};
    UiRect titleBar{};
    UiRect closeButton{};
    UiRect toolbar{};
    UiRect tabBar{};
    UiRect canvas{};
    UiRect preview{};
    UiRect inspector{};
    std::vector<ControlRigEditorCardView> cards;
    std::vector<ControlRigEditorLinkView> links;
    std::vector<ControlRigEditorCommentView> comments;
    std::vector<ControlRigEditorPreviewLineView> previewLines;
    std::vector<ControlRigEditorContextActionView> contextActions;
    std::vector<ControlRigEditorTabView> tabs;
    std::vector<ControlRigInspectorPropertyView> inspectorProperties;
    std::optional<ControlRigEditorPinView> draggedPin;
    int draggedPinX{};
    int draggedPinY{};
    std::optional<UiRect> marquee;
    std::optional<ControlRigControlId> previewSelection;
    int gizmoOriginX{};
    int gizmoOriginY{};
    int gizmoAxis{};
    bool gizmoCaptured{};
    float zoom{1.0F};
    std::string status;
};

struct ControlRigAssetPaths {
    std::filesystem::path rig;
    std::filesystem::path layout;
    std::filesystem::path skeleton;
};

struct ControlRigPairSaveOptions {
    // Deterministic failure injection used by recovery tests and host fault simulation.
    bool failAfterRuntimeCommit{};
};

struct ControlRigPairSaveResult {
    bool success{};
    std::string error;
    std::uint64_t rigHash{};
};

[[nodiscard]] ControlRigPairSaveResult save_control_rig_pair_transactional(
    const ControlRigAssetPaths& paths, const SkeletonAsset& skeleton,
    const ControlRigAuthoringDocument& document,
    ControlRigPairSaveOptions options = {});

[[nodiscard]] bool load_control_rig_pair(
    const ControlRigAssetPaths& paths, const SkeletonAsset& skeleton,
    ControlRigAuthoringDocument& document, std::string* error = nullptr,
    bool layoutOptional = true);

// Platform-neutral native control-rig editor. It owns interaction state while the existing
// IEditorCanvas implementations do the painting, so X11, SDL3, Win32, and Cocoa hosts all
// consume exactly the same hit-testing, graph navigation, and pointer-capture behavior.
class EditorControlRigPanel {
public:
    EditorControlRigPanel();

    [[nodiscard]] bool open() const noexcept { return open_; }
    void open_document(SkeletonAsset skeleton, ControlRigAuthoringDocument document);
    [[nodiscard]] bool open_asset(ControlRigAssetPaths paths, bool recoverIfNewer = true,
                                  std::string* error = nullptr);
    void open_demo();
    void close() noexcept;
    void toggle();
    void resize(int width, int height);

    [[nodiscard]] ControlRigAuthoringSession& session() noexcept;
    [[nodiscard]] const ControlRigAuthoringSession& session() const noexcept;
    [[nodiscard]] const SkeletonAsset& skeleton() const noexcept;
    [[nodiscard]] std::size_t tab_count() const noexcept { return documents_.size(); }
    [[nodiscard]] std::size_t active_tab() const noexcept { return activeDocument_; }
    [[nodiscard]] std::vector<ControlRigEditorTabView> tab_views() const;
    [[nodiscard]] bool switch_tab(std::size_t index) noexcept;
    [[nodiscard]] bool close_tab(std::size_t index, bool discardChanges = false,
                                 std::string* error = nullptr);
    [[nodiscard]] bool dirty(std::size_t index) const noexcept;
    [[nodiscard]] bool save_active(std::string* error = nullptr,
                                   ControlRigPairSaveOptions options = {});
    [[nodiscard]] bool save_active_as(ControlRigAssetPaths paths, std::string* error = nullptr,
                                      ControlRigPairSaveOptions options = {});
    [[nodiscard]] bool autosave_active(std::string* error = nullptr);
    void update(float elapsedSeconds);
    [[nodiscard]] const std::vector<std::filesystem::path>& recent_documents() const noexcept {
        return recentDocuments_;
    }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }
    [[nodiscard]] float zoom() const noexcept { return zoom_; }
    [[nodiscard]] bool pointer_captured() const noexcept { return capture_ != Capture::NoCapture; }
    [[nodiscard]] ControlRigEditorFrame frame() const;

    [[nodiscard]] bool pointer_move(int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool pointer_down(int button, int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool pointer_up(int button, int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool pointer_wheel(float steps, int x, int y, std::uint32_t modifiers = 0U);
    [[nodiscard]] bool key_down(std::string_view key, bool control, bool shift, bool alt);
    [[nodiscard]] bool text_input(std::string_view text);
    [[nodiscard]] bool adjust_inspector_property(
        std::string_view propertyId, int direction, std::string* error = nullptr);

private:
    enum class Capture : std::uint8_t { NoCapture, Pan, MoveCard, Marquee, Link, Gizmo };
    struct Hit {
        enum class Kind : std::uint8_t { NoKind, Card, Pin, Link, Comment, PreviewControl, GizmoAxis } kind{Kind::NoKind};
        ControlRigGraphEntityKind entityKind{ControlRigGraphEntityKind::Node};
        std::uint64_t id{};
        std::optional<ControlRigEditorPinView> pin;
        int axis{};
    };

    [[nodiscard]] Hit hit_test(int x, int y) const;
    [[nodiscard]] ControlRigGraphPoint screen_to_graph(int x, int y) const noexcept;
    [[nodiscard]] std::pair<int, int> graph_to_screen(ControlRigGraphPoint point) const noexcept;
    [[nodiscard]] bool activate_context_action(std::string_view actionId);
    struct DocumentState {
        SkeletonAsset skeleton;
        ControlRigAuthoringSession session;
        ControlRigAssetPaths paths;
        std::uint64_t savedRevision{};
        std::uint64_t autosavedRevision{};
        bool forceDirty{};
        bool recovered{};
    };
    [[nodiscard]] DocumentState& active_document() noexcept;
    [[nodiscard]] const DocumentState& active_document() const noexcept;
    void add_recent(const std::filesystem::path& path);
    [[nodiscard]] static ControlRigAssetPaths autosave_paths(const ControlRigAssetPaths& paths);
    void select_entity(ControlRigGraphEntityKind kind, std::uint64_t id, bool additive);
    void clear_selection() noexcept;
    void delete_selection();
    void duplicate_selection();
    void frame_selection();
    [[nodiscard]] bool begin_inspector_edit(const ControlRigInspectorPropertyView& property);
    [[nodiscard]] bool commit_inspector_edit();
    void cancel_inspector_edit() noexcept;
    void set_status(std::string value) { status_ = std::move(value); }

    bool open_{};
    int width_{1280};
    int height_{800};
    std::vector<std::unique_ptr<DocumentState>> documents_;
    std::size_t activeDocument_{};
    std::vector<std::filesystem::path> recentDocuments_;
    float autosaveElapsed_{};
    float autosaveIntervalSeconds_{30.0F};
    float zoom_{1.0F};
    ControlRigGraphPoint pan_{24.0F, 24.0F};
    int pointerX_{};
    int pointerY_{};
    int pointerDownX_{};
    int pointerDownY_{};
    Capture capture_{Capture::NoCapture};
    Hit hovered_{};
    ControlRigGraphEntityKind dragEntityKind_{ControlRigGraphEntityKind::Node};
    std::uint64_t dragEntity_{};
    ControlRigGraphPoint dragEntityStart_{};
    ControlRigGraphPoint dragPointerStart_{};
    ControlRigGraphPoint dragDelta_{};
    std::optional<ControlRigEditorPinView> dragPin_{};
    std::optional<std::uint64_t> selectedComment_{};
    bool contextOpen_{};
    ControlRigGraphPoint contextGraphPosition_{};
    int contextX_{};
    int contextY_{};
    std::optional<ControlRigControlId> previewSelection_{};
    int gizmoAxis_{};
    RigidTransform gizmoStart_{};
    RigidTransform gizmoPreview_{};
    std::string editingProperty_;
    std::string inspectorTextBuffer_;
    bool replaceInspectorText_{};
    std::string status_{"Control Rig ready"};
};

[[nodiscard]] ControlRigAuthoringDocument make_control_rig_editor_demo_document();
[[nodiscard]] SkeletonAsset make_control_rig_editor_demo_skeleton();

} // namespace dve::editor
