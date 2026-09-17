#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/sprite2d.hpp"
#include "dve/component.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float left, float right, float epsilon = 0.0001F) {
    return std::abs(left - right) <= epsilon;
}

SpriteAsset sprite_asset(std::string texture = "textures/original_hero.png") {
    SpriteAsset asset;
    asset.name = "Original Hero";
    asset.textureAsset = std::move(texture);
    asset.textureWidth = 64U;
    asset.textureHeight = 16U;
    asset.pixelsPerWorldUnit = 16.0F;
    asset.sampling = SpriteSampling::Nearest;
    asset.materialId = 7U;
    asset.paletteBank = 2U;
    asset.frames = {
        {"idle", {0U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, ""},
        {"step_a", {16U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.2F, "footstep"},
        {"step_b", {32U, 0U, 16U, 16U}, 16U, 16U, 0, 0, {8.0F, 0.0F}, 0.1F, ""},
    };
    asset.clips = {
        {"walk", SpriteLoopMode::Loop, 1.0F, {0U, 1U, 2U}},
        {"once", SpriteLoopMode::Once, 1.0F, {0U, 1U, 2U}},
        {"ping", SpriteLoopMode::PingPong, 1.0F, {0U, 1U, 2U}},
    };
    asset.recompute_hash();
    return asset;
}

void test_pixel_presentation_and_mapping() {
    PixelPresentationConfig config;
    config.logicalWidth = 320U;
    config.logicalHeight = 180U;
    const auto integer = compute_pixel_presentation(config, 1366U, 768U);
    require(integer.has_value(), "integer-fit presentation was rejected");
    require(integer->integerScale && integer->viewportWidth == 1280U &&
            integer->viewportHeight == 720U && integer->viewportX == 43 &&
            integer->viewportY == 24,
            "integer-fit viewport or letterbox is incorrect");
    const auto logical = output_to_logical_pixel(config, *integer, {683.0F, 384.0F});
    require(logical && close(logical->x, 160.0F) && close(logical->y, 90.0F),
            "output-to-logical input mapping is incorrect");
    require(!output_to_logical_pixel(config, *integer, {10.0F, 10.0F}),
            "letterbox input was accepted without clamping");
    const SpriteVec2 output = logical_to_output_pixel(*integer, *logical);
    require(close(output.x, 683.0F) && close(output.y, 384.0F),
            "logical-to-output mapping did not round trip");
    const auto highDpi = window_to_logical_pixel(
        config, *integer, {341.5F, 192.0F}, 683U, 384U);
    require(highDpi && close(highDpi->x, 160.0F) && close(highDpi->y, 90.0F),
            "high-DPI window pointer did not map through drawable pixels");

    const auto downscaled = compute_pixel_presentation(config, 160U, 90U);
    require(downscaled && !downscaled->integerScale && close(downscaled->scaleX, 0.5F),
            "fractional downscale fallback is incorrect");
    config.allowFractionalDownscale = false;
    require(!compute_pixel_presentation(config, 160U, 90U),
            "forbidden fractional downscale was accepted");
    config.allowFractionalDownscale = true;
    config.scaleMode = PixelScaleMode::IntegerFill;
    const auto fill = compute_pixel_presentation(config, 800U, 800U);
    require(fill && fill->integerScale && fill->cropped && fill->viewportWidth == 1600U &&
            fill->viewportHeight == 900U && fill->viewportX == -400 && fill->viewportY == -50,
            "integer-fill crop is incorrect");

    const Float3 xy = snap_world_to_pixel({1.031F, 2.094F, 7.3F}, {}, 16.0F, GameplayPlane2D::XY);
    require(close(xy.x, 1.0F) && close(xy.y, 2.125F) && close(xy.z, 7.3F),
            "XY pixel snapping changed the wrong axes");
    const Float3 xz = snap_world_to_pixel({1.031F, 7.3F, 2.094F}, {}, 16.0F, GameplayPlane2D::XZ);
    require(close(xz.x, 1.0F) && close(xz.y, 7.3F) && close(xz.z, 2.125F),
            "XZ pixel snapping changed the wrong axes");
}

void test_asset_validation_hash_and_codec() {
    SpriteAsset asset = sprite_asset();
    std::string error;
    require(asset.validate(&error), error);
    const std::uint64_t hash = asset.contentHash;
    asset.frames[0].durationSeconds = 0.125F;
    require(sprite_asset_content_hash(asset) != hash, "sprite hash ignored frame duration");
    asset = sprite_asset();

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_sprite_v191_test.dvesprite";
    require(write_dvesprite(path, asset, &error), error);
    const SpriteAssetReadResult read = read_dvesprite(path);
    require(read && read.asset.contentHash == asset.contentHash && read.asset.frames.size() == 3U &&
            read.asset.clips.size() == 3U && read.asset.frames[1].event == "footstep",
            read.error.empty() ? "sprite asset round trip changed content" : read.error);
    require(!read_dvesprite(path, 8U), "sprite byte limit was ignored");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream << "trailing";
    }
    require(!read_dvesprite(path), "trailing sprite data was accepted");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    SpriteAsset invalid = sprite_asset();
    invalid.frames[0].atlasRect.x = 63U;
    require(!invalid.validate(), "out-of-bounds atlas rectangle was accepted");
    invalid = sprite_asset();
    invalid.clips[0].frames.push_back(99U);
    require(!invalid.validate(), "invalid clip frame index was accepted");
}

void test_clip_sampling() {
    const SpriteAsset asset = sprite_asset();
    const auto loopA = sample_sprite_clip(asset, "walk", 0.05F);
    const auto loopB = sample_sprite_clip(asset, "walk", 0.15F);
    const auto loopWrap = sample_sprite_clip(asset, "walk", 0.45F);
    require(loopA && loopA->frame == 0U && loopB && loopB->frame == 1U &&
            loopWrap && loopWrap->frame == 0U,
            "looping sprite sampling selected the wrong frame");
    const auto once = sample_sprite_clip(asset, "once", 2.0F);
    require(once && once->frame == 2U && once->finished,
            "one-shot sprite sampling did not clamp to its last frame");
    const auto pingForward = sample_sprite_clip(asset, "ping", 0.35F);
    const auto pingBackward = sample_sprite_clip(asset, "ping", 0.45F);
    require(pingForward && pingForward->frame == 2U && pingBackward && pingBackward->frame == 1U,
            "ping-pong sprite sampling did not reverse");
    require(!sample_sprite_clip(asset, "missing", 0.0F) &&
            !sample_sprite_clip(asset, "walk", -1.0F),
            "invalid sprite sampling request was accepted");
}

void test_runtime_render_order_batches_and_events() {
    SpriteRuntime runtime;
    std::string error;
    require(runtime.register_asset(1U, sprite_asset(), &error), error);
    require(runtime.register_asset(2U, sprite_asset("textures/original_enemy.png"), &error), error);

    SpriteInstanceDesc hero;
    hero.asset = 1U;
    hero.clip = "walk";
    hero.transform = make_rigid_transform({1.031F, 2.094F, 0.0F}, {});
    hero.sortingLayer = 1;
    hero.orderInLayer = 2;
    require(runtime.bind(20U, hero, &error), error);

    SpriteInstanceDesc background = hero;
    background.transform = make_rigid_transform({}, {});
    background.sortingLayer = -1;
    background.orderInLayer = 0;
    require(runtime.bind(30U, background, &error), error);

    SpriteInstanceDesc enemy = hero;
    enemy.asset = 2U;
    enemy.transform = make_rigid_transform({3.0F, 4.0F, 0.0F}, {});
    enemy.sortingLayer = 1;
    enemy.orderInLayer = 1;
    require(runtime.bind(10U, enemy, &error), error);

    SpriteRenderList list = runtime.build_render_list();
    require(list.items.size() == 3U && list.items[0].owner == 30U &&
            list.items[1].owner == 10U && list.items[2].owner == 20U,
            "sprite semantic draw ordering is not deterministic");
    require(list.batches.size() == 3U,
            "batching crossed an intervening semantic draw order");
    require(close(list.items[2].vertices[0].position.x, 0.5F) &&
            close(list.items[2].vertices[0].position.y, 2.125F),
            "sprite pivot or pixel-snapped quad placement is incorrect");

    runtime.tick(0.11F);
    require(runtime.events().size() == 3U && runtime.events()[0].name == "footstep",
            "sprite frame events were not published in owner order");
    const auto sample = runtime.sample(20U);
    require(sample && sample->frame == 1U, "sprite runtime did not advance the clip");
    require(!runtime.unregister_asset(1U), "runtime removed an asset still used by instances");
    require(runtime.unbind(20U) && runtime.unbind(30U) && runtime.unregister_asset(1U),
            "sprite instance or asset teardown failed");

    SpriteInstanceDesc topDown;
    topDown.asset = 2U;
    topDown.clip = "walk";
    topDown.plane = GameplayPlane2D::XZ;
    topDown.transform = make_rigid_transform({0.0F, 5.0F, 0.0F}, {});
    require(runtime.unbind(10U), "enemy teardown failed");
    require(runtime.bind(40U, topDown, &error), error);
    list = runtime.build_render_list();
    require(list.items.size() == 1U && close(list.items[0].vertices[0].position.y, 5.0F) &&
            list.items[0].vertices[2].position.z > list.items[0].vertices[0].position.z,
            "XZ sprite quad did not preserve depth-axis placement");
}

void test_mixed_visual_sort_contract() {
    const std::vector<Visual2DSortEntry> input = {
        {8U, Visual2DKind::Model3D, 1, 0, 0.0F, 4U},
        {5U, Visual2DKind::Sprite, 0, 9, 0.0F, 2U},
        {7U, Visual2DKind::Particle, 1, 0, -1.0F, 3U},
        {6U, Visual2DKind::FlatMesh, 1, 0, 0.0F, 1U},
    };
    const auto sorted = sort_visual_2d_entries(input);
    require(sorted.size() == 4U && sorted[0].owner == 5U && sorted[1].owner == 7U &&
            sorted[2].owner == 6U && sorted[3].owner == 8U,
            "mixed sprite/3D visual sorting is not deterministic");
}

void test_sprite_component_schema() {
    const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
    const ComponentTypeSchema* schema = registry.find("dve.sprite");
    require(schema != nullptr && schema->properties.size() == 12U,
            "sprite component schema is missing or incomplete");
    const ComponentTypeSchema* world = registry.find("dve.physics2d_world");
    const ComponentTypeSchema* body = registry.find("dve.physics2d_body");
    const ComponentTypeSchema* collider = registry.find("dve.physics2d_collider");
    const ComponentTypeSchema* character = registry.find("dve.sideview_character");
    require(world != nullptr && world->properties.size() == 7U,
            "2D physics world component schema is missing or incomplete");
    require(body != nullptr && body->properties.size() == 9U,
            "2D physics body component schema is missing or incomplete");
    require(collider != nullptr && collider->allowMultiple && collider->properties.size() == 17U,
            "2D collider component schema is missing or incomplete");
    require(character != nullptr && !character->allowMultiple && character->properties.size() == 21U,
            "side-view character component schema is missing or incomplete");
}

} // namespace

int main() {
    try {
        test_pixel_presentation_and_mapping();
        test_asset_validation_hash_and_codec();
        test_clip_sampling();
        test_runtime_render_order_batches_and_events();
        test_mixed_visual_sort_contract();
        test_sprite_component_schema();
        std::cout << "dve_v191_sprite2d_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v191_sprite2d_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
