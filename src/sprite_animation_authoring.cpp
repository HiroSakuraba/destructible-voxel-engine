#include "dve/sprite_animation_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <system_error>
#include <utility>

namespace dve {
namespace {

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

bool valid_index(SpriteAnimationMachineSelection selection,
                 const SpriteAnimationStateMachineAsset& asset) noexcept {
    switch (selection.kind) {
    case SpriteAnimationMachineSelectionKind::NoSelection: return true;
    case SpriteAnimationMachineSelectionKind::Parameter:
        return selection.index < asset.parameters.size();
    case SpriteAnimationMachineSelectionKind::State:
        return selection.index < asset.states.size();
    case SpriteAnimationMachineSelectionKind::Transition:
        return selection.index < asset.transitions.size();
    }
    return false;
}

} // namespace

template<class Edit>
bool SpriteAnimationMachineAuthoringSession::apply_edit(Edit&& edit, std::string* error) {
    Snapshot before{asset_, selection_};
    edit(asset_, selection_);
    normalize_selection();
    asset_.contentHash = 0U;
    std::string validation;
    if (!asset_.validate(sprite_, &validation)) {
        asset_ = std::move(before.asset);
        selection_ = before.selection;
        return fail(error, std::move(validation));
    }
    asset_.recompute_hash();
    undo_.push_back(std::move(before));
    constexpr std::size_t maximumUndo = 128U;
    if (undo_.size() > maximumUndo) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
    return true;
}

bool SpriteAnimationMachineAuthoringSession::create(
    SpriteAsset sprite, std::string name, std::string initialClip, std::string* error) {
    if (!sprite.validate(error)) return false;
    if (name.empty() || initialClip.empty() || find_sprite_clip(sprite, initialClip) == nullptr)
        return fail(error, "state-machine creation requires a name and existing initial clip");
    sprite_ = std::move(sprite);
    asset_ = {};
    asset_.name = std::move(name);
    asset_.compatibleSpriteAssetHash = sprite_.contentHash;
    asset_.initialState = "Entry";
    asset_.states.push_back({"Entry", std::move(initialClip), 1.0F, {80.0F, 80.0F}});
    asset_.recompute_hash();
    if (!asset_.validate(sprite_, error)) return false;
    selection_ = {SpriteAnimationMachineSelectionKind::State, 0U};
    path_.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = true;
    signatureValid_ = false;
    return true;
}

bool SpriteAnimationMachineAuthoringSession::open(
    const std::filesystem::path& path, SpriteAsset sprite, std::string* error) {
    if (!sprite.validate(error)) return false;
    const SpriteAnimationMachineReadResult read = read_dvesprite_machine(path);
    if (!read) return fail(error, read.error);
    if (!read.asset.validate(sprite, error)) return false;
    sprite_ = std::move(sprite);
    asset_ = read.asset;
    path_ = path;
    selection_ = {};
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    remember_signature();
    return true;
}

bool SpriteAnimationMachineAuthoringSession::save(
    const std::filesystem::path& path, std::string* error) {
    const std::filesystem::path target = path.empty() ? path_ : path;
    if (target.empty()) return fail(error, "state-machine save path is empty");
    asset_.recompute_hash();
    if (!write_dvesprite_machine(target, asset_, error)) return false;
    path_ = target;
    dirty_ = false;
    remember_signature();
    return true;
}

bool SpriteAnimationMachineAuthoringSession::select(
    SpriteAnimationMachineSelection selection) noexcept {
    if (!valid_index(selection, asset_)) return false;
    selection_ = selection;
    return true;
}

bool SpriteAnimationMachineAuthoringSession::add_state(
    std::string name, std::string clip, SpriteVec2 graphPosition, std::string* error) {
    if (name.empty() || clip.empty() || !std::isfinite(graphPosition.x) ||
        !std::isfinite(graphPosition.y))
        return fail(error, "state-machine state addition is invalid");
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection& selection) {
        asset.states.push_back({std::move(name), std::move(clip), 1.0F, graphPosition});
        selection = {SpriteAnimationMachineSelectionKind::State, asset.states.size() - 1U};
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::rename_state(
    std::size_t index, std::string name, std::string* error) {
    if (index >= asset_.states.size() || name.empty())
        return fail(error, "state-machine state rename is invalid");
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection&) {
        const std::string previous = asset.states[index].name;
        asset.states[index].name = std::move(name);
        if (asset.initialState == previous) asset.initialState = asset.states[index].name;
        for (SpriteAnimationTransition& transition : asset.transitions) {
            if (transition.fromState == previous) transition.fromState = asset.states[index].name;
            if (transition.toState == previous) transition.toState = asset.states[index].name;
        }
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::move_state(
    std::size_t index, SpriteVec2 graphPosition, std::string* error) {
    if (index >= asset_.states.size() || !std::isfinite(graphPosition.x) ||
        !std::isfinite(graphPosition.y))
        return fail(error, "state-machine graph movement is invalid");
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection&) {
        asset.states[index].graphPosition = graphPosition;
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::remove_state(
    std::size_t index, std::string* error) {
    if (index >= asset_.states.size() || asset_.states.size() <= 1U)
        return fail(error, "state-machine must retain at least one state");
    const std::string removed = asset_.states[index].name;
    if (asset_.initialState == removed)
        return fail(error, "state-machine initial state must be changed before removal");
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection& selection) {
        asset.states.erase(asset.states.begin() + static_cast<std::ptrdiff_t>(index));
        std::erase_if(asset.transitions, [&](const SpriteAnimationTransition& transition) {
            return transition.fromState == removed || transition.toState == removed;
        });
        selection = {};
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::set_initial_state(
    std::string_view state, std::string* error) {
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection&) {
        asset.initialState = std::string(state);
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::add_parameter(
    SpriteAnimationParameterDefinition parameter, std::string* error) {
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection& selection) {
        asset.parameters.push_back(std::move(parameter));
        selection = {SpriteAnimationMachineSelectionKind::Parameter,
                     asset.parameters.size() - 1U};
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::remove_parameter(
    std::size_t index, std::string* error) {
    if (index >= asset_.parameters.size())
        return fail(error, "state-machine parameter selection is invalid");
    const std::string removed = asset_.parameters[index].name;
    for (const SpriteAnimationTransition& transition : asset_.transitions) {
        if (std::any_of(transition.conditions.begin(), transition.conditions.end(),
                        [&](const SpriteAnimationCondition& condition) {
                            return condition.parameter == removed;
                        }))
            return fail(error, "state-machine parameter is still used by a transition");
    }
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection& selection) {
        asset.parameters.erase(asset.parameters.begin() + static_cast<std::ptrdiff_t>(index));
        selection = {};
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::add_transition(
    SpriteAnimationTransition transition, std::string* error) {
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection& selection) {
        asset.transitions.push_back(std::move(transition));
        selection = {SpriteAnimationMachineSelectionKind::Transition,
                     asset.transitions.size() - 1U};
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::update_transition(
    std::size_t index, SpriteAnimationTransition transition, std::string* error) {
    if (index >= asset_.transitions.size())
        return fail(error, "state-machine transition selection is invalid");
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection&) {
        asset.transitions[index] = std::move(transition);
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::remove_transition(
    std::size_t index, std::string* error) {
    if (index >= asset_.transitions.size())
        return fail(error, "state-machine transition selection is invalid");
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection& selection) {
        asset.transitions.erase(asset.transitions.begin() + static_cast<std::ptrdiff_t>(index));
        selection = {};
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::replace_asset(
    SpriteAnimationStateMachineAsset replacement, SpriteAnimationMachineSelection selection,
    std::string* error) {
    return apply_edit([&](SpriteAnimationStateMachineAsset& asset,
                          SpriteAnimationMachineSelection& currentSelection) {
        asset = std::move(replacement);
        currentSelection = selection;
    }, error);
}

bool SpriteAnimationMachineAuthoringSession::undo(std::string* error) {
    if (undo_.empty()) return fail(error, "state-machine undo history is empty");
    redo_.push_back({asset_, selection_});
    Snapshot snapshot = std::move(undo_.back());
    undo_.pop_back();
    asset_ = std::move(snapshot.asset);
    selection_ = snapshot.selection;
    normalize_selection();
    dirty_ = true;
    return true;
}

bool SpriteAnimationMachineAuthoringSession::redo(std::string* error) {
    if (redo_.empty()) return fail(error, "state-machine redo history is empty");
    undo_.push_back({asset_, selection_});
    Snapshot snapshot = std::move(redo_.back());
    redo_.pop_back();
    asset_ = std::move(snapshot.asset);
    selection_ = snapshot.selection;
    normalize_selection();
    dirty_ = true;
    return true;
}

SpriteAnimationMachineExternalChangeResult
SpriteAnimationMachineAuthoringSession::poll_external_change(bool force) {
    SpriteAnimationMachineExternalChangeResult result;
    if (path_.empty()) return result;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path_, ec);
    if (ec) {
        result.state = SpriteAnimationMachineExternalChange::Missing;
        result.message = "State-machine file is missing or unreadable";
        return result;
    }
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) {
        result.state = SpriteAnimationMachineExternalChange::Missing;
        result.message = "State-machine file size is unavailable";
        return result;
    }
    if (!force && signatureValid_ && time == writeTime_ && size == fileSize_) return result;
    if (dirty_) {
        result.state = SpriteAnimationMachineExternalChange::Conflict;
        result.message = "State-machine changed on disk while local edits are unsaved";
        return result;
    }
    const SpriteAnimationMachineReadResult read = read_dvesprite_machine(path_);
    std::string validation;
    if (!read || !read.asset.validate(sprite_, &validation)) {
        result.state = SpriteAnimationMachineExternalChange::Failed;
        result.message = read ? validation : read.error;
        return result;
    }
    asset_ = read.asset;
    selection_ = {};
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    writeTime_ = time;
    fileSize_ = size;
    signatureValid_ = true;
    result.state = SpriteAnimationMachineExternalChange::Reloaded;
    result.message = "Reloaded changed state-machine asset";
    return result;
}

void SpriteAnimationMachineAuthoringSession::normalize_selection() noexcept {
    if (!valid_index(selection_, asset_)) selection_ = {};
}

void SpriteAnimationMachineAuthoringSession::remember_signature() noexcept {
    signatureValid_ = false;
    if (path_.empty()) return;
    std::error_code ec;
    writeTime_ = std::filesystem::last_write_time(path_, ec);
    if (ec) return;
    fileSize_ = std::filesystem::file_size(path_, ec);
    signatureValid_ = !ec;
}



namespace {

[[nodiscard]] std::string graph_curve_name(SpriteAnimationBlendCurve value) {
    switch (value) {
    case SpriteAnimationBlendCurve::Linear: return "Linear";
    case SpriteAnimationBlendCurve::SmoothStep: return "SmoothStep";
    case SpriteAnimationBlendCurve::EaseIn: return "EaseIn";
    case SpriteAnimationBlendCurve::EaseOut: return "EaseOut";
    }
    return "Unknown";
}

[[nodiscard]] std::string graph_track_policy_name(SpriteAnimationBlendTrackPolicy value) {
    switch (value) {
    case SpriteAnimationBlendTrackPolicy::DestinationOnly: return "Dst";
    case SpriteAnimationBlendTrackPolicy::SourceOnly: return "Src";
    case SpriteAnimationBlendTrackPolicy::HighestWeight: return "Highest";
    case SpriteAnimationBlendTrackPolicy::BothWeighted: return "Weighted";
    }
    return "?";
}

[[nodiscard]] std::string graph_condition_summary(const SpriteAnimationTransition& transition) {
    if (transition.conditions.empty()) return transition.hasExitTime ? "exit time" : "always";
    std::string result;
    for (std::size_t index = 0U; index < transition.conditions.size(); ++index) {
        if (index != 0U) result += " && ";
        result += transition.conditions[index].parameter;
        if (result.size() > 60U) { result.resize(57U); result += "..."; break; }
    }
    return result;
}

[[nodiscard]] SpriteAnimationGraphRect graph_bounds(std::span<const SpriteAnimationGraphNodeFrame> nodes) {
    if (nodes.empty()) return {};
    float minimumX = nodes.front().rect.x;
    float minimumY = nodes.front().rect.y;
    float maximumX = nodes.front().rect.x + nodes.front().rect.width;
    float maximumY = nodes.front().rect.y + nodes.front().rect.height;
    for (const auto& node : nodes) {
        minimumX = std::min(minimumX, node.rect.x);
        minimumY = std::min(minimumY, node.rect.y);
        maximumX = std::max(maximumX, node.rect.x + node.rect.width);
        maximumY = std::max(maximumY, node.rect.y + node.rect.height);
    }
    return {minimumX, minimumY, maximumX - minimumX, maximumY - minimumY};
}

} // namespace

SpriteVec2 SpriteAnimationMachineGraphWorkspace::graph_to_screen(
    SpriteVec2 graph, SpriteAnimationGraphRect viewport) const noexcept {
    return {viewport.x + viewport.width * 0.5F + view_.pan.x + graph.x * view_.zoom,
            viewport.y + viewport.height * 0.5F + view_.pan.y + graph.y * view_.zoom};
}

SpriteVec2 SpriteAnimationMachineGraphWorkspace::screen_to_graph(
    SpriteVec2 screen, SpriteAnimationGraphRect viewport) const noexcept {
    const float inverse = view_.zoom > 0.0001F ? 1.0F / view_.zoom : 1.0F;
    return {(screen.x - viewport.x - viewport.width * 0.5F - view_.pan.x) * inverse,
            (screen.y - viewport.y - viewport.height * 0.5F - view_.pan.y) * inverse};
}

bool SpriteAnimationMachineGraphWorkspace::create(
    SpriteAsset sprite, std::string name, std::string initialClip, std::string* error) {
    if (!session_.create(std::move(sprite), std::move(name), std::move(initialClip), error))
        return false;
    selectedStates_.clear();
    selectedStates_.insert(0U);
    selectedTransition_.reset();
    view_ = {};
    transitionReroutes_.clear();
    comments_.clear();
    groups_.clear();
    subgraphs_.clear();
    breadcrumbs_ = {"Root"};
    nextAnnotationId_ = 1U;
    liveTransition_.reset();
    capture_ = Capture::NoCapture;
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::open(
    const std::filesystem::path& path, SpriteAsset sprite, std::string* error) {
    if (!session_.open(path, std::move(sprite), error)) return false;
    selectedStates_.clear();
    selectedTransition_.reset();
    view_ = {};
    transitionReroutes_.clear();
    comments_.clear();
    groups_.clear();
    subgraphs_.clear();
    breadcrumbs_ = {"Root"};
    nextAnnotationId_ = 1U;
    liveTransition_.reset();
    capture_ = Capture::NoCapture;
    return true;
}

SpriteAnimationGraphFrame SpriteAnimationMachineGraphWorkspace::frame(
    SpriteAnimationGraphRect viewport) const {
    SpriteAnimationGraphFrame result;
    result.viewport = viewport;
    const SpriteAnimationStateMachineAsset& asset = session_.asset();
    result.nodes.reserve(asset.states.size());
    const float width = view_.nodeSize.x * view_.zoom;
    const float height = view_.nodeSize.y * view_.zoom;
    for (std::size_t index = 0U; index < asset.states.size(); ++index) {
        const SpriteAnimationState& state = asset.states[index];
        SpriteVec2 graphPosition = state.graphPosition;
        if (capture_ == Capture::MoveNodes && selectedStates_.contains(index)) {
            graphPosition.x += (marqueeCurrent_.x - pointerDown_.x) / view_.zoom;
            graphPosition.y += (marqueeCurrent_.y - pointerDown_.y) / view_.zoom;
        }
        const SpriteVec2 center = graph_to_screen(graphPosition, viewport);
        SpriteAnimationGraphNodeFrame node;
        node.stateIndex = index;
        node.title = state.name;
        node.subtitle = state.clip + "  x" + std::to_string(state.playbackSpeed);
        node.rect = {center.x - width * 0.5F, center.y - height * 0.5F, width, height};
        const float portSize = std::clamp(14.0F * view_.zoom, 8.0F, 22.0F);
        node.inputPort = {node.rect.x - portSize * 0.5F,
                          center.y - portSize * 0.5F, portSize, portSize};
        node.outputPort = {node.rect.x + node.rect.width - portSize * 0.5F,
                           center.y - portSize * 0.5F, portSize, portSize};
        node.selected = selectedStates_.contains(index);
        node.initial = state.name == asset.initialState;
        node.live = state.name == liveCurrentState_ || state.name == livePreviousState_;
        if (state.clip.empty()) node.badges.push_back({SpriteAnimationGraphBadgeSeverity::Error, "Missing clip"});
        if (state.playbackSpeed <= 0.0F) node.badges.push_back({SpriteAnimationGraphBadgeSeverity::Warning, "Paused"});
        if (node.initial) node.badges.push_back({SpriteAnimationGraphBadgeSeverity::Info, "Initial"});
        result.nodes.push_back(std::move(node));
    }
    result.edges.reserve(asset.transitions.size());
    for (std::size_t index = 0U; index < asset.transitions.size(); ++index) {
        const SpriteAnimationTransition& transition = asset.transitions[index];
        const auto destination = std::find_if(result.nodes.begin(), result.nodes.end(),
            [&asset, &transition](const SpriteAnimationGraphNodeFrame& node) {
                return asset.states[node.stateIndex].name == transition.toState;
            });
        if (destination == result.nodes.end()) continue;
        SpriteAnimationGraphEdgeFrame edge;
        edge.transitionIndex = index;
        edge.to = {destination->inputPort.x + destination->inputPort.width * 0.5F,
                   destination->inputPort.y + destination->inputPort.height * 0.5F};
        edge.anyState = transition.fromState == "*";
        if (edge.anyState) {
            edge.from = {viewport.x + 18.0F, viewport.y + 28.0F +
                         static_cast<float>(index % 8U) * 11.0F};
        } else {
            const auto source = std::find_if(result.nodes.begin(), result.nodes.end(),
                [&asset, &transition](const SpriteAnimationGraphNodeFrame& node) {
                    return asset.states[node.stateIndex].name == transition.fromState;
                });
            if (source == result.nodes.end()) continue;
            edge.from = {source->outputPort.x + source->outputPort.width * 0.5F,
                         source->outputPort.y + source->outputPort.height * 0.5F};
        }
        const float bend = std::max(48.0F, std::fabs(edge.to.x - edge.from.x) * 0.35F);
        edge.controlA = {edge.from.x + bend, edge.from.y};
        edge.controlB = {edge.to.x - bend, edge.to.y};
        if (const auto reroute = transitionReroutes_.find(index); reroute != transitionReroutes_.end()) {
            edge.reroutePoints.reserve(reroute->second.size());
            for (const SpriteVec2 point : reroute->second)
                edge.reroutePoints.push_back(graph_to_screen(point, viewport));
        }
        edge.label = std::to_string(transition.priority) + " | " +
            graph_curve_name(transition.blendCurve) + " | " +
            graph_track_policy_name(transition.trackPolicy);
        edge.conditionSummary = graph_condition_summary(transition);
        if (transition.fromState == transition.toState)
            edge.badges.push_back({SpriteAnimationGraphBadgeSeverity::Warning, "Self transition"});
        if (transition.conditions.empty() && !transition.hasExitTime)
            edge.badges.push_back({SpriteAnimationGraphBadgeSeverity::Info, "Unconditional"});
        edge.selected = selectedTransition_ && *selectedTransition_ == index;
        edge.live = (liveTransition_ && *liveTransition_ == index) ||
            (transition.fromState == livePreviousState_ && transition.toState == liveCurrentState_);
        result.edges.push_back(std::move(edge));
    }
    if (capture_ == Capture::Marquee) {
        result.marquee = {
            std::min(pointerDown_.x, marqueeCurrent_.x),
            std::min(pointerDown_.y, marqueeCurrent_.y),
            std::fabs(pointerDown_.x - marqueeCurrent_.x),
            std::fabs(pointerDown_.y - marqueeCurrent_.y)};
    }
    for (const SpriteAnimationGraphComment& comment : comments_) {
        const SpriteVec2 center = graph_to_screen(comment.graphPosition, viewport);
        result.comments.push_back({comment.id,
            {center.x - comment.size.x * view_.zoom * 0.5F,
             center.y - comment.size.y * view_.zoom * 0.5F,
             comment.size.x * view_.zoom, comment.size.y * view_.zoom}, comment.text});
    }
    for (const SpriteAnimationGraphGroup& group : groups_) {
        SpriteAnimationGraphRect bounds{};
        bool first = true;
        std::vector<std::size_t> retained;
        for (const std::size_t stateIndex : group.stateIndices) {
            const auto node = std::find_if(result.nodes.begin(), result.nodes.end(),
                [stateIndex](const auto& item) { return item.stateIndex == stateIndex; });
            if (node == result.nodes.end()) continue;
            retained.push_back(stateIndex);
            if (first) { bounds = node->rect; first = false; }
            else {
                const float minimumX = std::min(bounds.x, node->rect.x);
                const float minimumY = std::min(bounds.y, node->rect.y);
                const float maximumX = std::max(bounds.x + bounds.width, node->rect.x + node->rect.width);
                const float maximumY = std::max(bounds.y + bounds.height, node->rect.y + node->rect.height);
                bounds = {minimumX, minimumY, maximumX - minimumX, maximumY - minimumY};
            }
        }
        if (!first) {
            const float padding = group.padding * view_.zoom;
            bounds = {bounds.x - padding, bounds.y - padding,
                      bounds.width + padding * 2.0F, bounds.height + padding * 2.0F};
            result.groups.push_back({group.id, bounds, group.name, std::move(retained)});
        }
    }
    result.breadcrumbs = breadcrumbs_;
    result.minimap.rect = {viewport.x + viewport.width - 168.0F, viewport.y + 12.0F, 156.0F, 104.0F};
    result.minimap.visibleGraphBounds = graph_bounds(result.nodes);
    if (result.minimap.visibleGraphBounds.width > 0.0F && result.minimap.visibleGraphBounds.height > 0.0F) {
        for (const auto& node : result.nodes) {
            const float x = result.minimap.rect.x + (node.rect.x - result.minimap.visibleGraphBounds.x) /
                result.minimap.visibleGraphBounds.width * result.minimap.rect.width;
            const float y = result.minimap.rect.y + (node.rect.y - result.minimap.visibleGraphBounds.y) /
                result.minimap.visibleGraphBounds.height * result.minimap.rect.height;
            const float w = std::max(3.0F, node.rect.width / result.minimap.visibleGraphBounds.width * result.minimap.rect.width);
            const float h = std::max(2.0F, node.rect.height / result.minimap.visibleGraphBounds.height * result.minimap.rect.height);
            result.minimap.nodes.push_back({x, y, w, h});
        }
    }
    if (capture_ == Capture::CreateTransition && transitionSource_) {
        const auto source = std::find_if(result.nodes.begin(), result.nodes.end(),
            [this](const SpriteAnimationGraphNodeFrame& node) {
                return node.stateIndex == *transitionSource_;
            });
        if (source != result.nodes.end()) {
            result.transitionPreview = std::pair<SpriteVec2, SpriteVec2>{
                {source->outputPort.x + source->outputPort.width * 0.5F,
                 source->outputPort.y + source->outputPort.height * 0.5F},
                marqueeCurrent_};
        }
    }
    return result;
}

void SpriteAnimationMachineGraphWorkspace::set_search_query(std::string query) {
    searchQuery_ = std::move(query);
}

bool SpriteAnimationMachineGraphWorkspace::select_search_result(int direction) noexcept {
    if (searchQuery_.empty() || session_.asset().states.empty()) return false;
    std::string needle = searchQuery_;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    std::vector<std::size_t> matches;
    for (std::size_t index = 0U; index < session_.asset().states.size(); ++index) {
        std::string value = session_.asset().states[index].name + " " +
            session_.asset().states[index].clip;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (value.find(needle) != std::string::npos) matches.push_back(index);
    }
    if (matches.empty()) return false;
    std::size_t selected = matches.front();
    if (!selectedStates_.empty()) {
        const std::size_t current = *selectedStates_.begin();
        const auto found = std::find(matches.begin(), matches.end(), current);
        if (found != matches.end()) {
            const std::ptrdiff_t offset = direction >= 0 ? 1 : -1;
            const std::ptrdiff_t at = std::distance(matches.begin(), found);
            const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(matches.size());
            selected = matches[static_cast<std::size_t>((at + offset + count) % count)];
        }
    }
    selectedStates_.clear();
    selectedStates_.insert(selected);
    selectedTransition_.reset();
    (void)session_.select({SpriteAnimationMachineSelectionKind::State, selected});
    return true;
}

void SpriteAnimationMachineGraphWorkspace::set_live_state(
    std::string current, std::string previous) {
    liveCurrentState_ = std::move(current);
    livePreviousState_ = std::move(previous);
}

void SpriteAnimationMachineGraphWorkspace::clear_live_state() noexcept {
    liveCurrentState_.clear();
    livePreviousState_.clear();
    liveTransition_.reset();
}

bool SpriteAnimationMachineGraphWorkspace::inspect_transition(
    std::size_t index, const SpriteAnimationTransition& replacement, std::string* error) {
    if (!session_.update_transition(index, replacement, error)) return false;
    selectedTransition_ = index;
    selectedStates_.clear();
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::set_transition_reroute(
    std::size_t index, std::vector<SpriteVec2> graphPoints, std::string* error) {
    if (index >= session_.asset().transitions.size())
        return fail(error, "animation graph transition index is invalid");
    if (graphPoints.size() > 32U || !std::all_of(graphPoints.begin(), graphPoints.end(), [](SpriteVec2 point) {
            return std::isfinite(point.x) && std::isfinite(point.y);
        })) return fail(error, "animation graph reroute points are invalid");
    transitionReroutes_[index] = std::move(graphPoints);
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::add_comment(
    SpriteVec2 graphPosition, SpriteVec2 size, std::string text, std::uint64_t* id,
    std::string* error) {
    if (!std::isfinite(graphPosition.x) || !std::isfinite(graphPosition.y) ||
        !std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 80.0F || size.y < 40.0F ||
        text.empty() || text.size() > 2048U)
        return fail(error, "animation graph comment is invalid");
    const std::uint64_t created = nextAnnotationId_++;
    comments_.push_back({created, graphPosition, size, std::move(text)});
    if (id) *id = created;
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::add_group(
    std::string name, std::set<std::size_t> states, std::uint64_t* id, std::string* error) {
    if (name.empty() || name.size() > 255U || states.empty() ||
        std::any_of(states.begin(), states.end(), [this](std::size_t index) {
            return index >= session_.asset().states.size();
        })) return fail(error, "animation graph group is invalid");
    const std::uint64_t created = nextAnnotationId_++;
    groups_.push_back({created, std::move(name), std::move(states), 28.0F});
    if (id) *id = created;
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::create_subgraph(
    std::string name, std::set<std::size_t> states, std::string* error) {
    if (name.empty() || name.size() > 255U || states.empty() ||
        std::any_of(states.begin(), states.end(), [this](std::size_t index) {
            return index >= session_.asset().states.size();
        }) || std::any_of(subgraphs_.begin(), subgraphs_.end(), [&name](const auto& item) {
            return item.name == name;
        })) return fail(error, "animation graph subgraph is invalid or duplicated");
    subgraphs_.push_back({std::move(name), std::move(states)});
    return true;
}

void SpriteAnimationMachineGraphWorkspace::enter_subgraph(std::string_view name) noexcept {
    const auto found = std::find_if(subgraphs_.begin(), subgraphs_.end(), [name](const auto& item) {
        return item.name == name;
    });
    if (found != subgraphs_.end()) breadcrumbs_.push_back(found->name);
}

void SpriteAnimationMachineGraphWorkspace::leave_subgraph() noexcept {
    if (breadcrumbs_.size() > 1U) breadcrumbs_.pop_back();
}

std::optional<std::size_t> SpriteAnimationMachineGraphWorkspace::node_at(
    SpriteVec2 point, SpriteAnimationGraphRect viewport, bool outputPortOnly) const {
    const SpriteAnimationGraphFrame layout = frame(viewport);
    for (auto found = layout.nodes.rbegin(); found != layout.nodes.rend(); ++found) {
        if ((outputPortOnly ? found->outputPort : found->rect).contains(point))
            return found->stateIndex;
    }
    return std::nullopt;
}

std::optional<std::size_t> SpriteAnimationMachineGraphWorkspace::transition_at(
    SpriteVec2 point, SpriteAnimationGraphRect viewport) const {
    const SpriteAnimationGraphFrame layout = frame(viewport);
    float bestDistance = 8.0F;
    std::optional<std::size_t> best;
    for (const SpriteAnimationGraphEdgeFrame& edge : layout.edges) {
        const float dx = edge.to.x - edge.from.x;
        const float dy = edge.to.y - edge.from.y;
        const float lengthSquared = dx * dx + dy * dy;
        float t = lengthSquared <= 0.0001F ? 0.0F :
            ((point.x - edge.from.x) * dx + (point.y - edge.from.y) * dy) / lengthSquared;
        t = std::clamp(t, 0.0F, 1.0F);
        const float px = edge.from.x + dx * t;
        const float py = edge.from.y + dy * t;
        const float distance = std::hypot(point.x - px, point.y - py);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = edge.transitionIndex;
        }
    }
    return best;
}

bool SpriteAnimationMachineGraphWorkspace::pointer_down(
    int button, SpriteVec2 point, SpriteAnimationGraphRect viewport,
    bool control, bool shift) {
    if (!viewport.contains(point)) return false;
    pointerDown_ = point;
    marqueeCurrent_ = point;
    capturePan_ = view_.pan;
    if (button == 2) {
        capture_ = Capture::Pan;
        return true;
    }
    if (button != 1) return false;
    if (const auto port = node_at(point, viewport, true)) {
        transitionSource_ = *port;
        capture_ = Capture::CreateTransition;
        return true;
    }
    if (const auto node = node_at(point, viewport)) {
        if (!control && !shift && !selectedStates_.contains(*node)) selectedStates_.clear();
        if (control && selectedStates_.contains(*node)) selectedStates_.erase(*node);
        else selectedStates_.insert(*node);
        selectedTransition_.reset();
        (void)session_.select({SpriteAnimationMachineSelectionKind::State, *node});
        capturePositions_.clear();
        for (std::size_t index : selectedStates_)
            capturePositions_.emplace(index, session_.asset().states[index].graphPosition);
        capture_ = Capture::MoveNodes;
        return true;
    }
    if (const auto transition = transition_at(point, viewport)) {
        selectedStates_.clear();
        selectedTransition_ = transition;
        (void)session_.select({SpriteAnimationMachineSelectionKind::Transition, *transition});
        return true;
    }
    if (!control && !shift) selectedStates_.clear();
    selectedTransition_.reset();
    capture_ = Capture::Marquee;
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::pointer_move(
    SpriteVec2 point, SpriteAnimationGraphRect viewport) {
    if (capture_ == Capture::NoCapture) return false;
    marqueeCurrent_ = point;
    if (capture_ == Capture::Pan) {
        view_.pan = {capturePan_.x + point.x - pointerDown_.x,
                     capturePan_.y + point.y - pointerDown_.y};
    } else if (capture_ == Capture::Marquee) {
        const SpriteAnimationGraphRect selection{
            std::min(pointerDown_.x, point.x), std::min(pointerDown_.y, point.y),
            std::fabs(pointerDown_.x - point.x), std::fabs(pointerDown_.y - point.y)};
        selectedStates_.clear();
        for (const SpriteAnimationGraphNodeFrame& node : frame(viewport).nodes) {
            if (selection.intersects(node.rect)) selectedStates_.insert(node.stateIndex);
        }
    }
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::pointer_up(
    int button, SpriteVec2 point, SpriteAnimationGraphRect viewport, std::string* error) {
    if (capture_ == Capture::NoCapture || (button != 1 && button != 2)) return false;
    const Capture finished = capture_;
    capture_ = Capture::NoCapture;
    if (finished == Capture::MoveNodes) {
        const SpriteVec2 deltaScreen{point.x - pointerDown_.x, point.y - pointerDown_.y};
        const SpriteVec2 deltaGraph{deltaScreen.x / view_.zoom, deltaScreen.y / view_.zoom};
        for (const auto& [index, origin] : capturePositions_) {
            if (!session_.move_state(index, {origin.x + deltaGraph.x, origin.y + deltaGraph.y}, error))
                return false;
        }
        capturePositions_.clear();
    } else if (finished == Capture::CreateTransition && transitionSource_) {
        const auto destination = node_at(point, viewport);
        if (destination && *destination != *transitionSource_) {
            SpriteAnimationTransition transition;
            transition.fromState = session_.asset().states[*transitionSource_].name;
            transition.toState = session_.asset().states[*destination].name;
            transition.blendDurationSeconds = 0.1F;
            if (!session_.add_transition(std::move(transition), error)) return false;
            selectedTransition_ = session_.asset().transitions.size() - 1U;
            selectedStates_.clear();
        }
        transitionSource_.reset();
    }
    normalize_selection();
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::wheel(
    float steps, SpriteVec2 point, SpriteAnimationGraphRect viewport) noexcept {
    if (!viewport.contains(point) || !std::isfinite(steps)) return false;
    const SpriteVec2 before = screen_to_graph(point, viewport);
    view_.zoom = std::clamp(view_.zoom * std::pow(1.12F, steps), 0.25F, 3.0F);
    const SpriteVec2 after = graph_to_screen(before, viewport);
    view_.pan.x += point.x - after.x;
    view_.pan.y += point.y - after.y;
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::copy_selection() noexcept {
    clipboard_ = {};
    if (selectedStates_.empty()) return false;
    std::set<std::string, std::less<>> names;
    for (std::size_t index : selectedStates_) {
        if (index >= session_.asset().states.size()) continue;
        clipboard_.states.push_back(session_.asset().states[index]);
        names.insert(session_.asset().states[index].name);
    }
    for (const SpriteAnimationTransition& transition : session_.asset().transitions) {
        if (transition.fromState != "*" && names.contains(transition.fromState) &&
            names.contains(transition.toState)) clipboard_.transitions.push_back(transition);
    }
    return !clipboard_.states.empty();
}

bool SpriteAnimationMachineGraphWorkspace::paste_selection(std::string* error) {
    if (clipboard_.states.empty()) return fail(error, "animation graph clipboard is empty");
    ++pasteSerial_;
    SpriteAnimationStateMachineAsset replacement = session_.asset();
    std::map<std::string, std::string, std::less<>> names;
    std::set<std::size_t> pastedStates;
    for (const SpriteAnimationState& state : clipboard_.states) {
        std::string name = state.name + "_copy" + std::to_string(pasteSerial_);
        std::uint32_t collision = 1U;
        while (std::any_of(replacement.states.begin(), replacement.states.end(),
                           [&name](const SpriteAnimationState& candidate) {
                               return candidate.name == name;
                           })) {
            name = state.name + "_copy" + std::to_string(pasteSerial_) + "_" +
                std::to_string(collision++);
        }
        SpriteAnimationState copy = state;
        copy.name = name;
        copy.graphPosition.x += 36.0F * static_cast<float>(pasteSerial_);
        copy.graphPosition.y += 36.0F * static_cast<float>(pasteSerial_);
        names.emplace(state.name, name);
        replacement.states.push_back(std::move(copy));
        pastedStates.insert(replacement.states.size() - 1U);
    }
    for (SpriteAnimationTransition transition : clipboard_.transitions) {
        transition.fromState = names.at(transition.fromState);
        transition.toState = names.at(transition.toState);
        replacement.transitions.push_back(std::move(transition));
    }
    const SpriteAnimationMachineSelection selection{
        SpriteAnimationMachineSelectionKind::State, *pastedStates.begin()};
    if (!session_.replace_asset(std::move(replacement), selection, error)) return false;
    selectedStates_ = std::move(pastedStates);
    selectedTransition_.reset();
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::delete_selection(std::string* error) {
    if (selectedTransition_) {
        const std::size_t index = *selectedTransition_;
        selectedTransition_.reset();
        return session_.remove_transition(index, error);
    }
    if (selectedStates_.empty()) return false;
    std::vector<std::size_t> indices(selectedStates_.begin(), selectedStates_.end());
    std::sort(indices.rbegin(), indices.rend());
    for (std::size_t index : indices) {
        if (session_.asset().states.size() <= 1U)
            return fail(error, "animation graph must retain at least one state");
        if (!session_.remove_state(index, error)) return false;
    }
    selectedStates_.clear();
    normalize_selection();
    return true;
}

bool SpriteAnimationMachineGraphWorkspace::key_down(
    std::string_view key, bool control, bool shift, std::string* error) {
    std::string normalized(key);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (control && normalized == "c") return copy_selection();
    if (control && normalized == "v") return paste_selection(error);
    if (control && normalized == "z") {
        const bool changed = shift ? session_.redo(error) : session_.undo(error);
        normalize_selection();
        return changed;
    }
    if (normalized == "delete" || normalized == "backspace")
        return delete_selection(error);
    if (normalized == "f3") return select_search_result(shift ? -1 : 1);
    return false;
}

void SpriteAnimationMachineGraphWorkspace::normalize_selection() noexcept {
    for (auto found = selectedStates_.begin(); found != selectedStates_.end();) {
        if (*found >= session_.asset().states.size()) found = selectedStates_.erase(found);
        else ++found;
    }
    if (selectedTransition_ && *selectedTransition_ >= session_.asset().transitions.size())
        selectedTransition_.reset();
}

} // namespace dve
