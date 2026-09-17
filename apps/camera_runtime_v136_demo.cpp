#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "dve/camera_runtime.hpp"
#include "dve/camera_sequence.hpp"
#include "dve/editor_camera_sequencer.hpp"
#include "dve/editor_settings.hpp"
#include "dve/game_world.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace {
using namespace dve;
using namespace dve::camera;
using namespace dve::editor;

struct Sample { float time{}; Float3 position{}; float focal{}; float exposure{}; float nearFocus{}; float farFocus{}; std::uint64_t shot{}; std::uint32_t cutGeneration{}; };

CameraDollyKey make_key(float time, Float3 position, Float3 target, float focal, float focus, float aperture, float exposure, Float3 tint) {
    CameraDollyKey key; key.timeSeconds=time; key.pose.position=position; key.pose.target=target;
    key.pose.lens.physical.enabled=true; key.pose.lens.physical.focalLengthMillimeters=focal;
    key.pose.lens.physical.focusDistanceMeters=focus; key.pose.lens.physical.apertureFStop=aperture;
    key.postProcess.exposure=exposure; key.postProcess.focusDistanceMeters=focus; key.postProcess.apertureFStop=aperture; key.postProcess.colorTint=tint;
    key.postProcess.depthOfFieldWeight=1.0F; key.postProcess.bloomIntensity=0.8F; return key;
}

CameraSequence build_sequence() {
    CameraSequence q; q.name="Foundry Reveal"; q.durationSeconds=8.0F;
    CameraDollySpline crane; crane.id=1; crane.name="Crane Reveal";
    crane.keys={make_key(0,{0,2,12},{0,1,0},28,12,5.6F,0.9F,{0.85F,0.92F,1.0F}),
                make_key(2,{5,5,8},{0,1,0},40,9,4.0F,1.1F,{1.0F,0.92F,0.82F}),
                make_key(4,{2,3,5},{0,1,0},55,5.5F,2.8F,1.3F,{1.0F,0.82F,0.72F})};
    CameraDollySpline orbit; orbit.id=2; orbit.name="Hero Orbit";
    orbit.keys={make_key(0,{2,3,5},{0,1,0},55,5.5F,2.8F,1.3F,{1.0F,0.82F,0.72F}),
                make_key(2,{-4,2.5F,4},{0,1,0},70,4.8F,2.0F,1.45F,{1.0F,0.75F,0.62F}),
                make_key(4,{-2,1.8F,7},{0,1,0},46,7.0F,3.5F,1.15F,{0.88F,0.90F,1.0F})};
    q.dollies={crane,orbit};
    CameraShot a; a.id=10;a.name="Establishing Crane";a.startSeconds=0;a.durationSeconds=4;a.dollyId=1;a.blendIn={CameraBlendCurve::Cut,0};a.marker="establish";a.stateTrigger="cinematic";
    CameraShot b; b.id=20;b.name="Hero Orbit";b.startSeconds=4;b.durationSeconds=4;b.dollyId=2;b.blendIn={CameraBlendCurve::EaseInOut,0.75F};b.marker="hero";
    q.shots={a,b}; return q;
}

void pixel(std::vector<unsigned char>& image,int w,int h,int x,int y,unsigned char r,unsigned char g,unsigned char b) {
    if(x<0||y<0||x>=w||y>=h)return;const std::size_t i=static_cast<std::size_t>((y*w+x)*3);image[i]=r;image[i+1]=g;image[i+2]=b;
}
void line(std::vector<unsigned char>& image,int w,int h,int x0,int y0,int x1,int y1,unsigned char r,unsigned char g,unsigned char b){int dx=std::abs(x1-x0),sx=x0<x1?1:-1,dy=-std::abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;for(;;){pixel(image,w,h,x0,y0,r,g,b);if(x0==x1&&y0==y1)break;const int e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}}
void rect(std::vector<unsigned char>& image,int w,int h,int x,int y,int rw,int rh,unsigned char r,unsigned char g,unsigned char b){line(image,w,h,x,y,x+rw,y,r,g,b);line(image,w,h,x+rw,y,x+rw,y+rh,r,g,b);line(image,w,h,x+rw,y+rh,x,y+rh,r,g,b);line(image,w,h,x,y+rh,x,y,r,g,b);}

} // namespace

int main(int argc,char** argv){
    try {
        const std::filesystem::path output=argc>1?argv[1]:"camera_runtime_v1_36_demo";std::filesystem::create_directories(output);
        CameraSequence sequence=build_sequence();std::string error;if(!sequence.validate(&error))throw std::runtime_error(error);
        GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
        if(!world.cameras().add_viewport({1,"Cinematic",1,0,0,0.72F,1,"main",true},&error))throw std::runtime_error(error);
        if(!world.cameras().add_viewport({2,"Director Preview",2,0.72F,0,0.28F,1,"camera_preview",true},&error))throw std::runtime_error(error);
        CameraRig preview;preview.id=99;preview.name="Director Overview";preview.priority=1;preview.outputChannels=2;preview.authoredPose.position={10,9,14};preview.authoredPose.target={0,1,0};preview.collision.enabled=false;
        if(!world.cameras().director(2)->add_or_replace_rig(preview,&error))throw std::runtime_error(error);
        if(!world.cameras().set_sequence(1,sequence,&error)||!world.cameras().play_sequence(1))throw std::runtime_error(error.empty()?"sequence setup failed":error);
        std::vector<Sample> samples;constexpr float dt=1.0F/30.0F;for(int frame=0;frame<=240;++frame){world.tick(dt);const auto* view=world.cameras().frame(1);if(!view)throw std::runtime_error("missing cinematic frame");const auto dof=camera_depth_of_field_range(view->pose.lens.physical);samples.push_back({frame*dt,view->pose.position,view->pose.lens.physical.focalLengthMillimeters,view->postProcess.exposure,dof.nearFocusMeters,dof.farFocusMeters,view->activeShotId.value_or(0),view->gpu.cameraCutGeneration});}
        std::ofstream seqFile(output/"foundry_reveal.dvecamseq");seqFile<<sequence.serialize();
        EditorCameraSequencer editor;if(!editor.set_sequence(sequence,&error))throw std::runtime_error(error);std::ofstream otio(output/"foundry_reveal.otio.json");otio<<editor.export_otio_json();
        EditorSettingsRegistry settings=EditorSettingsRegistry::make_default();
        if(!settings.set(SettingScope::Project,"camera.physical_lens",true,&error) ||
           !settings.set(SettingScope::Project,"camera.depth_of_field",true,&error) ||
           !settings.set(SettingScope::Project,"camera.split_screen_layout",std::string("vertical"),&error) ||
           !settings.set(SettingScope::Project,"camera.temporal_reset_on_cut",true,&error)) throw std::runtime_error(error);
        std::ofstream profile(output/"cinematic_camera_profile.dvesettingsprofile");profile<<settings.serialize_profile("Cinematic Authoring",SettingScope::Project);
        std::ofstream csv(output/"camera_runtime_samples.csv");csv<<"time,shot,x,y,z,focal_mm,exposure,near_focus,far_focus,cut_generation\n";for(const auto&s:samples)csv<<std::fixed<<std::setprecision(5)<<s.time<<','<<s.shot<<','<<s.position.x<<','<<s.position.y<<','<<s.position.z<<','<<s.focal<<','<<s.exposure<<','<<s.nearFocus<<','<<s.farFocus<<','<<s.cutGeneration<<'\n';
        const int width=1200,height=720;std::vector<unsigned char> image(static_cast<std::size_t>(width*height*3),18);for(int x=0;x<width;x+=50)line(image,width,height,x,0,x,height-1,30,34,42);for(int y=0;y<height;y+=50)line(image,width,height,0,y,width-1,y,30,34,42);
        rect(image,width,height,35,35,810,570,76,92,112);rect(image,width,height,870,35,295,570,76,92,112);rect(image,width,height,75,75,730,490,135,160,190);
        const auto guides=camera_frame_guides(16.0F/9.0F,2.39F);const int insetY=static_cast<int>(guides.insetY*490);rect(image,width,height,75,75+insetY,730,490-2*insetY,220,176,86);
        auto mapx=[](float x){return 440+static_cast<int>(x*45);};auto mapy=[](float z){return 500-static_cast<int>(z*32);};for(std::size_t i=1;i<samples.size();++i){const auto&a=samples[i-1],&b=samples[i];const bool second=b.shot==20;line(image,width,height,mapx(a.position.x),mapy(a.position.z),mapx(b.position.x),mapy(b.position.z),second?230:80,second?100:190,second?82:240);}for(const auto&s:samples)if(std::fmod(s.time,1.0F)<dt)rect(image,width,height,mapx(s.position.x)-3,mapy(s.position.z)-3,6,6,245,245,245);
        const int timelineX=60,timelineY=635,timelineW=1080,timelineH=46;rect(image,width,height,timelineX,timelineY,timelineW,timelineH,90,106,128);for(const auto&shot:sequence.shots){const int x=timelineX+static_cast<int>(shot.startSeconds/sequence.durationSeconds*timelineW);const int rw=static_cast<int>(shot.durationSeconds/sequence.durationSeconds*timelineW);rect(image,width,height,x,timelineY+4,rw,timelineH-8,shot.id==10?75:210,shot.id==10?160:92,shot.id==10?225:76);}for(int second=0;second<=8;++second){const int x=timelineX+second*timelineW/8;line(image,width,height,x,timelineY-7,x,timelineY+timelineH+7,120,132,150);}
        std::ofstream ppm(output/"camera_runtime_v1_36.ppm",std::ios::binary);ppm<<"P6\n"<<width<<' '<<height<<"\n255\n";ppm.write(reinterpret_cast<const char*>(image.data()),static_cast<std::streamsize>(image.size()));
        std::ofstream json(output/"camera_runtime_evidence_v1_36.json");const auto* finalFrame=world.cameras().frame(1);json<<"{\n  \"sequence\": \"Foundry Reveal\",\n  \"duration_seconds\": 8.0,\n  \"sample_count\": "<<samples.size()<<",\n  \"viewport_count\": "<<world.cameras().viewport_ids().size()<<",\n  \"shot_count\": "<<sequence.shots.size()<<",\n  \"dolly_count\": "<<sequence.dollies.size()<<",\n  \"final_cut_generation\": "<<(finalFrame?finalFrame->gpu.cameraCutGeneration:0)<<",\n  \"split_screen_channels\": [1, 2],\n  \"render_targets\": [\"main\", \"camera_preview\"],\n  \"temporal_reset_on_cut\": true,\n  \"physical_depth_of_field\": true\n}\n";
        std::cout<<"Wrote camera runtime v1.36 evidence to "<<output<<'\n';return 0;
    } catch(const std::exception& e){std::cerr<<"camera_runtime_v136_demo: "<<e.what()<<'\n';return 1;}
}
