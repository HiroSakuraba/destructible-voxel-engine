#pragma once

#include "dve/sprite_animation.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dve {

enum class SpriteAnimationMachineSelectionKind : std::uint8_t {
    NoSelection,
    Parameter,
    State,
    Transition,
};

struct SpriteAnimationMachineSelection {
    SpriteAnimationMachineSelectionKind kind{SpriteAnimationMachineSelectionKind::NoSelection};
    std::size_t index{};
};

enum class SpriteAnimationMachineExternalChange : std::uint8_t {
    Unchanged,
    Reloaded,
    Conflict,
    Missing,
    Failed,
};

struct SpriteAnimationMachineExternalChangeResult {
    SpriteAnimationMachineExternalChange state{SpriteAnimationMachineExternalChange::Unchanged};
    std::string message;
};

// Transactional graph-document model used by native, SDL, Qt, and headless state-machine tools.
// Every edit validates against the associated sprite asset before commit and records bounded undo.
class SpriteAnimationMachineAuthoringSession {
public:
    SpriteAnimationMachineAuthoringSession() = default;

    [[nodiscard]] const SpriteAnimationStateMachineAsset& asset() const noexcept { return asset_; }
    [[nodiscard]] const SpriteAsset& sprite_asset() const noexcept { return sprite_; }
    [[nodiscard]] SpriteAnimationMachineSelection selection() const noexcept { return selection_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

    [[nodiscard]] bool create(SpriteAsset sprite, std::string name,
                              std::string initialClip, std::string* error = nullptr);
    [[nodiscard]] bool open(const std::filesystem::path& path, SpriteAsset sprite,
                            std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path = {},
                            std::string* error = nullptr);

    [[nodiscard]] bool select(SpriteAnimationMachineSelection selection) noexcept;
    [[nodiscard]] bool add_state(std::string name, std::string clip, SpriteVec2 graphPosition,
                                 std::string* error = nullptr);
    [[nodiscard]] bool rename_state(std::size_t index, std::string name,
                                    std::string* error = nullptr);
    [[nodiscard]] bool move_state(std::size_t index, SpriteVec2 graphPosition,
                                  std::string* error = nullptr);
    [[nodiscard]] bool remove_state(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool set_initial_state(std::string_view state, std::string* error = nullptr);

    [[nodiscard]] bool add_parameter(SpriteAnimationParameterDefinition parameter,
                                     std::string* error = nullptr);
    [[nodiscard]] bool remove_parameter(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool add_transition(SpriteAnimationTransition transition,
                                      std::string* error = nullptr);
    [[nodiscard]] bool update_transition(std::size_t index, SpriteAnimationTransition transition,
                                         std::string* error = nullptr);
    [[nodiscard]] bool remove_transition(std::size_t index, std::string* error = nullptr);

    // Commit a complete validated document replacement as one undo transaction.
    [[nodiscard]] bool replace_asset(SpriteAnimationStateMachineAsset asset,
                                     SpriteAnimationMachineSelection selection = {},
                                     std::string* error = nullptr);

    [[nodiscard]] bool undo(std::string* error = nullptr);
    [[nodiscard]] bool redo(std::string* error = nullptr);
    [[nodiscard]] SpriteAnimationMachineExternalChangeResult poll_external_change(
        bool force = false);

private:
    struct Snapshot {
        SpriteAnimationStateMachineAsset asset;
        SpriteAnimationMachineSelection selection;
    };

    template<class Edit>
    [[nodiscard]] bool apply_edit(Edit&& edit, std::string* error);
    void normalize_selection() noexcept;
    void remember_signature() noexcept;

    SpriteAsset sprite_;
    SpriteAnimationStateMachineAsset asset_;
    SpriteAnimationMachineSelection selection_{};
    std::filesystem::path path_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool dirty_{};
    std::filesystem::file_time_type writeTime_{};
    std::uintmax_t fileSize_{};
    bool signatureValid_{};
};


struct SpriteAnimationGraphRect {
    float x{};
    float y{};
    float width{};
    float height{};
    [[nodiscard]] bool contains(SpriteVec2 point) const noexcept {
        return point.x >= x && point.y >= y && point.x < x + width && point.y < y + height;
    }
    [[nodiscard]] bool intersects(const SpriteAnimationGraphRect& other) const noexcept {
        return x < other.x + other.width && x + width > other.x &&
            y < other.y + other.height && y + height > other.y;
    }
};

enum class SpriteAnimationGraphBadgeSeverity : std::uint8_t { Info, Warning, Error };

struct SpriteAnimationGraphBadge {
    SpriteAnimationGraphBadgeSeverity severity{SpriteAnimationGraphBadgeSeverity::Info};
    std::string text;
};

struct SpriteAnimationGraphNodeFrame {
    std::size_t stateIndex{};
    SpriteAnimationGraphRect rect{};
    SpriteAnimationGraphRect inputPort{};
    SpriteAnimationGraphRect outputPort{};
    std::string title;
    std::string subtitle;
    std::vector<SpriteAnimationGraphBadge> badges;
    bool selected{};
    bool initial{};
    bool live{};
};

struct SpriteAnimationGraphEdgeFrame {
    std::size_t transitionIndex{};
    SpriteVec2 from{};
    SpriteVec2 controlA{};
    SpriteVec2 controlB{};
    SpriteVec2 to{};
    std::vector<SpriteVec2> reroutePoints;
    std::string label;
    std::string conditionSummary;
    std::vector<SpriteAnimationGraphBadge> badges;
    bool selected{};
    bool anyState{};
    bool live{};
};

struct SpriteAnimationGraphCommentFrame {
    std::uint64_t id{};
    SpriteAnimationGraphRect rect{};
    std::string text;
};

struct SpriteAnimationGraphGroupFrame {
    std::uint64_t id{};
    SpriteAnimationGraphRect rect{};
    std::string name;
    std::vector<std::size_t> stateIndices;
};

struct SpriteAnimationGraphMiniMap {
    SpriteAnimationGraphRect rect{};
    SpriteAnimationGraphRect visibleGraphBounds{};
    std::vector<SpriteAnimationGraphRect> nodes;
};

struct SpriteAnimationGraphFrame {
    SpriteAnimationGraphRect viewport{};
    std::vector<SpriteAnimationGraphNodeFrame> nodes;
    std::vector<SpriteAnimationGraphEdgeFrame> edges;
    std::vector<SpriteAnimationGraphCommentFrame> comments;
    std::vector<SpriteAnimationGraphGroupFrame> groups;
    SpriteAnimationGraphMiniMap minimap{};
    std::vector<std::string> breadcrumbs;
    std::optional<SpriteAnimationGraphRect> marquee;
    std::optional<std::pair<SpriteVec2, SpriteVec2>> transitionPreview;
};

struct SpriteAnimationGraphComment {
    std::uint64_t id{};
    SpriteVec2 graphPosition{};
    SpriteVec2 size{240.0F, 96.0F};
    std::string text;
};

struct SpriteAnimationGraphGroup {
    std::uint64_t id{};
    std::string name;
    std::set<std::size_t> stateIndices;
    float padding{28.0F};
};

struct SpriteAnimationGraphSubgraph {
    std::string name;
    std::set<std::size_t> stateIndices;
};

struct SpriteAnimationGraphViewState {
    float zoom{1.0F};
    SpriteVec2 pan{};
    SpriteVec2 nodeSize{170.0F, 72.0F};
};

// Native-ready visual graph workspace. It provides deterministic layout, hit testing, zoom/pan,
// marquee and multi-selection, node dragging, transition creation, copy/paste, search, and live
// owner highlighting while routing all mutations through SpriteAnimationMachineAuthoringSession.
class SpriteAnimationMachineGraphWorkspace {
public:
    [[nodiscard]] SpriteAnimationMachineAuthoringSession& session() noexcept { return session_; }
    [[nodiscard]] const SpriteAnimationMachineAuthoringSession& session() const noexcept {
        return session_;
    }
    [[nodiscard]] SpriteAnimationGraphViewState& view() noexcept { return view_; }
    [[nodiscard]] const SpriteAnimationGraphViewState& view() const noexcept { return view_; }
    [[nodiscard]] const std::set<std::size_t>& selected_states() const noexcept {
        return selectedStates_;
    }
    [[nodiscard]] std::optional<std::size_t> selected_transition() const noexcept {
        return selectedTransition_;
    }
    [[nodiscard]] const std::string& search_query() const noexcept { return searchQuery_; }

    [[nodiscard]] bool create(SpriteAsset sprite, std::string name, std::string initialClip,
                              std::string* error = nullptr);
    [[nodiscard]] bool open(const std::filesystem::path& path, SpriteAsset sprite,
                            std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path = {}, std::string* error = nullptr) {
        return session_.save(path, error);
    }
    [[nodiscard]] SpriteAnimationGraphFrame frame(
        SpriteAnimationGraphRect viewport) const;

    void set_search_query(std::string query);
    [[nodiscard]] bool select_search_result(int direction = 1) noexcept;
    void set_live_state(std::string current, std::string previous = {});
    void set_live_transition(std::optional<std::size_t> transition) noexcept {
        liveTransition_ = transition;
    }
    void clear_live_state() noexcept;

    [[nodiscard]] bool inspect_transition(std::size_t index,
        const SpriteAnimationTransition& replacement, std::string* error = nullptr);
    [[nodiscard]] bool set_transition_reroute(std::size_t index,
        std::vector<SpriteVec2> graphPoints, std::string* error = nullptr);
    [[nodiscard]] bool add_comment(SpriteVec2 graphPosition, SpriteVec2 size,
        std::string text, std::uint64_t* id = nullptr, std::string* error = nullptr);
    [[nodiscard]] bool add_group(std::string name, std::set<std::size_t> states,
        std::uint64_t* id = nullptr, std::string* error = nullptr);
    [[nodiscard]] bool create_subgraph(std::string name, std::set<std::size_t> states,
        std::string* error = nullptr);
    void enter_subgraph(std::string_view name) noexcept;
    void leave_subgraph() noexcept;
    [[nodiscard]] const std::vector<SpriteAnimationGraphComment>& comments() const noexcept {
        return comments_;
    }
    [[nodiscard]] const std::vector<SpriteAnimationGraphGroup>& groups() const noexcept {
        return groups_;
    }
    [[nodiscard]] const std::vector<SpriteAnimationGraphSubgraph>& subgraphs() const noexcept {
        return subgraphs_;
    }

    [[nodiscard]] bool pointer_down(int button, SpriteVec2 point,
                                    SpriteAnimationGraphRect viewport,
                                    bool control = false, bool shift = false);
    [[nodiscard]] bool pointer_move(SpriteVec2 point, SpriteAnimationGraphRect viewport);
    [[nodiscard]] bool pointer_up(int button, SpriteVec2 point,
                                  SpriteAnimationGraphRect viewport,
                                  std::string* error = nullptr);
    [[nodiscard]] bool wheel(float steps, SpriteVec2 point,
                             SpriteAnimationGraphRect viewport) noexcept;
    [[nodiscard]] bool key_down(std::string_view key, bool control, bool shift,
                                std::string* error = nullptr);

private:
    enum class Capture : std::uint8_t { NoCapture, Pan, MoveNodes, Marquee, CreateTransition };
    struct Clipboard {
        std::vector<SpriteAnimationState> states;
        std::vector<SpriteAnimationTransition> transitions;
    };

    [[nodiscard]] SpriteVec2 graph_to_screen(
        SpriteVec2 graph, SpriteAnimationGraphRect viewport) const noexcept;
    [[nodiscard]] SpriteVec2 screen_to_graph(
        SpriteVec2 screen, SpriteAnimationGraphRect viewport) const noexcept;
    void normalize_selection() noexcept;
    [[nodiscard]] std::optional<std::size_t> node_at(
        SpriteVec2 point, SpriteAnimationGraphRect viewport, bool outputPortOnly = false) const;
    [[nodiscard]] std::optional<std::size_t> transition_at(
        SpriteVec2 point, SpriteAnimationGraphRect viewport) const;
    [[nodiscard]] bool copy_selection() noexcept;
    [[nodiscard]] bool paste_selection(std::string* error);
    [[nodiscard]] bool delete_selection(std::string* error);

    SpriteAnimationMachineAuthoringSession session_;
    SpriteAnimationGraphViewState view_{};
    std::set<std::size_t> selectedStates_;
    std::optional<std::size_t> selectedTransition_;
    std::string searchQuery_;
    std::string liveCurrentState_;
    std::string livePreviousState_;
    std::optional<std::size_t> liveTransition_;
    std::map<std::size_t, std::vector<SpriteVec2>> transitionReroutes_;
    std::vector<SpriteAnimationGraphComment> comments_;
    std::vector<SpriteAnimationGraphGroup> groups_;
    std::vector<SpriteAnimationGraphSubgraph> subgraphs_;
    std::vector<std::string> breadcrumbs_{"Root"};
    std::uint64_t nextAnnotationId_{1U};
    Capture capture_{Capture::NoCapture};
    SpriteVec2 pointerDown_{};
    SpriteVec2 capturePan_{};
    SpriteVec2 marqueeCurrent_{};
    std::map<std::size_t, SpriteVec2> capturePositions_;
    std::optional<std::size_t> transitionSource_;
    Clipboard clipboard_;
    std::uint32_t pasteSerial_{};
};

} // namespace dve
