#include "dve/editor_sprite_authoring.hpp"

#include <png.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float left, float right, float epsilon = 0.0002F) {
    return std::abs(left - right) <= epsilon;
}

SpriteAsset make_asset(SpriteLoopMode loopMode = SpriteLoopMode::Loop) {
    SpriteAsset asset;
    asset.name = "v209 hero";
    asset.textureAsset = "textures/v209_hero.png";
    asset.textureWidth = 48U;
    asset.textureHeight = 16U;
    asset.pixelsPerWorldUnit = 16.0F;
    asset.frames = {
        {"idle", {0U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, ""},
        {"active", {16U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, "active"},
        {"recover", {32U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, ""},
    };
    SpriteClip clip{"attack", loopMode, 1.0F, {0U, 1U, 2U}};

    SpriteCombatWindow combat;
    combat.firstSequenceIndex = 0U;
    combat.lastSequenceIndex = 1U;
    combat.volume.name = "blade";
    combat.volume.role = SpriteCombatRole::Hitbox;
    combat.volume.shape = SpriteCombatShape::Box;
    combat.volume.centerPixels = {4.0F, 6.0F};
    combat.volume.sizePixels = {8.0F, 6.0F};
    combat.volume.radiusPixels = 2.0F;
    combat.volume.attackId = 11U;
    combat.volume.damage = 5.0F;
    clip.combatWindows.push_back(combat);

    clip.socketKeys.push_back({"weapon", 0U, {2.0F, 4.0F}, 0.0F, {1.0F, 1.0F},
                               SpriteTrackInterpolation::Linear});
    clip.socketKeys.push_back({"weapon", 2U, {10.0F, 6.0F}, 45.0F, {1.0F, 1.0F},
                               SpriteTrackInterpolation::Step});

    SpritePropertyValue aim;
    aim.type = SpritePropertyType::Vec2;
    aim.vec2Value = {1.0F, 2.0F};
    clip.propertyKeys.push_back({"aim", 1U, SpriteTrackInterpolation::Step, aim});

    clip.rootMotionKeys.push_back({0U, {1.0F, 0.0F}, 1.0F});
    clip.rootMotionKeys.push_back({1U, {3.0F, 1.0F}, 2.0F});
    clip.rootMotionKeys.push_back({2U, {2.0F, 0.0F}, 3.0F});
    asset.clips.push_back(std::move(clip));
    asset.recompute_hash();
    return asset;
}

std::vector<std::byte> make_png() {
    constexpr std::uint32_t width = 8U;
    constexpr std::uint32_t height = 4U;
    std::vector<std::byte> rgba(width * height * 4U, std::byte{0});
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[offset] = std::byte{static_cast<unsigned char>(x < 4U ? 220U : 40U)};
            rgba[offset + 1U] = std::byte{static_cast<unsigned char>(x < 4U ? 40U : 220U)};
            rgba[offset + 2U] = std::byte{80U};
            rgba[offset + 3U] = std::byte{255U};
        }
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0U;
    require(png_image_write_to_memory(&image, nullptr, &size, 0, rgba.data(), 0, nullptr) != 0,
            "could not size v2.09 test PNG");
    std::vector<std::byte> encoded(static_cast<std::size_t>(size));
    require(png_image_write_to_memory(&image, encoded.data(), &size, 0, rgba.data(), 0, nullptr) != 0,
            "could not encode v2.09 test PNG");
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}

void test_stable_track_ids_and_codec() {
    SpriteAsset asset = make_asset();
    const SpriteClip& clip = asset.clips.front();
    std::vector<SpriteTrackId> ids;
    ids.push_back(clip.combatWindows.front().id);
    for (const SpriteSocketKey& key : clip.socketKeys) ids.push_back(key.id);
    for (const SpritePropertyKey& key : clip.propertyKeys) ids.push_back(key.id);
    for (const SpriteRootMotionKey& key : clip.rootMotionKeys) ids.push_back(key.id);
    require(std::all_of(ids.begin(), ids.end(), [](SpriteTrackId id) {
        return id != kInvalidSpriteTrackId;
    }), "track IDs were not assigned");
    std::sort(ids.begin(), ids.end());
    require(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), "track IDs are not unique");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_v209_stable_tracks.dvesprite";
    std::string error;
    require(write_dvesprite(path, asset, &error), error);
    {
        std::ifstream input(path);
        std::string magic;
        unsigned version{};
        require(static_cast<bool>(input >> magic >> version) && magic == "DVE_SPRITE" && version == 4U,
                "v2.09 did not publish DVE_SPRITE 4");
    }
    const SpriteAssetReadResult read = read_dvesprite(path);
    require(static_cast<bool>(read), read.error);
    require(read.asset.contentHash == asset.contentHash,
            "v2.09 track IDs changed the asset across a codec round trip");
    require(read.asset.clips.front().combatWindows.front().id ==
            asset.clips.front().combatWindows.front().id,
            "combat track ID was not persisted");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_precision_authoring_and_reorder_identity() {
    SpriteAuthoringSession session(make_asset());
    const SpriteTrackId combatId = session.asset().clips.front().combatWindows.front().id;
    std::string error;
    require(session.select_track(SpriteTrackItemKind::CombatWindow, combatId),
            "could not select combat track by stable ID");
    require(session.translate_combat_window("attack", combatId, {2.0F, -1.0F}, &error), "translate: " + error);
    require(session.resize_combat_window("attack", combatId, {12.0F, 8.0F}, 2.0F, &error), "resize: " + error);
    require(session.rotate_combat_window("attack", combatId, 30.0F, &error), "rotate: " + error);
    require(session.set_combat_window_range("attack", combatId, 1U, 1U, &error), "range: " + error);
    const SpriteCombatWindow& edited = session.asset().clips.front().combatWindows.front();
    require(edited.id == combatId && close(edited.volume.centerPixels.x, 6.0F) &&
            close(edited.volume.centerPixels.y, 5.0F) && close(edited.volume.rotationDegrees, 30.0F),
            "precision combat editing lost identity or values");

    SpriteTrackId duplicateId{};
    require(session.duplicate_track_item("attack", SpriteTrackItemKind::CombatWindow,
                                         combatId, 1, &duplicateId, &error), error);
    require(duplicateId != combatId && duplicateId != kInvalidSpriteTrackId,
            "track duplication did not allocate a new stable ID");
    require(session.set_combat_window_range("attack", combatId, 0U, 1U, &error),
            "restore split range: " + error);
    require(session.mirror_track_item_x("attack", SpriteTrackItemKind::CombatWindow,
                                        duplicateId, &error), error);
    const auto duplicate = std::find_if(session.asset().clips.front().combatWindows.begin(),
                                        session.asset().clips.front().combatWindows.end(),
        [duplicateId](const SpriteCombatWindow& item) { return item.id == duplicateId; });
    require(duplicate != session.asset().clips.front().combatWindows.end() &&
            close(duplicate->volume.centerPixels.x, -6.0F),
            "mirrored duplicate did not retain its ID or transform");

    require(session.move_clip_frame("attack", 0U, 2U, &error), "move: " + error);
    const SpriteClip& reordered = session.asset().clips.front();
    const auto originalSegment = std::find_if(reordered.combatWindows.begin(), reordered.combatWindows.end(),
        [combatId](const SpriteCombatWindow& item) { return item.id == combatId; });
    require(originalSegment != reordered.combatWindows.end(),
            "timeline reorder discarded the original stable combat ID");
    std::vector<SpriteTrackId> ids;
    for (const SpriteCombatWindow& item : reordered.combatWindows) ids.push_back(item.id);
    std::sort(ids.begin(), ids.end());
    require(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
            "split combat windows reused one stable ID");
    require(session.remove_track_by_id("attack", SpriteTrackItemKind::CombatWindow,
                                       duplicateId, &error), error);
}

void test_root_motion_attachments_and_debug_packets() {
    const SpriteAsset asset = make_asset(SpriteLoopMode::Loop);
    const auto interval = accumulate_sprite_root_motion(
        asset, "attack", 0.0F, 0.35F, GameplayPlane2D::XY, false, false);
    require(interval && interval->enteredFrames == 3U && interval->crossedLoopBoundary,
            "root-motion interval did not report skipped frames and loop crossing");
    require(close(interval->deltaPixels.x, 6.0F) && close(interval->deltaPixels.y, 1.0F) &&
            close(interval->rotationDegrees, 6.0F),
            "root-motion interval accumulated the wrong authored keys");

    SpriteRuntime runtime;
    std::string error;
    require(runtime.register_asset(1U, asset, &error), error);
    SpriteInstanceDesc desc;
    desc.asset = 1U;
    desc.clip = "attack";
    desc.transform = make_rigid_transform(
        {10.0F, 20.0F, 0.0F}, quaternion_from_axis_angle({0.0F, 0.0F, 1.0F},
                                                          1.57079632679489661923F));
    require(runtime.bind(7U, desc, &error), error);
    SpriteSocketAttachmentDesc attachment;
    attachment.id = 91U;
    attachment.parent = 7U;
    attachment.socket = "weapon";
    attachment.localOffset = make_rigid_transform({1.0F, 0.0F, 0.0F}, {});
    require(runtime.bind_socket_attachment(attachment, &error), error);

    runtime.tick(0.35F);
    const auto consumed = runtime.consume_root_motion(7U);
    require(consumed && close(consumed->deltaPixels.x, 6.0F) &&
            close(consumed->worldTranslation.x, -1.0F / 16.0F) &&
            close(consumed->worldTranslation.y, 6.0F / 16.0F),
            "runtime root motion was not transformed into actor world orientation");
    const auto empty = runtime.consume_root_motion(7U);
    require(empty && empty->enteredFrames == 0U && close(empty->deltaPixels.x, 0.0F),
            "root-motion consumption was not destructive");

    require(runtime.seek(7U, 0.15F), "could not seek attached sprite");
    const std::vector<SpriteSocketAttachmentSample> attachments = runtime.sample_socket_attachments();
    require(attachments.size() == 1U && attachments.front().id == 91U && attachments.front().visible,
            "socket attachment was not resolved");
    const auto tracks = runtime.sample_tracks(7U);
    require(tracks && !tracks->sockets.empty(), "socket track was not sampled");
    const RigidTransform expected = compose_rigid_transforms(
        tracks->sockets.front().worldTransform, attachment.localOffset);
    require(close(attachments.front().worldTransform.position.x, expected.position.x) &&
            close(attachments.front().worldTransform.position.y, expected.position.y),
            "socket attachment local offset was composed incorrectly");

    const std::vector<SpriteGameplayDebugPacket> packets = runtime.build_gameplay_debug_packets();
    require(packets.size() == 1U && packets.front().owner == 7U &&
            !packets.front().combatVolumes.empty() && !packets.front().sockets.empty() &&
            packets.front().combatVolumes.front().id != kInvalidSpriteTrackId &&
            packets.front().sockets.front().id != kInvalidSpriteTrackId,
            "renderer-neutral gameplay debug packet omitted stable track identities");
}

void test_native_track_rows_and_precision_keys() {
    const std::vector<std::byte> encoded = make_png();
    const std::filesystem::path imagePath =
        std::filesystem::temp_directory_path() / "dve_v209_track_panel.png";
    const std::filesystem::path assetPath =
        std::filesystem::temp_directory_path() / "dve_v209_track_panel.dvesprite";
    {
        std::ofstream output(imagePath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
    }
    EditorSpriteAuthoringPanel panel;
    panel.resize(1100, 760);
    std::string error;
    require(panel.open_texture(imagePath, assetPath, &error), error);
    SpriteGridSliceSettings slicing;
    slicing.cellWidth = 4U;
    slicing.cellHeight = 4U;
    slicing.columns = 2U;
    slicing.rows = 1U;
    slicing.trimTransparent = false;
    require(panel.workspace().slice_grid(slicing, &error), error);
    require(panel.workspace().select_timeline_frames(std::array<std::size_t, 1>{0U}, 0U),
            "could not select first timeline frame");

    SpriteAuthoringPanelFrame layout = panel.frame();
    require(panel.pointer_down(1, layout.tracksTabButton.x + 2, layout.tracksTabButton.y + 2),
            "could not open track inspector");
    layout = panel.frame();
    require(panel.pointer_down(1, layout.trackAddHitboxButton.x + 2,
                               layout.trackAddHitboxButton.y + 2),
            "could not create native hitbox row");
    layout = panel.frame();
    require(layout.trackRows.size() == 1U && layout.trackRowIds.size() == 1U,
            "native track inspector did not expose a stable selectable row");
    require(panel.pointer_down(1, layout.trackRows.front().x + 2,
                               layout.trackRows.front().y + 2),
            "could not select native track row");
    const SpriteTrackId selectedId = layout.trackRowIds.front();
    require(panel.workspace().session().document().selectedTrack &&
            panel.workspace().session().document().selectedTrack->id == selectedId,
            "native row selection did not use the stable track ID");

    require(panel.key_down("right", false, false, false), "right-arrow precision edit not handled");
    require(panel.key_down("right", false, true, false), "Shift resize precision edit not handled");
    require(panel.key_down("right", false, false, true), "Alt rotate precision edit not handled");
    const SpriteClip* clip = find_sprite_clip(panel.workspace().session().asset(),
                                              panel.workspace().timeline().clip);
    require(clip != nullptr, "native precision clip disappeared");
    const auto edited = std::find_if(clip->combatWindows.begin(), clip->combatWindows.end(),
        [selectedId](const SpriteCombatWindow& item) { return item.id == selectedId; });
    require(edited != clip->combatWindows.end() && close(edited->volume.centerPixels.x, 1.0F) &&
            close(edited->volume.sizePixels.x, 13.0F) &&
            close(edited->volume.rotationDegrees, 1.0F),
            "native precision keys did not edit the selected stable row");

    layout = panel.frame();
    require(panel.pointer_down(1, layout.trackDuplicateButton.x + 2,
                               layout.trackDuplicateButton.y + 2),
            "native duplicate button was not handled");
    clip = find_sprite_clip(panel.workspace().session().asset(), panel.workspace().timeline().clip);
    require(clip != nullptr && clip->combatWindows.size() == 2U,
            "native duplicate command did not create a second combat window");
    const auto duplicateSelection = panel.workspace().session().document().selectedTrack;
    require(duplicateSelection && duplicateSelection->id != selectedId,
            "native duplicate command did not select a new stable ID");
    layout = panel.frame();
    require(panel.pointer_down(1, layout.trackMirrorButton.x + 2,
                               layout.trackMirrorButton.y + 2),
            "native mirror button was not handled");
    require(panel.key_down("delete", false, false, false),
            "native selected-track delete was not handled");
    clip = find_sprite_clip(panel.workspace().session().asset(), panel.workspace().timeline().clip);
    require(clip != nullptr && clip->combatWindows.size() == 1U &&
            !panel.workspace().session().document().selectedTrack,
            "native selected-track delete removed the wrong item or retained stale selection");

    std::error_code ec;
    std::filesystem::remove(assetPath, ec);
    std::filesystem::remove(imagePath, ec);
}

} // namespace

int main() {
    try {
        test_stable_track_ids_and_codec();
        test_precision_authoring_and_reorder_identity();
        test_root_motion_attachments_and_debug_packets();
        test_native_track_rows_and_precision_keys();
        std::cout << "dve_v209_sprite_precision_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v209_sprite_precision_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
