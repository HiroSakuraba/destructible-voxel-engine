#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "dve/camera_runtime.hpp"
#include "dve/camera_sequence.hpp"
#include "dve/game_world.hpp"
#include "dve/render/camera_frame_graph.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace {
using namespace dve;
using namespace dve::camera;
using namespace dve::render;

CameraFloatImage make_scene(std::uint32_t width, std::uint32_t height, bool alternate) {
    CameraFloatImage image;
    image.width=width; image.height=height;
    image.rgba.assign(static_cast<std::size_t>(width)*height*4U,0.0F);
    image.linearDepthMeters.assign(static_cast<std::size_t>(width)*height,24.0F);
    for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){
        const std::size_t p=static_cast<std::size_t>(y)*width+x;
        image.rgba[p*4U+0U]=0.025F+0.06F*static_cast<float>(y)/height;
        image.rgba[p*4U+1U]=0.035F+0.08F*static_cast<float>(y)/height;
        image.rgba[p*4U+2U]=0.055F+0.10F*static_cast<float>(y)/height;
        image.rgba[p*4U+3U]=1.0F;
    }
    auto disc=[&](float cx,float cy,float radius,float depth,Float3 color){
        for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){
            const float dx=static_cast<float>(x)-cx,dy=static_cast<float>(y)-cy;
            if(dx*dx+dy*dy>radius*radius)continue;
            const std::size_t p=static_cast<std::size_t>(y)*width+x;
            image.rgba[p*4U+0U]=color.x;image.rgba[p*4U+1U]=color.y;image.rgba[p*4U+2U]=color.z;
            image.linearDepthMeters[p]=depth;
        }
    };
    disc(width*0.28F,height*0.54F,height*0.15F,3.5F,alternate?Float3{0.25F,0.75F,1.0F}:Float3{1.0F,0.35F,0.14F});
    disc(width*0.52F,height*0.42F,height*0.12F,7.0F,alternate?Float3{1.0F,0.72F,0.18F}:Float3{0.18F,0.85F,0.45F});
    disc(width*0.75F,height*0.58F,height*0.18F,16.0F,alternate?Float3{0.9F,0.22F,0.55F}:Float3{0.28F,0.42F,1.0F});
    return image;
}

CameraSequence make_sequence(){
    CameraSequence q;q.name="V1.37 Constant Speed Reveal";q.durationSeconds=4.0F;
    q.metadata["editor.track.color"]="#5A8DEE";q.metadata["unknown.external.note"]="preserved";
    CameraDollySpline d;d.id=7;d.name="Constant Speed Arc";d.arcLengthSamplesPerSegment=64;
    CameraDollyKey a;a.timeSeconds=0;a.pose.position={-4,2,8};a.pose.target={0,1,0};a.pose.lens.physical.enabled=true;a.pose.lens.physical.focalLengthMillimeters=35;a.pose.lens.physical.focusDistanceMeters=7;a.pose.lens.physical.apertureFStop=1.8F;a.postProcess.depthOfFieldWeight=1;a.postProcess.focusDistanceMeters=7;a.postProcess.apertureFStop=1.8F;a.postProcess.bloomIntensity=1.2F;
    CameraDollyKey b=a;b.timeSeconds=1.2F;b.pose.position={-1,4,5};b.pose.lens.physical.focalLengthMillimeters=55;b.pose.lens.physical.focusDistanceMeters=4.5F;b.postProcess.focusDistanceMeters=4.5F;
    CameraDollyKey c=b;c.timeSeconds=4;c.pose.position={4,2,7};c.pose.lens.physical.focalLengthMillimeters=70;c.pose.lens.physical.focusDistanceMeters=8;c.postProcess.focusDistanceMeters=8;
    d.keys={a,b,c};q.dollies.push_back(d);
    CameraShot shot;shot.id=1;shot.name="Reveal";shot.startSeconds=0;shot.durationSeconds=4;shot.dollyId=7;shot.constantSpeedDolly=true;shot.blendIn={CameraBlendCurve::Cut,0};shot.marker="reveal";q.shots.push_back(shot);
    q.events.push_back({1,1.0F,"impact","foundry_door"});q.events.push_back({2,3.0F,"music","hero_layer"});return q;
}

void write_ppm(const std::filesystem::path& path,const CameraFloatImage& image){
    const auto rgba=camera_image_to_srgb8(image);std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(std::size_t p=0;p<static_cast<std::size_t>(image.width)*image.height;++p){out.put(static_cast<char>(rgba[p*4U]));out.put(static_cast<char>(rgba[p*4U+1U]));out.put(static_cast<char>(rgba[p*4U+2U]));}
}
}

int main(int argc,char**argv){
    try{
        const std::filesystem::path output=argc>1?argv[1]:"camera_render_v1_37_demo";std::filesystem::create_directories(output);
        GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());std::string error;
        if(!world.cameras().add_viewport({1,"Gameplay",1,0,0,0.5F,1,"main",true},&error)||
           !world.cameras().add_viewport({2,"Director",2,0.5F,0,0.5F,1,"main",true},&error)||
           !world.cameras().add_viewport({3,"Security",4,0,0,1,1,"security",true},&error))throw std::runtime_error(error);
        CameraRig fixed;fixed.id=10;fixed.name="Director";fixed.priority=1;fixed.authoredPose.position={5,4,8};fixed.authoredPose.target={0,1,0};fixed.collision.enabled=false;
        if(!world.cameras().director(2)->add_or_replace_rig(fixed,&error)||!world.cameras().director(3)->add_or_replace_rig(fixed,&error))throw std::runtime_error(error);
        CameraSequence sequence=make_sequence();if(!world.cameras().set_sequence(1,sequence,&error)||!world.cameras().play_sequence(1,false))throw std::runtime_error(error);
        CameraAccessibilitySettings accessibility;accessibility.preset=CameraAccessibilityPreset::ReducedMotion;accessibility.horizonLock=true;accessibility.motionScale=0.85F;accessibility.shakeScale=0.35F;accessibility.bloomScale=0.55F;accessibility.maximumDepthOfFieldWeight=0.75F;if(!world.cameras().set_accessibility(accessibility,&error))throw std::runtime_error(error);
        CameraFrameGraph graph;std::uint64_t resetFrames=0,eventCount=0;float previousTime=0;
        for(int frame=0;frame<120;++frame){world.tick(1.0F/30.0F);graph.begin_frame(1280,720,{0,0,0});for(auto id:world.cameras().viewport_ids()){const auto* view=world.cameras().frame(id);if(!view||!graph.submit(*view,id==3?512U:0U,id==3?288U:0U,&error))throw std::runtime_error(error.empty()?"camera frame submission failed":error);eventCount+=view->triggeredEvents.size();}const auto plan=graph.finalize();for(const auto&v:plan.viewports)if(v.history.resetTaa)++resetFrames;previousTime=world.cameras().sequence_time(1);}
        const auto* gameplay=world.cameras().frame(1);const auto* director=world.cameras().frame(2);if(!gameplay||!director)throw std::runtime_error("missing camera output");
        CameraFloatImage left=make_scene(640,720,false),right=make_scene(640,720,true);
        const auto leftDof=apply_camera_depth_of_field(left,gameplay->pose,gameplay->postProcess,{CameraDofQuality::High,14,0.12F,false,true});
        CameraPostProcessProfile previewPost=director->postProcess;previewPost.depthOfFieldWeight=1;previewPost.focusDistanceMeters=7;previewPost.apertureFStop=2.0F;
        const auto rightDof=apply_camera_depth_of_field(right,director->pose,previewPost,{CameraDofQuality::Medium,10,0.15F,true,true});
        CameraFloatImage composite;composite.width=1280;composite.height=720;composite.rgba.assign(1280U*720U*4U,0);composite.linearDepthMeters.assign(1280U*720U,INFINITY);
        if(!composite_camera_viewport(composite,left,{1,"Left",1,0,0,0.5F,1,"main",true},&error)||!composite_camera_viewport(composite,right,{2,"Right",2,0.5F,0,0.5F,1,"main",true},&error))throw std::runtime_error(error);
        write_ppm(output/"camera_render_v1_37.ppm",composite);
        std::ofstream seq(output/"constant_speed_reveal.dvecamseq");seq<<sequence.serialize();
        std::ofstream state(output/"camera_runtime_v1_37.state");state<<world.cameras().serialize_state();
        std::ofstream json(output/"camera_render_evidence_v1_37.json");json<<std::fixed<<std::setprecision(5)<<"{\n  \"version\": \"1.37\",\n  \"viewports\": 3,\n  \"render_targets\": [\"main\", \"security\"],\n  \"sequence_time\": "<<previousTime<<",\n  \"sequence_events\": "<<eventCount<<",\n  \"temporal_reset_viewport_frames\": "<<resetFrames<<",\n  \"constant_speed_dolly\": true,\n  \"left_blurred_pixels\": "<<leftDof.blurredPixels<<",\n  \"right_blurred_pixels\": "<<rightDof.blurredPixels<<",\n  \"photosensitivity_contract\": true,\n  \"camera_library_format\": 3\n}\n";
        std::cout<<"Wrote v1.37 camera evidence to "<<output<<'\n';return 0;
    }catch(const std::exception&e){std::cerr<<"camera_render_v137_demo: "<<e.what()<<'\n';return 1;}
}
