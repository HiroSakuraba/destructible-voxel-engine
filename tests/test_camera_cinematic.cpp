#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/camera_sequence.hpp"
#include "dve/render/camera_frame_graph.hpp"

namespace {
using namespace dve;
using namespace dve::camera;
using namespace dve::render;

void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool close(float a,float b,float epsilon=1.0e-4F){return std::abs(a-b)<=epsilon;}

CameraFloatImage make_gradient(std::uint32_t width,std::uint32_t height){
    CameraFloatImage image;
    image.width=width;image.height=height;
    image.rgba.resize(static_cast<std::size_t>(width)*height*4U);
    image.linearDepthMeters.assign(static_cast<std::size_t>(width)*height,5.0F);
    for(std::uint32_t y=0U;y<height;++y){
        for(std::uint32_t x=0U;x<width;++x){
            const std::size_t pixel=static_cast<std::size_t>(y)*width+x;
            image.rgba[pixel*4U]=static_cast<float>(x)/static_cast<float>(std::max(1U,width-1U));
            image.rgba[pixel*4U+1U]=static_cast<float>(y)/static_cast<float>(std::max(1U,height-1U));
            image.rgba[pixel*4U+2U]=0.25F;
            image.rgba[pixel*4U+3U]=1.0F;
        }
    }
    return image;
}

void test_filmback_presets_profiles_and_gpu_packet(){
    const auto imax=camera_physical_lens_preset(CameraFilmbackPreset::Imax15Perf,50.0F);
    std::string error;
    require(imax.validate(&error),error.c_str());
    require(close(imax.sensorWidthMillimeters,70.41F)&&close(imax.sensorHeightMillimeters,52.63F),
            "IMAX 15-perf filmback dimensions changed");
    const auto anamorphic=camera_cinematic_preset(CameraCinematicPreset::VintageAnamorphic);
    require(anamorphic.validate(&error),error.c_str());
    require(anamorphic.cinematic.film.framing==CameraFramingPreset::Scope239&&
            close(anamorphic.cinematic.lens.anamorphicSqueeze,2.0F)&&
            anamorphic.cinematic.bokeh.anamorphicRatio>1.9F,
            "vintage anamorphic preset is incomplete");
    require(close(camera_framing_aspect_ratio(anamorphic.cinematic.film,16.0F/9.0F),2.39F),
            "scope framing aspect is wrong");

    CameraPose pose;pose.lens.physical=imax;
    CameraViewportDesc viewport{1U,"Cinema",1U,0.0F,0.0F,1.0F,1.0F,"main",true};
    const auto packet=pack_camera_gpu_packet(viewport,pose,anamorphic,7U,true);
    require(close(packet.lensEffects1[2],2.0F)&&
            close(packet.film1[2],static_cast<float>(CameraFramingPreset::Scope239))&&
            packet.cameraCutGeneration==7U&&packet.resetTemporalHistory==1U,
            "cinematic GPU packet fields were not published");
}

void test_sequence_v3_round_trip_and_interpolation(){
    CameraSequence sequence;sequence.name="Lens test";sequence.durationSeconds=2.0F;
    CameraDollySpline dolly;dolly.id=4U;dolly.name="Lens dolly";
    CameraDollyKey a;a.timeSeconds=0.0F;a.pose.position={0.0F,1.0F,5.0F};a.pose.target={0.0F,1.0F,0.0F};
    a.pose.lens.physical=camera_physical_lens_preset(CameraFilmbackPreset::Super35,35.0F);
    a.postProcess=camera_cinematic_preset(CameraCinematicPreset::SplitDiopter);
    a.postProcess.cinematic.colorGrade.temperatureKelvin=4300.0F;
    a.postProcess.cinematic.film.grainIntensity=0.15F;
    CameraDollyKey b=a;b.timeSeconds=2.0F;b.pose.position={0.0F,1.0F,2.0F};
    b.pose.lens.physical.focalLengthMillimeters=85.0F;
    b.postProcess.cinematic.splitDiopter.farFocusDistanceMeters=30.0F;
    b.postProcess.cinematic.colorGrade.temperatureKelvin=7500.0F;
    dolly.keys={a,b};sequence.dollies.push_back(dolly);
    CameraShot shot;shot.id=9U;shot.name="Lens";shot.startSeconds=0.0F;shot.durationSeconds=2.0F;
    shot.dollyId=4U;shot.blendIn={CameraBlendCurve::Cut,0.0F};sequence.shots.push_back(shot);
    std::string error;require(sequence.validate(&error),error.c_str());
    const std::string serialized=sequence.serialize();
    require(serialized.starts_with("DVE_CAMERA_SEQUENCE 3\n"),"camera sequence did not advance to v3");
    const auto parsed=CameraSequence::parse(serialized,&error);require(parsed.has_value(),error.c_str());
    const auto middle=parsed->evaluate(1.0F);require(middle.pose.has_value(),"v3 sequence lost dolly pose");
    // 2026-07-25: these midpoints were the linear-in-display-units values (21.0 m, 5900 K,
    // 60 mm). They are now the perceptual ones, because these controls do not interpolate
    // meaningfully in the units they are labelled in: focus is linear in diopters, colour
    // temperature in mireds, and focal length is logarithmic. The expected values below are the
    // closed forms, not observed output:
    //   focus       1 / mean(1/12, 1/30)              = 17.1429 m
    //   temperature 1e6 / mean(1e6/4300, 1e6/7500)    = 5466.1 K
    //   focal       sqrt(35 * 85)                     = 54.5436 mm
    require(middle.postProcess.cinematic.splitDiopter.enabled&&
            close(middle.postProcess.cinematic.splitDiopter.farFocusDistanceMeters,17.1429F,0.01F)&&
            close(middle.postProcess.cinematic.colorGrade.temperatureKelvin,5466.1F,1.0F)&&
            close(middle.pose->lens.physical.focalLengthMillimeters,54.5436F,0.01F),
            "v3 cinematic values did not round-trip and interpolate");
}

void test_cube_lut_and_color_grading(){
    const std::string cube=
        "TITLE \"Invert\"\nLUT_3D_SIZE 2\n"
        "1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n";
    std::string error;const auto lut=parse_camera_cube_lut(cube,&error);require(lut.has_value(),error.c_str());
    CameraFloatImage image;image.width=1U;image.height=1U;image.rgba={1.0F,0.0F,0.0F,1.0F};image.linearDepthMeters={1.0F};
    CameraPose pose;CameraPostProcessProfile post;
    post.cinematic.colorGrade.toneMap=CameraToneMapCurve::Linear;
    post.cinematic.colorGrade.lutBlend=1.0F;
    CameraCinematicPipelineSettings settings;settings.applyDepthOfField=false;
    settings.applyLensDistortion=false;settings.applyFilmEffects=false;settings.applyFraming=false;
    const auto telemetry=apply_camera_cinematic_pipeline(image,pose,post,settings,&*lut,nullptr);
    require(telemetry.gradedPixels==1U,"color-grade telemetry did not count pixel");
    require(image.rgba[0]<0.05F&&image.rgba[1]>0.95F&&image.rgba[2]>0.95F,
            "3D LUT grading did not invert red to cyan");
}

void test_split_diopter_and_bokeh(){
    CameraFloatImage source;source.width=80U;source.height=40U;
    source.rgba.assign(80U*40U*4U,0.0F);source.linearDepthMeters.assign(80U*40U,5.0F);
    for(std::size_t pixel=0U;pixel<80U*40U;++pixel)source.rgba[pixel*4U+3U]=1.0F;
    const std::size_t left=20U*80U+16U,right=20U*80U+64U,center=20U*80U+40U;
    source.rgba[left*4U]=1.0F;source.rgba[right*4U+1U]=1.0F;source.rgba[center*4U+2U]=1.0F;
    source.linearDepthMeters[left]=1.0F;source.linearDepthMeters[right]=12.0F;source.linearDepthMeters[center]=5.0F;
    CameraPose pose;pose.lens.physical=camera_physical_lens_preset(CameraFilmbackPreset::FullFrame35,85.0F);
    CameraPostProcessProfile post=camera_cinematic_preset(CameraCinematicPreset::SplitDiopter);
    post.cinematic.splitDiopter.nearFocusDistanceMeters=1.0F;
    post.cinematic.splitDiopter.farFocusDistanceMeters=12.0F;
    post.cinematic.splitDiopter.centerX=0.0F;post.cinematic.splitDiopter.angleRadians=0.0F;
    post.cinematic.bokeh.bladeCount=5U;post.cinematic.bokeh.roundness=0.0F;
    post.cinematic.bokeh.anamorphicRatio=2.4F;post.cinematic.bokeh.catEye=0.8F;
    auto hex=source;
    const auto telemetry=apply_camera_depth_of_field(hex,pose,post,
        {CameraDofQuality::High,10.0F,0.05F,false,true,true,true});
    require(telemetry.blurredPixels>100U&&hex.rgba[left*4U]>0.45F&&hex.rgba[right*4U+1U]>0.45F,
            "split diopter did not preserve both focus regions");
    auto round=source;post.cinematic.bokeh.bladeCount=0U;post.cinematic.bokeh.roundness=1.0F;
    post.cinematic.bokeh.anamorphicRatio=1.0F;post.cinematic.bokeh.catEye=0.0F;
    (void)apply_camera_depth_of_field(round,pose,post,
        {CameraDofQuality::High,10.0F,0.05F,false,true,true,true});
    require(!std::equal(hex.rgba.begin(),hex.rgba.end(),round.rgba.begin()),
            "aperture blade settings did not alter bokeh sampling");
}

void test_distortion_framing_grain_and_motion_blur(){
    CameraPose pose;CameraPostProcessProfile post=camera_cinematic_preset(CameraCinematicPreset::FisheyeAction);
    post.cinematic.film.framing=CameraFramingPreset::Scope239;
    post.cinematic.film.grainIntensity=0.2F;
    post.cinematic.colorGrade.toneMap=CameraToneMapCurve::Linear;
    auto first=make_gradient(96U,64U);auto second=first;auto nextFrame=first;
    CameraCinematicPipelineSettings settings;settings.applyDepthOfField=false;settings.frameIndex=10U;
    const auto telemetry=apply_camera_cinematic_pipeline(first,pose,post,settings,nullptr,nullptr);
    (void)apply_camera_cinematic_pipeline(second,pose,post,settings,nullptr,nullptr);
    settings.frameIndex=11U;(void)apply_camera_cinematic_pipeline(nextFrame,pose,post,settings,nullptr,nullptr);
    require(telemetry.distortedPixels>1000U&&telemetry.mattePixels>0U&&
            telemetry.maximumDistortionPixels>1.0F,"fisheye or scope framing did not execute");
    require(first.rgba==second.rgba,"film grain is not deterministic for one frame seed");
    require(first.rgba!=nextFrame.rgba,"film grain did not vary across frames");
    require(first.rgba[0]<0.05F&&first.rgba[1]<0.05F&&first.rgba[2]<0.05F,
            "scope matte did not cover the image corner");

    CameraFloatImage motion;motion.width=32U;motion.height=8U;
    motion.rgba.assign(32U*8U*4U,0.0F);motion.linearDepthMeters.assign(32U*8U,2.0F);
    for(std::size_t pixel=0U;pixel<32U*8U;++pixel)motion.rgba[pixel*4U+3U]=1.0F;
    const std::size_t bright=4U*32U+8U;motion.rgba[bright*4U]=1.0F;
    CameraMotionField field;field.width=32U;field.height=8U;field.xyPixels.assign(32U*8U*2U,0.0F);
    for(std::size_t pixel=0U;pixel<32U*8U;++pixel)field.xyPixels[pixel*2U]=12.0F;
    std::uint64_t blurred{};std::string error;
    require(apply_camera_motion_blur(motion,field,1.0F,360.0F,16U,&blurred,&error),error.c_str());
    require(blurred>0U&&motion.rgba[(4U*32U+12U)*4U]>0.01F,
            "motion-vector blur did not spread the bright sample");
}

}

int main(){
    try{
        test_filmback_presets_profiles_and_gpu_packet();
        test_sequence_v3_round_trip_and_interpolation();
        test_cube_lut_and_color_grading();
        test_split_diopter_and_bokeh();
        test_distortion_framing_grain_and_motion_blur();
        std::cout<<"dve_camera_cinematic_tests: PASS\n";return 0;
    }catch(const std::exception& error){
        std::cerr<<"dve_camera_cinematic_tests: FAIL: "<<error.what()<<'\n';return 1;
    }
}
