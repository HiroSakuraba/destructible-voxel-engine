#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dve/control_rig.hpp"

namespace dve::editor {

struct ControlRigGraphPoint { float x{}; float y{}; };
struct ControlRigGraphSize { float width{240.0F}; float height{120.0F}; };
struct ControlRigEditorColor { float r{1.0F}; float g{1.0F}; float b{1.0F}; float a{1.0F}; };

enum class ControlRigGraphEntityKind : std::uint8_t { Control, Node };
enum class ControlRigGraphPinDirection : std::uint8_t { Input, Output };
enum class ControlRigGraphPinType : std::uint8_t { Execute, Transform, Position, Rotation };
enum class ControlRigControlShape : std::uint8_t { Cross, Box, Circle, Sphere, Arrow };
enum class ControlRigDiagnosticSeverity : std::uint8_t { Info, Warning, Error };

struct ControlRigGraphPinSpec {
    std::string name;
    ControlRigGraphPinDirection direction{ControlRigGraphPinDirection::Input};
    ControlRigGraphPinType type{ControlRigGraphPinType::Execute};
    bool required{};
    bool multiple{};
};

struct ControlRigGraphEndpoint {
    ControlRigGraphEntityKind kind{ControlRigGraphEntityKind::Node};
    std::uint64_t entity{};
    std::string pin;
};

struct ControlRigGraphLink {
    std::uint64_t id{};
    ControlRigGraphEndpoint from;
    ControlRigGraphEndpoint to;
};

struct ControlRigGraphNodeLayout {
    ControlRigGraphPoint position{};
    ControlRigGraphSize size{};
    bool collapsed{};
};

struct ControlRigControlVisual {
    ControlRigControlShape shape{ControlRigControlShape::Cross};
    float sizeMeters{0.15F};
    ControlRigEditorColor color{0.95F, 0.7F, 0.1F, 1.0F};
    bool visible{true};
};

struct ControlRigGraphComment {
    std::uint64_t id{};
    std::string text;
    ControlRigGraphPoint position{};
    ControlRigGraphSize size{360.0F, 220.0F};
    ControlRigEditorColor color{0.18F, 0.22F, 0.28F, 0.7F};
};

struct ControlRigAuthoringDiagnostic {
    ControlRigDiagnosticSeverity severity{ControlRigDiagnosticSeverity::Error};
    std::string message;
    ControlRigGraphEntityKind entityKind{ControlRigGraphEntityKind::Node};
    std::uint64_t entity{};
};

struct ControlRigAuthoringDocument {
    ControlRigAsset rig;
    std::map<ControlRigControlId, ControlRigGraphNodeLayout> controlLayouts;
    std::map<ControlRigNodeId, ControlRigGraphNodeLayout> nodeLayouts;
    std::map<ControlRigControlId, ControlRigControlVisual> controlVisuals;
    std::vector<ControlRigGraphLink> links;
    std::vector<ControlRigGraphComment> comments;
    std::set<ControlRigControlId> selectedControls;
    std::set<ControlRigNodeId> selectedNodes;
    std::uint64_t nextLinkId{1U};
    std::uint64_t nextCommentId{1U};
    std::uint64_t revision{};

    [[nodiscard]] std::vector<ControlRigAuthoringDiagnostic> diagnostics(
        const SkeletonAsset& skeleton) const;
    [[nodiscard]] bool compile(
        const SkeletonAsset& skeleton, ControlRigAsset& output,
        std::string* error = nullptr) const;
};

[[nodiscard]] std::vector<ControlRigGraphPinSpec> control_rig_graph_pins(
    const ControlRigControl& control);
[[nodiscard]] std::vector<ControlRigGraphPinSpec> control_rig_graph_pins(
    const ControlRigNode& node);

class ControlRigAuthoringSession {
public:
    explicit ControlRigAuthoringSession(ControlRigAuthoringDocument document = {});

    [[nodiscard]] ControlRigAuthoringDocument& document() noexcept { return document_; }
    [[nodiscard]] const ControlRigAuthoringDocument& document() const noexcept { return document_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::string_view undo_label() const noexcept;
    [[nodiscard]] std::string_view redo_label() const noexcept;
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    void clear_history() noexcept;

    [[nodiscard]] ControlRigControlId add_control(
        ControlRigControl control, ControlRigGraphPoint position,
        std::string* error = nullptr);
    [[nodiscard]] ControlRigNodeId add_node(
        ControlRigNode node, ControlRigGraphPoint position,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_control(ControlRigControlId control, std::string* error = nullptr);
    [[nodiscard]] bool remove_node(ControlRigNodeId node, std::string* error = nullptr);
    [[nodiscard]] bool move_control(ControlRigControlId control, ControlRigGraphPoint position);
    [[nodiscard]] bool move_node(ControlRigNodeId node, ControlRigGraphPoint position);
    [[nodiscard]] bool set_control_visual(
        ControlRigControlId control, ControlRigControlVisual visual,
        std::string* error = nullptr);
    // Inspector edits replace a validated set of complete records in one atomic undo step.
    [[nodiscard]] bool update_controls(
        const SkeletonAsset& skeleton, std::span<const ControlRigControl> controls,
        std::string* error = nullptr);
    [[nodiscard]] bool update_nodes(
        const SkeletonAsset& skeleton, std::span<const ControlRigNode> nodes,
        std::string* error = nullptr);
    [[nodiscard]] bool set_control_visuals(
        const std::map<ControlRigControlId, ControlRigControlVisual>& visuals,
        std::string* error = nullptr);
    [[nodiscard]] bool set_control_default(
        ControlRigControlId control, RigidTransform value,
        std::string* error = nullptr);
    [[nodiscard]] std::uint64_t connect(
        ControlRigGraphEndpoint from, ControlRigGraphEndpoint to,
        std::string* error = nullptr);
    [[nodiscard]] bool disconnect(std::uint64_t link);
    [[nodiscard]] std::uint64_t add_comment(
        std::string text, ControlRigGraphPoint position, ControlRigGraphSize size,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_comment(std::uint64_t comment);

    [[nodiscard]] bool switch_control_space_preserve_model(
        const SkeletonAsset& skeleton, std::span<const RigidTransform> inputPose,
        const std::map<ControlRigControlId, RigidTransform>& controlLocals,
        ControlRigControlId control, ControlRigSpace newSpace,
        BoneIndex newSpaceBone, ControlRigControlId newParentControl,
        std::string* error = nullptr);
    [[nodiscard]] bool match_control_to_bone(
        const SkeletonAsset& skeleton, std::span<const RigidTransform> inputPose,
        const std::map<ControlRigControlId, RigidTransform>& controlLocals,
        ControlRigControlId control, BoneIndex bone,
        std::string* error = nullptr);
    [[nodiscard]] bool match_two_bone_ik(
        const SkeletonAsset& skeleton, std::span<const RigidTransform> inputPose,
        const std::map<ControlRigControlId, RigidTransform>& controlLocals,
        ControlRigNodeId node, std::string* error = nullptr);

private:
    struct HistoryEntry { std::string label; ControlRigAuthoringDocument state; };
    [[nodiscard]] bool mutate(
        std::string label,
        const std::function<bool(ControlRigAuthoringDocument&, std::string*)>& operation,
        std::string* error = nullptr);

    ControlRigAuthoringDocument document_;
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
    std::size_t historyLimit_{128U};
};

struct ControlRigViewportLine {
    Float3 from{};
    Float3 to{};
    ControlRigEditorColor color{};
    ControlRigControlId control{kInvalidControlRigControlId};
};

[[nodiscard]] std::vector<ControlRigViewportLine> build_control_rig_viewport_lines(
    const ControlRigAuthoringDocument& document,
    const std::map<ControlRigControlId, RigidTransform>& controlModels);
[[nodiscard]] std::optional<ControlRigControlId> pick_control_rig_viewport_control(
    const ControlRigAuthoringDocument& document,
    const std::map<ControlRigControlId, RigidTransform>& controlModels,
    Float3 rayOrigin, Float3 rayDirection, float toleranceMeters = 0.025F);

[[nodiscard]] bool write_control_rig_authoring_layout(
    const std::filesystem::path& path, const ControlRigAuthoringDocument& document,
    std::string* error = nullptr);
[[nodiscard]] bool read_control_rig_authoring_layout(
    const std::filesystem::path& path, const ControlRigAsset& rig,
    ControlRigAuthoringDocument& document, std::string* error = nullptr,
    std::uint64_t maximumBytes = 8ULL * 1024ULL * 1024ULL);

} // namespace dve::editor
