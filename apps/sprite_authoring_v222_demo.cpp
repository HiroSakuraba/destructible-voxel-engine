#include "dve/sprite_pixel_art.hpp"
#include "dve/sprite_rig2d.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "sprite_authoring_v222_demo.json";
        dve::SpritePaletteAsset palette;
        palette.name = "Demo"; palette.transparentIndex = 0U;
        palette.banks.push_back({"Default", {{0,0,0,0},{255,240,160,255},{32,48,80,255},{240,96,72,255}}});
        palette.recompute_hash();
        dve::editor::SpritePixelArtSession pixels;
        std::string error;
        if (!pixels.new_document("v2.22 Hero", 16U, 16U, dve::editor::SpritePixelStorage::Indexed8,
                                 palette, &error)) throw std::runtime_error(error);
        pixels.set_index(1U);
        if (!pixels.draw_rectangle({4,2,8,12}, true, &error)) throw std::runtime_error(error);
        pixels.set_index(2U);
        if (!pixels.draw_rectangle({5,3,6,4}, true, &error)) throw std::runtime_error(error);
        pixels.set_index(3U);
        if (!pixels.draw_line(4, 12, 11, 12, &error)) throw std::runtime_error(error);
        if (!pixels.add_frame("hero_blink", true, &error)) throw std::runtime_error(error);

        dve::editor::SpriteRig2DAuthoringSession rig;
        dve::SpriteRigBoneId body{};
        if (!rig.add_bone("body", 1U, {{0,0},0,{1,1}}, 12.0F, &body, &error)) throw std::runtime_error(error);
        dve::SpriteRigPart2D part;
        part.name = "hero"; part.bone = body; part.spriteAsset = "v222_hero.dvesprite"; part.clip = "default";
        if (!rig.add_part(part, nullptr, &error)) throw std::runtime_error(error);
        dve::SpriteRigPose2D pose;
        if (!dve::sample_sprite_rig2d(rig.asset(), "idle", 0.0F, {}, pose, &error)) throw std::runtime_error(error);

        std::ofstream stream(output, std::ios::trunc);
        stream << "{\n"
               << "  \"pixel_document_hash\": " << pixels.document().contentHash << ",\n"
               << "  \"pixel_frames\": " << pixels.document().frames.size() << ",\n"
               << "  \"rig_hash\": " << rig.asset().contentHash << ",\n"
               << "  \"rig_bones\": " << rig.asset().bones.size() << ",\n"
               << "  \"rig_parts\": " << rig.asset().parts.size() << ",\n"
               << "  \"pose_hash\": " << pose.poseHash << "\n"
               << "}\n";
        std::cout << output.string() << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
