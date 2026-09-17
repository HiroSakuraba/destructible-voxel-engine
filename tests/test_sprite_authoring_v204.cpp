#include "dve/editor_sprite_authoring.hpp"

#include <png.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
[[noreturn]] void fail_test(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
}
void require(bool condition, const std::string& message) { if (!condition) fail_test(message); }

using namespace dve;
using namespace dve::editor;

std::vector<std::byte> make_png() {
    constexpr std::uint32_t width = 8U;
    constexpr std::uint32_t height = 4U;
    std::vector<std::byte> rgba(width * height * 4U, std::byte{0});
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[offset] = std::byte{static_cast<unsigned char>(x < 4U ? 255U : 0U)};
            rgba[offset + 1U] = std::byte{static_cast<unsigned char>(x >= 4U ? 255U : 0U)};
            rgba[offset + 2U] = std::byte{0};
            rgba[offset + 3U] = std::byte{255U};
        }
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    require(png_image_write_to_memory(&image, nullptr, &size, 0, rgba.data(), 0, nullptr) != 0, "png_image_write_to_memory(&image, nullptr, &size, 0, rgba.data(), 0, nullptr) != 0");
    std::vector<std::byte> encoded(static_cast<std::size_t>(size));
    require(png_image_write_to_memory(&image, encoded.data(), &size, 0, rgba.data(), 0, nullptr) != 0, "png_image_write_to_memory(&image, encoded.data(), &size, 0, rgba.data(), 0, nullptr) != 0");
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}

void test_decoder_workspace_and_timeline() {
    const std::vector<std::byte> encoded = make_png();
    SpriteDecodedImage decoded;
    std::string error;
    require(decode_sprite_image(encoded, "image/png", decoded, &error), "decode_sprite_image(encoded, 'image/png', decoded, &error)");
    require(decoded.width == 8U && decoded.height == 4U, "decoded.width == 8U && decoded.height == 4U");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "dve_v204_sprite.png";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    SpriteAuthoringWorkspace workspace;
    require(workspace.create_from_texture(path, "hero", path.generic_string(), 16.0F, &error), "workspace.create_from_texture(path, 'hero', 'textures/hero.png', 16.0F, &error)");
    SpriteGridSliceSettings slice;
    slice.cellWidth = 4U;
    slice.cellHeight = 4U;
    slice.columns = 2U;
    slice.rows = 1U;
    slice.trimTransparent = false;
    require(workspace.slice_grid(slice, &error), "workspace.slice_grid(slice, &error)");
    require(workspace.session().asset().frames.size() == 2U, "workspace.session().asset().frames.size() == 2U");
    require(workspace.select_timeline_frames(std::array<std::size_t, 2>{0U, 1U}, 1U), "workspace.select_timeline_frames(std::array<std::size_t, 2>{0U, 1U}, 1U)");
    require(workspace.set_selected_frame_duration(0.125F, &error), "workspace.set_selected_frame_duration(0.125F, &error)");
    require(workspace.set_selected_frame_event("step", &error), "workspace.set_selected_frame_event('step', &error)");
    require(workspace.set_selected_frame_pivot({2.0F, 1.0F}, &error), "workspace.set_selected_frame_pivot({2.0F, 1.0F}, &error)");
    require(workspace.set_clip_playback(SpriteLoopMode::PingPong, 1.5F, &error), "workspace.set_clip_playback(SpriteLoopMode::PingPong, 1.5F, &error)");
    const std::array<std::size_t, 1> moveSelection{1U};
    require(workspace.select_timeline_frames(moveSelection, 1U), "select one frame for reorder");
    require(workspace.reorder_timeline_selection(0U, &error), "reorder selected timeline frame: " + error);

    const float oldZoom = workspace.canvas().zoom;
    workspace.zoom_at(2.0F, {20.0F, 10.0F});
    workspace.pan_by({3.0F, -2.0F});
    require(workspace.canvas().zoom > oldZoom, "workspace.canvas().zoom > oldZoom");
    workspace.canvas().onionSkinPrevious = true;
    workspace.canvas().onionSkinNext = true;

    const SpriteTimelineState before = workspace.timeline();
    require(workspace.reimport_source(true, &error), "reimport source before atlas packing: " + error);
    require(workspace.timeline().clip == before.clip, "reimport preserves selected clip");
    require(workspace.timeline().primarySequenceIndex == before.primarySequenceIndex,
            "reimport preserves primary frame");

    const std::filesystem::path assetPath = std::filesystem::temp_directory_path() / "dve_v204_sprite.dvesprite";
    require(workspace.session().save(assetPath, &error), "save sprite asset: " + error);
    SpriteAuthoringWorkspace reopened;
    require(reopened.open_asset(assetPath, path.parent_path(), &error), "open saved sprite asset: " + error);
    require(reopened.session().asset().frames.size() == 2U, "open asset restores frames");

    SpriteAtlasPackSettings packing;
    packing.maximumWidth = 16U;
    packing.paddingPixels = 1U;
    packing.powerOfTwo = true;
    SpriteAtlasPackResult packed;
    require(workspace.repack_atlas(packing, packed, &error), "repack atlas: " + error);
    require((packed.width & (packed.width - 1U)) == 0U, "packed width is power of two");
    require((packed.height & (packed.height - 1U)) == 0U, "packed height is power of two");
    require(packed.frameRects.size() == 2U, "packed frame count");
    require(workspace.session().asset().textureWidth == packed.width, "asset atlas dimensions update");
    const SpriteTimelineState packedSelection = workspace.timeline();
    require(workspace.reimport_source(true, &error), "reimport source after atlas packing: " + error);
    require(workspace.session().asset().textureWidth == packed.width &&
            workspace.session().asset().textureHeight == packed.height,
            "packed layout is rebuilt after source reimport");
    require(workspace.timeline().primarySequenceIndex == packedSelection.primarySequenceIndex,
            "packed reimport preserves timeline selection");
    require(workspace.source_image().sourcePath == path,
            "packed preview retains the original reimport path");
    const std::filesystem::path packedAssetPath =
        std::filesystem::temp_directory_path() / "dve_v204_sprite_packed_preview.dvesprite";
    require(workspace.save_asset(packedAssetPath, &error),
            "save authoring asset while packed preview is active: " + error);
    SpriteAuthoringWorkspace packedReopened;
    require(packedReopened.open_asset(packedAssetPath, path.parent_path(), &error),
            "reopen asset saved from packed preview: " + error);
    require(packedReopened.session().asset().textureWidth == 8U &&
            packedReopened.session().asset().textureHeight == 4U,
            "save restores source-space dimensions instead of publishing a broken packed reference");

    EditorSpriteAuthoringPanel panel;
    panel.resize(1000, 700);
    require(panel.open_texture(path, assetPath, &error), "panel opens texture: " + error);
    require(panel.open(), "panel is open");
    SpriteAuthoringPanelFrame panelFrame = panel.frame();
    require(panelFrame.canvas.width > 0 && panelFrame.timeline.height > 0, "panel layout is usable");
    require(panel.pointer_down(1, panelFrame.gridSliceButton.x + 2, panelFrame.gridSliceButton.y + 2),
            "panel handles grid slice button");
    require(panel.workspace().session().asset().frames.size() >= 1U, "panel grid slice produces frames");
    const float panelZoom = panel.workspace().canvas().zoom;
    require(panel.pointer_wheel(1.0F, panelFrame.canvas.x + 20, panelFrame.canvas.y + 20),
            "panel handles canvas zoom");
    require(panel.workspace().canvas().zoom > panelZoom, "panel wheel changes zoom");

    SpriteGridSliceSettings manyFrames;
    manyFrames.cellWidth = 1U;
    manyFrames.cellHeight = 4U;
    manyFrames.columns = 8U;
    manyFrames.rows = 1U;
    manyFrames.trimTransparent = false;
    require(panel.workspace().slice_grid(manyFrames, &error), "panel creates a virtualized timeline: " + error);
    panelFrame = panel.frame();
    require(panelFrame.timelineFrames.size() < panel.workspace().session().asset().frames.size(),
            "timeline test requires more frames than fit in the panel");
    require(panel.pointer_wheel(-1.0F, panelFrame.timeline.x + 20, panelFrame.timeline.y + 40),
            "panel handles timeline scrolling");
    require(panel.workspace().timeline().firstVisibleSequenceIndex > 0U,
            "timeline wheel did not virtualize past the first frame");

    panelFrame = panel.frame();
    require(panel.pointer_down(1, panelFrame.paletteTabButton.x + 2,
                               panelFrame.paletteTabButton.y + 2),
            "panel creates a linked palette from source art");
    require(panel.has_palette_document() && panel.palette_inspector_mode(),
            "palette inspector did not open");
    require(!panel.workspace().session().asset().paletteAsset.empty(),
            "created palette was not linked to the sprite asset");
    panelFrame = panel.frame();
    require(panelFrame.paletteMode && !panelFrame.paletteSwatches.empty(),
            "palette inspector did not expose native swatches");
    require(panel.pointer_down(1, panelFrame.paletteAddBankButton.x + 2,
                               panelFrame.paletteAddBankButton.y + 2),
            "palette inspector handles bank creation");
    require(panel.palette_session().asset().banks.size() == 2U,
            "palette bank was not created");
    panelFrame = panel.frame();
    require(panel.pointer_down(1, panelFrame.paletteAddCycleButton.x + 2,
                               panelFrame.paletteAddCycleButton.y + 2),
            "palette inspector handles cycle creation");
    require(panel.palette_session().asset().cycles.size() == 1U,
            "palette cycle was not created");
    panel.update(1.0F / 60.0F);
    require(panel.palette_session().preview_ticks() == 4U,
            "palette preview did not advance on the 240 Hz clock");

    panelFrame = panel.frame();
    require(panel.pointer_down(1, panelFrame.tracksTabButton.x + 2,
                               panelFrame.tracksTabButton.y + 2),
            "panel opens the authored-track inspector");
    require(panel.tracks_inspector_mode(), "track inspector did not open");
    panelFrame = panel.frame();
    require(panelFrame.tracksMode, "track inspector layout is inactive");
    require(panel.pointer_down(1, panelFrame.trackAddHitboxButton.x + 2,
                               panelFrame.trackAddHitboxButton.y + 2),
            "track inspector handles hitbox creation");
    require(panel.pointer_down(1, panelFrame.trackAddSocketButton.x + 2,
                               panelFrame.trackAddSocketButton.y + 2),
            "track inspector handles socket creation");
    require(panel.pointer_down(1, panelFrame.trackAddPropertyButton.x + 2,
                               panelFrame.trackAddPropertyButton.y + 2),
            "track inspector handles property creation");
    require(panel.pointer_down(1, panelFrame.trackSetRootMotionButton.x + 2,
                               panelFrame.trackSetRootMotionButton.y + 2),
            "track inspector handles root-motion creation");
    const SpriteClip* authoredClip = find_sprite_clip(
        panel.workspace().session().asset(), panel.workspace().timeline().clip);
    require(authoredClip != nullptr, "track inspector clip disappeared");
    require(authoredClip->combatWindows.size() == 1U, "hitbox was not authored");
    require(authoredClip->socketKeys.size() == 1U, "socket key was not authored");
    require(authoredClip->propertyKeys.size() == 1U, "property key was not authored");
    require(authoredClip->rootMotionKeys.size() == 1U, "root-motion key was not authored");
    panelFrame = panel.frame();
    require(!panelFrame.timelineCombatMarkers.empty() &&
            !panelFrame.timelineSocketMarkers.empty() &&
            !panelFrame.timelinePropertyMarkers.empty() &&
            !panelFrame.timelineRootMarkers.empty(),
            "timeline did not expose authored-track markers");

    require(panel.save(&error), "panel saves sprite and palette together: " + error);
    const std::filesystem::path palettePath = assetPath.parent_path() /
        panel.workspace().session().asset().paletteAsset;
    require(std::filesystem::exists(palettePath), "linked palette file was not saved");

    require(panel.key_down("space", false, false, false), "panel handles playback key");
    require(!panel.workspace().timeline().playing, "panel playback toggles");

    std::filesystem::remove(packedAssetPath);
    const std::filesystem::path createdPalette = assetPath.parent_path() / "dve_v204_sprite.dvepalette";
    std::filesystem::remove(createdPalette);
    std::filesystem::remove(assetPath);
    std::filesystem::remove(path);
}
} // namespace

int main() {
    test_decoder_workspace_and_timeline();
    return 0;
}
