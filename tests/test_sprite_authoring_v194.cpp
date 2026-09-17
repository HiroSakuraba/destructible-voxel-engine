#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/render/sprite_renderer.hpp"
#include "dve/rhi/null_device.hpp"
#include "dve/sprite_authoring.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::vector<std::byte> pixels = std::vector<std::byte>(16U * 8U * 4U);
    editor::SpriteSourceImageView image{16U, 8U, pixels};
    editor::SpriteAuthoringSession session{
        editor::make_sprite_authoring_asset(
            "Original Runner", "textures/original_runner.png", 16U, 8U, 8.0F)};

    Fixture() {
        const auto opaque = [&](std::uint32_t x, std::uint32_t y) {
            const std::size_t index = (static_cast<std::size_t>(y) * 16U + x) * 4U;
            pixels[index + 0U] = std::byte{80};
            pixels[index + 1U] = std::byte{160};
            pixels[index + 2U] = std::byte{220};
            pixels[index + 3U] = std::byte{255};
        };
        for (std::uint32_t y = 1U; y <= 6U; ++y)
            for (std::uint32_t x = 2U; x <= 5U; ++x) opaque(x, y);
        for (std::uint32_t y = 2U; y <= 5U; ++y)
            for (std::uint32_t x = 9U; x <= 14U; ++x) opaque(x, y);
    }
};

void test_grid_trim_and_atomic_history() {
    Fixture fixture;
    editor::SpriteGridSliceSettings settings;
    settings.cellWidth = 8U;
    settings.cellHeight = 8U;
    settings.columns = 2U;
    settings.rows = 1U;
    settings.frameNamePrefix = "run";
    settings.pivotPixels = {4.0F, 0.0F};
    settings.durationSeconds = 0.1F;
    std::string error;
    require(fixture.session.slice_grid(fixture.image, settings, &error), error);
    const SpriteAsset& sliced = fixture.session.asset();
    require(sliced.frames.size() == 2U && sliced.clips.size() == 1U &&
            sliced.clips[0].frames == std::vector<SpriteFrameIndex>({0U, 1U}),
            "grid slicing did not rebuild the default timeline");
    require(sliced.frames[0].atlasRect.x == 2U && sliced.frames[0].atlasRect.y == 1U &&
            sliced.frames[0].atlasRect.width == 4U && sliced.frames[0].atlasRect.height == 6U &&
            sliced.frames[0].sourceWidth == 8U && sliced.frames[0].sourceOffsetX == 2 &&
            sliced.frames[0].sourceOffsetY == 1,
            "transparent trim metadata is incorrect");
    require(sliced.frames[1].atlasRect.x == 9U && sliced.frames[1].atlasRect.y == 2U &&
            sliced.frames[1].atlasRect.width == 6U && sliced.frames[1].atlasRect.height == 4U &&
            sliced.frames[1].sourceOffsetX == 1 && sliced.frames[1].sourceOffsetY == 2,
            "second grid cell was not trimmed in source coordinates");
    require(fixture.session.can_undo() && fixture.session.undo_label() == "Slice Sprite",
            "slice transaction did not produce one named undo record");
    require(fixture.session.undo() && fixture.session.asset().frames.size() == 1U,
            "slice undo did not restore the previous asset");
    require(fixture.session.redo() && fixture.session.asset().frames.size() == 2U,
            "slice redo did not restore the authored frames");

    const std::uint64_t beforeInvalid = fixture.session.asset().contentHash;
    const std::vector<editor::SpriteSliceSpec> duplicate{
        {"same", {0U, 0U, 8U, 8U}, {4.0F, 0.0F}, 0.1F, ""},
        {"same", {8U, 0U, 8U, 8U}, {4.0F, 0.0F}, 0.1F, ""},
    };
    require(!fixture.session.replace_slices(fixture.image, duplicate, true, 0U, true, &error) &&
            fixture.session.asset().contentHash == beforeInvalid,
            "invalid freeform slicing partially changed the document");
}

void test_timeline_preview_save_and_rhi_bridge() {
    Fixture fixture;
    editor::SpriteGridSliceSettings settings;
    settings.cellWidth = 8U;
    settings.cellHeight = 8U;
    settings.columns = 2U;
    settings.rows = 1U;
    settings.frameNamePrefix = "run";
    settings.pivotPixels = {4.0F, 0.0F};
    settings.durationSeconds = 0.1F;
    std::string error;
    require(fixture.session.slice_grid(fixture.image, settings, &error), error);
    require(fixture.session.set_frame_pivot(0U, {3.0F, 1.0F}, &error), error);
    require(fixture.session.undo_label() == "Set Sprite Pivot" &&
            fixture.session.set_frame_timing_event(1U, 0.2F, "footstep", &error), error);
    require(fixture.session.add_clip(
                {"ping", SpriteLoopMode::PingPong, 1.0F, {1U, 0U}}, &error), error);
    require(fixture.session.move_clip_frame("ping", 0U, 1U, &error), error);
    require(fixture.session.asset().clips[1].frames == std::vector<SpriteFrameIndex>({0U, 1U}),
            "timeline reorder selected the wrong frame");
    require(fixture.session.select_clip("ping") && fixture.session.seek_preview(0.15F),
            "preview clip selection or seek failed");
    const auto preview = fixture.session.preview(&error);
    require(preview && preview->sample.frame == 1U && preview->frame.event == "footstep" &&
            preview->renderList.items.size() == 1U && preview->renderList.batches.size() == 1U,
            error.empty() ? "authoring preview packet is incomplete" : error);
    require(fixture.session.select_frame(0U), "frame selection failed");
    const auto selectedPreview = fixture.session.preview(&error);
    require(selectedPreview && selectedPreview->sample.frame == 0U && !selectedPreview->playing,
            "paused authoring preview did not show the explicitly selected frame");

    rhi::NullDevice device;
    render::SpriteRhiRenderer renderer(device);
    render::SpriteRhiShaderBytecode shaders{{std::byte{1}}, {std::byte{2}}};
    require(renderer.initialize(shaders, rhi::TextureFormat::RGBA8Unorm, &error), error);
    rhi::TextureDesc atlasDesc;
    atlasDesc.width = 16U;
    atlasDesc.height = 8U;
    atlasDesc.usage = rhi::TextureUsage::Sampled;
    atlasDesc.initialState = rhi::ResourceState::ShaderRead;
    const rhi::TextureHandle atlas = device.create_texture(atlasDesc, &error);
    require(static_cast<bool>(atlas), error);
    rhi::TextureViewDesc atlasViewDesc;
    atlasViewDesc.texture = atlas;
    const rhi::TextureViewHandle atlasView = device.create_texture_view(atlasViewDesc, &error);
    require(static_cast<bool>(atlasView), error);
    require(renderer.register_texture(fixture.session.asset().textureAsset, atlasView, &error), error);
    rhi::TextureDesc targetDesc;
    targetDesc.width = 320U;
    targetDesc.height = 180U;
    targetDesc.usage = rhi::TextureUsage::RenderTarget;
    const rhi::TextureHandle target = device.create_texture(targetDesc, &error);
    require(static_cast<bool>(target), error);
    render::SpriteRhiFrameDesc frame;
    frame.colorTarget = target;
    frame.presentation = {320U, 180U, PixelScaleMode::IntegerFit, true, 8.0F};
    frame.outputWidth = 320U;
    frame.outputHeight = 180U;
    frame.items = selectedPreview->renderList.items;
    frame.batches = selectedPreview->renderList.batches;
    render::SpriteRhiFrameStats stats;
    require(renderer.record_frame(frame, stats, nullptr, &error), error);
    require(stats.items == 1U && stats.drawCalls == 1U && stats.triangles == 2U,
            "authoring preview did not execute through the sprite RHI bridge");
    require(renderer.unregister_texture(fixture.session.asset().textureAsset, &error), error);
    require(renderer.shutdown(&error), error);
    require(device.destroy_texture_view(atlasView, &error), error);
    require(device.destroy_texture(atlas, &error), error);
    require(device.destroy_texture(target, &error), error);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_sprite_authoring_v194.dvesprite";
    require(fixture.session.save(path, &error) && !fixture.session.dirty(), error);
    require(fixture.session.set_frame_timing_event(0U, 0.125F, "start", &error) &&
            fixture.session.dirty(), "saved-hash dirty tracking failed");
    editor::SpriteAuthoringSession reopened;
    require(reopened.open(path, &error) && !reopened.dirty() &&
            reopened.asset().frames[1].event == "footstep" &&
            reopened.asset().clips.size() == 2U,
            error.empty() ? "authored sprite round trip changed content" : error);
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

void test_empty_cell_policy() {
    Fixture fixture;
    for (std::uint32_t y = 0U; y < 8U; ++y)
        for (std::uint32_t x = 8U; x < 16U; ++x)
            fixture.pixels[(static_cast<std::size_t>(y) * 16U + x) * 4U + 3U] = std::byte{0};
    editor::SpriteGridSliceSettings settings;
    settings.cellWidth = 8U;
    settings.cellHeight = 8U;
    settings.columns = 2U;
    settings.rows = 1U;
    settings.pivotPixels = {4.0F, 0.0F};
    std::string error;
    require(fixture.session.slice_grid(fixture.image, settings, &error) &&
            fixture.session.asset().frames.size() == 1U,
            "transparent grid-cell skipping is incorrect");
    settings.skipTransparent = false;
    require(fixture.session.slice_grid(fixture.image, settings, &error) &&
            fixture.session.asset().frames.size() == 2U &&
            fixture.session.asset().frames[1].atlasRect.width == 8U,
            "preserved transparent grid cell did not retain its full rectangle");
}

} // namespace

int main() {
    try {
        test_grid_trim_and_atomic_history();
        test_timeline_preview_save_and_rhi_bridge();
        test_empty_cell_policy();
        std::cout << "dve_v194_sprite_authoring_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v194_sprite_authoring_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
