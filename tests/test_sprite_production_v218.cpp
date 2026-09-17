#include "dve/sprite_animation_graph_renderer.hpp"
#include "dve/sprite_production_tools.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_track_production_tools() {
    const auto read = read_dvesprite(std::filesystem::path(DVE_SOURCE_DIR) /
                                     "assets/sprites/v218_sun_route_cast.dvesprite");
    require(static_cast<bool>(read), read.error);
    SpriteAsset asset = read.asset;
    SpriteClip* run = nullptr;
    for (auto& clip : asset.clips) if (clip.name == "player_run") run = &clip;
    require(run != nullptr, "player_run clip is missing");
    run->frames = {1U, 2U, 1U, 2U, 1U, 2U};
    asset.frames[1U].event = "footstep_left";
    asset.frames[2U].event = "footstep_right";
    asset.recompute_hash();

    SpriteAuthoringSession session(asset);
    require(session.select_clip("player_run"), "could not select run clip");
    SpriteTrackProductionEditor tools(session);
    std::string error;
    SpriteTrackClipboard clipboard;
    require(tools.copy_range("player_run", 0U, 1U, clipboard, &error), error);
    const std::size_t windowsBefore = find_sprite_clip(session.asset(), "player_run")->combatWindows.size();
    const std::string eventBefore = session.asset().frames[1U].event;
    require(tools.paste_range("player_run", 2U, clipboard, SpriteTrackPasteMode::Merge, &error), error);
    const SpriteClip* pasted = find_sprite_clip(session.asset(), "player_run");
    require(pasted != nullptr && pasted->combatWindows.size() > windowsBefore,
            "range paste omitted combat tracks");
    require(session.asset().frames[pasted->frames[2U]].event == "footstep_left",
            "range paste omitted frame events");
    require(session.undo(), "atomic range paste did not record undo");
    const SpriteClip* undone = find_sprite_clip(session.asset(), "player_run");
    require(undone != nullptr && undone->combatWindows.size() == windowsBefore,
            "one undo did not restore pasted tracks");
    require(session.asset().frames[1U].event == eventBefore,
            "one undo did not restore frame events");

    require(tools.apply_preset("player_run", 3U, SpriteTrackPresetKind::MeleeHitbox, &error), error);
    const SpriteClip* withPreset = find_sprite_clip(session.asset(), "player_run");
    require(withPreset != nullptr && !withPreset->combatWindows.empty(), "preset did not add a combat window");
    const SpriteTrackId hitId = withPreset->combatWindows.back().id;
    require(session.select_track(SpriteTrackItemKind::CombatWindow, hitId), "could not select preset track");
    require(tools.drag_item("player_run", *session.document().selectedTrack,
                            SpriteTrackHandleKind::Move, {3.0F, -2.0F}, &error), error);
    require(tools.adjust_numeric("player_run", *session.document().selectedTrack,
                                 SpriteTrackNumericField::Damage, 2.0F, &error), error);
    require(!tools.numeric_descriptors("player_run", *session.document().selectedTrack).empty(),
            "numeric inspector descriptors are empty");
    require(!tools.search("footstep", "player_run").empty(), "event search found no rows");
    require(!tools.onion_overlays("player_run", 1U, true, true).empty(),
            "onion-skin metadata overlays are empty");
    require(tools.bulk_retime("player_run", 0U, 1U, 2U, 3U, &error), error);
    require(tools.duplicate_range("player_run", 2U, 3U, 2, &error), error);
    require(tools.mirror_range_x("player_run", 0U, 5U, &error), error);
    const auto diagnostics = tools.diagnostics("player_run");
    for (const auto& diagnostic : diagnostics)
        require(!diagnostic.field.empty() && !diagnostic.clip.empty(),
                "diagnostic is not tied to an exact field and clip");
}

void test_graph_production_tools() {
    const auto spriteRead = read_dvesprite(std::filesystem::path(DVE_SOURCE_DIR) /
                                            "assets/sprites/v218_sun_route_cast.dvesprite");
    require(static_cast<bool>(spriteRead), spriteRead.error);
    const auto machinePath = std::filesystem::path(DVE_SOURCE_DIR) /
                             "assets/sprites/v218_sun_route_player.dvesprite_machine";
    SpriteAnimationMachineGraphWorkspace graph;
    std::string error;
    require(graph.open(machinePath, spriteRead.asset, &error), error);
    require(graph.add_comment({48.0F, 40.0F}, {210.0F, 80.0F}, "Locomotion", nullptr, &error), error);
    require(graph.add_group("Grounded", {0U, 1U}, nullptr, &error), error);
    require(graph.create_subgraph("Locomotion", {0U, 1U}, &error), error);
    graph.enter_subgraph("Locomotion");
    require(graph.frame({0.0F, 0.0F, 900.0F, 600.0F}).breadcrumbs.size() == 2U,
            "subgraph breadcrumb was not exposed");
    graph.leave_subgraph();
    require(graph.set_transition_reroute(0U, {{250.0F, 50.0F}, {270.0F, 170.0F}}, &error), error);
    SpriteAnimationTransition edited = graph.session().asset().transitions[0U];
    edited.priority = 42;
    edited.blendCurve = SpriteAnimationBlendCurve::EaseOut;
    edited.trackPolicy = SpriteAnimationBlendTrackPolicy::BothWeighted;
    require(graph.inspect_transition(0U, edited, &error), error);
    graph.set_live_state("Run", "Idle");
    graph.set_live_transition(0U);

    const SpriteAnimationGraphRect viewport{0.0F, 0.0F, 900.0F, 600.0F};
    const auto frame = graph.frame(viewport);
    require(frame.nodes.size() == 3U && frame.edges.size() == 4U,
            "graph frame omitted nodes or transitions");
    require(frame.comments.size() == 1U && frame.groups.size() == 1U,
            "graph annotations were not rendered");
    require(frame.edges[0U].live && !frame.edges[0U].reroutePoints.empty(),
            "runtime transition trace or reroute points are missing");
    const auto draw = build_sprite_animation_graph_draw_list(frame);
    require(draw.nodeCount == frame.nodes.size() && draw.transitionCount == frame.edges.size(),
            "graph draw list counts are incorrect");
    const auto image = rasterize_sprite_animation_graph_reference(draw, 900U, 600U);
    require(image.contentHash != 0U && !image.rgba8.empty(), "graph reference raster is empty");

    // Select two nodes through the same hit-testing path used by the native graph canvas.
    const SpriteVec2 first{frame.nodes[0U].rect.x + 20.0F, frame.nodes[0U].rect.y + 20.0F};
    const SpriteVec2 second{frame.nodes[1U].rect.x + 20.0F, frame.nodes[1U].rect.y + 20.0F};
    require(graph.pointer_down(1, first, viewport), "could not select first graph node");
    require(graph.pointer_up(1, first, viewport, &error), error);
    require(graph.pointer_down(1, second, viewport, true, false), "could not add second graph node");
    require(graph.pointer_up(1, second, viewport, &error), error);
    require(graph.key_down("c", true, false, &error), error);
    const std::size_t beforePaste = graph.session().asset().states.size();
    require(graph.key_down("v", true, false, &error), error);
    require(graph.session().asset().states.size() == beforePaste + 2U,
            "graph paste did not duplicate the selected states");
    require(graph.session().undo(&error), error);
    require(graph.session().asset().states.size() == beforePaste,
            "graph paste was not one atomic undo transaction");
}

} // namespace

int main() {
    try {
        test_track_production_tools();
        test_graph_production_tools();
        std::cout << "v2.18 sprite production tools passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
