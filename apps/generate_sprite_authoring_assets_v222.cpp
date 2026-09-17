#include "dve/sprite_pixel_art.hpp"
#include "dve/sprite_rig2d.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

SpritePaletteAsset palette() {
    SpritePaletteAsset asset;
    asset.name = "Sun Pilot v2.22";
    asset.transparentIndex = 0U;
    asset.banks.push_back({"Day", {
        {0,0,0,0}, {24,32,56,255}, {56,88,136,255}, {88,152,216,255},
        {248,232,160,255}, {240,160,72,255}, {224,72,64,255}, {255,255,255,255}
    }});
    asset.banks.push_back({"Night", {
        {0,0,0,0}, {12,16,36,255}, {40,56,104,255}, {72,104,168,255},
        {176,208,248,255}, {144,112,216,255}, {200,64,128,255}, {232,240,255,255}
    }});
    asset.recompute_hash();
    return asset;
}

void draw_frame(SpritePixelArtSession& session, bool blink, std::string* error) {
    session.set_index(1U);
    if (!session.draw_rectangle({4,2,8,12}, true, error)) throw std::runtime_error(*error);
    session.set_index(2U);
    if (!session.draw_rectangle({5,4,6,6}, true, error)) throw std::runtime_error(*error);
    session.set_index(4U);
    if (!session.draw_rectangle({6,10,4,3}, true, error)) throw std::runtime_error(*error);
    session.set_index(5U);
    if (!session.draw_line(4,13,11,13,error)) throw std::runtime_error(*error);
    session.set_index(6U);
    if (blink) {
        if (!session.draw_line(6,11,7,11,error) || !session.draw_line(9,11,10,11,error))
            throw std::runtime_error(*error);
    } else {
        if (!session.paint(6,11,error) || !session.paint(10,11,error)) throw std::runtime_error(*error);
    }
    session.set_index(7U);
    if (!session.paint(8,7,error)) throw std::runtime_error(*error);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: generate_sprite_authoring_assets_v222 <project-root>");
        const std::filesystem::path root = argv[1];
        const auto output = root / "assets/sprites";
        std::filesystem::create_directories(output);
        std::string error;
        SpritePixelArtSession pixels;
        if (!pixels.new_document("Sun Pilot v2.22", 16U, 16U, SpritePixelStorage::Indexed8,
                                 palette(), &error)) throw std::runtime_error(error);
        draw_frame(pixels, false, &error);
        if (!pixels.add_frame("sun_pilot_blink", false, &error)) throw std::runtime_error(error);
        draw_frame(pixels, true, &error);
        if (!pixels.set_frame_duration(0U, 0.6F, &error) ||
            !pixels.set_frame_duration(1U, 0.1F, &error)) throw std::runtime_error(error);
        if (!pixels.save(output / "v222_sun_pilot.dvepixel", &error)) throw std::runtime_error(error);
        SpritePixelArtPublishSettings publish;
        publish.imagePath = output / "v222_sun_pilot_index.png";
        publish.spritePath = output / "v222_sun_pilot.dvesprite";
        publish.palettePath = output / "v222_sun_pilot.dvepalette";
        publish.imageAssetReference = "v222_sun_pilot_index.png";
        publish.paletteAssetReference = "v222_sun_pilot.dvepalette";
        publish.columns = 2U;
        publish.clipName = "idle";
        SpritePixelArtPublishResult result;
        if (!pixels.publish(publish, result, &error)) throw std::runtime_error(error);

        SpriteRig2DAuthoringSession rig;
        SpriteRigBoneId torso{}, arm{}, hand{};
        if (!rig.add_bone("torso", 1U, {{0,4},0,{1,1}}, 10.0F, &torso, &error) ||
            !rig.add_bone("arm", torso, {{5,8},-20,{1,1}}, 8.0F, &arm, &error) ||
            !rig.add_bone("hand", arm, {{8,0},20,{1,1}}, 6.0F, &hand, &error))
            throw std::runtime_error(error);
        SpriteRigPart2D body;
        body.name = "body"; body.bone = torso; body.spriteAsset = "v222_sun_pilot.dvesprite";
        body.clip = "idle"; body.drawOrder = 0;
        SpriteRigPartId bodyId{};
        if (!rig.add_part(body, &bodyId, &error)) throw std::runtime_error(error);
        SpriteRigPart2D badge;
        badge.name = "badge"; badge.bone = hand; badge.spriteAsset = "v222_sun_pilot.dvesprite";
        badge.clip = "idle"; badge.local.scale = {0.4F,0.4F}; badge.drawOrder = 2;
        SpriteRigPartId badgeId{};
        if (!rig.add_part(badge, &badgeId, &error)) throw std::runtime_error(error);
        if (!rig.add_clip({"wave",1.0F,true,{},{}}, &error)) throw std::runtime_error(error);
        if (!rig.upsert_bone_key("wave", {arm,0.0F,{{5,8},-30,{1,1}}}, &error) ||
            !rig.upsert_bone_key("wave", {arm,0.5F,{{5,8},35,{1,1}}}, &error) ||
            !rig.upsert_bone_key("wave", {arm,1.0F,{{5,8},-30,{1,1}}}, &error))
            throw std::runtime_error(error);
        SpriteRigConstraintId ik{};
        if (!rig.add_two_bone_ik({0U,"hand_target",arm,hand,{18,18},1.0F,0.4F},&ik,&error))
            throw std::runtime_error(error);
        SpriteRigVariant2D night;
        night.name = "night";
        night.overrides.push_back({"body",std::nullopt,std::nullopt,std::nullopt,std::optional<std::uint32_t>{1U}});
        night.overrides.push_back({"badge",std::nullopt,std::nullopt,std::optional<bool>{false},std::nullopt});
        if (!rig.add_variant(std::move(night), &error)) throw std::runtime_error(error);
        if (!rig.save(output / "v222_sun_pilot.dvespriterig", &error)) throw std::runtime_error(error);
        std::cout << "generated v2.22 pixel-art and rig assets\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
