#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "dve/camera_runtime.hpp"
#include "dve/game_world.hpp"
#include "dve/camera_sequence.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace {
using namespace dve;
using namespace dve::camera;
void require(bool c,const char*m){if(!c)throw std::runtime_error(m);} bool close(float a,float b,float e=1e-4F){return std::abs(a-b)<=e;}

CameraRig make_follow(CameraRigId id,std::uint32_t channels,CameraTargetId target){CameraRig r;r.id=id;r.name="Follow";r.mode=CameraRigMode::ThirdPerson;r.priority=10;r.outputChannels=channels;r.followTarget=target;r.lookAtTarget=target;r.framing.distanceMeters=4.0F;r.framing.localOffset={0,1,0};r.collision.enabled=false;return r;}

void test_runtime_ownership_split_screen_and_gpu_packets(){
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc marker;marker.name="player";marker.transform.position={2,1,-3};const GameObjectId player=world.create_object(std::move(marker));require(player!=0,"marker creation failed");
    std::string error;
    CameraViewportDesc left{1,"Player One",1,0,0,0.5F,1,"main",true};
    CameraViewportDesc right{2,"Player Two",2,0.5F,0,0.5F,1,"main",true};
    require(world.cameras().add_viewport(left,&error),error.c_str());require(world.cameras().add_viewport(right,&error),error.c_str());
    require(world.cameras().director(1)->add_or_replace_rig(make_follow(10,1,100),&error),error.c_str());
    require(world.cameras().director(2)->add_or_replace_rig(make_follow(20,2,200),&error),error.c_str());
    require(world.cameras().bind_target(1,100,player),"left target bind failed");require(world.cameras().bind_target(2,200,player),"right target bind failed");
    CameraPostProcessProfile pp;pp.exposure=1.5F;pp.depthOfFieldWeight=0.7F;require(world.cameras().set_post_process(1,10,pp,&error),error.c_str());
    world.tick(1.0F/60.0F);
    const auto*l=world.cameras().frame(1);const auto*r=world.cameras().frame(2);require(l&&r,"split-screen frames missing");
    require(close(l->gpu.viewportRect[2],0.5F)&&close(r->gpu.viewportRect[0],0.5F),"split-screen rectangles incorrect");
    require(l->gpu.outputChannelMask==1U&&r->gpu.outputChannelMask==2U,"camera channels incorrect");
    require(close(l->gpu.cameraPosition[3],1.0F)&&close(l->gpu.postProcess0[0],1.5F),"GPU packet fields incorrect");
    require(std::isfinite(l->gpu.viewProjection[0]),"camera matrix is not finite");
}

void test_collision_adapter(){
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    GameObjectDesc box;box.name="wall";box.transform.position={0,0,-2};box.dynamic=false;box.voxelSizeMeters=1.0F;box.voxels=std::make_unique<VoxelObject>();box.voxels->set_voxel({0,0,0},7);std::string error;const GameObjectId wallId=world.create_object(std::move(box),&error);require(wallId!=0,error.c_str());
    GameWorldCameraCollisionWorld collision(world);CameraCollisionHit hit;require(collision.sweep_sphere({0.5F,0.5F,0},{0.5F,0.5F,-5},0.2F,hit),"camera sweep did not hit voxel wall");require(hit.fraction>0.1F&&hit.fraction<0.8F,"camera sweep fraction implausible");require(hit.objectId==wallId&&hit.materialId==7U,"camera collision adapter lost object/material identity");
}

void test_temporal_cut_generation(){
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());std::string error;require(world.cameras().add_viewport({1,"Main",1,0,0,1,1,"main",true},&error),error.c_str());
    CameraRig a;a.id=1;a.name="A";a.priority=10;a.authoredPose.position={0,0,5};a.authoredPose.target={0,0,0};a.collision.enabled=false;
    CameraRig b=a;b.id=2;b.name="B";b.priority=1;b.authoredPose.position={5,0,0};
    auto*d=world.cameras().director(1);require(d->add_or_replace_rig(a,&error),error.c_str());require(d->add_or_replace_rig(b,&error),error.c_str());world.tick(0.016F);const auto first=world.cameras().frame(1)->gpu.cameraCutGeneration;
    require(d->force_live(2,true),"force live failed");world.tick(0.016F);const auto*frame=world.cameras().frame(1);require(frame->gpu.cameraCutGeneration>first,"camera cut generation did not advance");require(frame->gpu.resetTemporalHistory==1U,"camera cut did not request temporal reset");
}

void test_sequence_runtime_dof_guides_and_culling(){
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());std::string error;require(world.cameras().add_viewport({1,"Cine",1,0,0,1,1,"cinematic",true},&error),error.c_str());
    CameraRig fallback;fallback.id=1;fallback.name="Fallback";fallback.authoredPose.position={0,1,8};fallback.authoredPose.target={0,1,0};fallback.collision.enabled=false;require(world.cameras().director(1)->add_or_replace_rig(fallback,&error),error.c_str());
    CameraSequence sequence;sequence.name="Shot";sequence.durationSeconds=2;CameraDollySpline dolly;dolly.id=7;dolly.name="Push";CameraDollyKey a;a.timeSeconds=0;a.pose.position={0,1,8};a.pose.target={0,1,0};a.pose.lens.physical.enabled=true;a.pose.lens.physical.focalLengthMillimeters=35;a.pose.lens.physical.focusDistanceMeters=8;a.postProcess.exposure=1;CameraDollyKey b=a;b.timeSeconds=2;b.pose.position={0,1,4};b.pose.lens.physical.focalLengthMillimeters=70;b.pose.lens.physical.focusDistanceMeters=4;b.postProcess.exposure=2;dolly.keys={a,b};sequence.dollies.push_back(dolly);CameraShot shot;shot.id=1;shot.name="Push";shot.startSeconds=0;shot.durationSeconds=2;shot.dollyId=7;shot.blendIn={CameraBlendCurve::Cut,0};shot.marker="push";sequence.shots.push_back(shot);require(world.cameras().set_sequence(1,sequence,&error),error.c_str());require(world.cameras().play_sequence(1),"sequence did not play");world.tick(1.0F);const auto*frame=world.cameras().frame(1);require(frame&&frame->activeShotId==std::optional<std::uint64_t>(1),"runtime sequence shot missing");require(frame->pose.position.z<8.0F&&frame->pose.position.z>4.0F,"runtime sequence did not move camera");require(frame->activeMarker=="push","runtime sequence marker missing");
    const auto dof=camera_depth_of_field_range(frame->pose.lens.physical);require(dof.nearFocusMeters>0&&dof.farFocusMeters>dof.nearFocusMeters,"depth-of-field range invalid");const auto guides=camera_frame_guides(16.0F/9.0F,2.39F);require(guides.insetY>0.0F,"cinematic aspect guides incorrect");require(camera_sphere_visible(frame->pose,{0,1,0},0.5F),"visible target was culled");require(!camera_sphere_visible(frame->pose,{1000,1,0},0.5F),"off-frustum target was visible");require(camera_projected_sphere_radius_pixels(frame->pose,{0,1,0},0.5F,1080)>1.0F,"projected LOD size invalid");
}


void test_accessibility_profiles_and_state_persistence(){
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    std::string error;
    require(world.cameras().add_viewport({17,"Accessible",1,0,0,1,1,"main",true},&error),error.c_str());
    CameraRig rig;rig.id=170;rig.name="Accessible rig";rig.authoredPose.position={0,1,5};
    rig.authoredPose.target={0,1,0};rig.authoredPose.worldUp={0.2F,0.9F,0.1F};rig.collision.enabled=false;
    require(world.cameras().director(17)->add_or_replace_rig(rig,&error),error.c_str());
    CameraPostProcessProfile post;post.bloomIntensity=4.0F;post.depthOfFieldWeight=0.9F;post.vignette=0.8F;
    require(world.cameras().set_post_process(17,170,post,&error),error.c_str());
    CameraAccessibilitySettings settings;settings.preset=CameraAccessibilityPreset::Photosensitive;
    settings.horizonLock=true;settings.motionScale=0.5F;settings.shakeScale=1.0F;settings.bloomScale=1.0F;
    settings.maximumDepthOfFieldWeight=0.8F;
    require(world.cameras().set_accessibility(settings,&error),error.c_str());
    CameraShake shake;shake.id=99;shake.positionAmplitudeMeters={1,1,1};shake.durationSeconds=1;
    require(world.cameras().director(17)->start_shake(shake,&error),error.c_str());
    world.tick(0.25F);
    const auto* frame=world.cameras().frame(17);require(frame,"accessible camera frame missing");
    require(close(frame->pose.worldUp.x,0.0F)&&close(frame->pose.worldUp.y,1.0F)&&close(frame->pose.worldUp.z,0.0F),
            "horizon lock was not applied");
    require(frame->postProcess.bloomIntensity<=0.6001F,"photosensitivity profile did not cap bloom");
    require(frame->postProcess.depthOfFieldWeight<=0.2501F,"photosensitivity profile did not cap depth of field");
    require(frame->postProcess.vignette<=0.2001F,"photosensitivity profile did not cap vignette");
    const std::string state=world.cameras().serialize_state();
    CameraAccessibilitySettings standard;require(world.cameras().set_accessibility(standard,&error),error.c_str());
    require(world.cameras().restore_state(state,&error),error.c_str());
    require(world.cameras().accessibility().preset==CameraAccessibilityPreset::Photosensitive,
            "camera accessibility preset was not restored");
    require(world.cameras().accessibility().horizonLock,"camera horizon lock was not restored");
}

void test_post_process_stack_and_runtime_state(){
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());std::string error;require(world.cameras().add_viewport({9,"State",1,0,0,1,1,"state",true},&error),error.c_str());
    CameraRig a;a.id=91;a.name="A";a.priority=10;a.authoredPose.position={0,1,5};a.authoredPose.target={0,1,0};a.collision.enabled=false;CameraRig b=a;b.id=92;b.name="B";b.priority=1;b.authoredPose.position={4,1,5};
    auto*d=world.cameras().director(9);require(d->add_or_replace_rig(a,&error),error.c_str());require(d->add_or_replace_rig(b,&error),error.c_str());d->bind_state("alternate",92);
    CameraPostProcessProfile warm;warm.exposure=2.0F;warm.colorTint={1.0F,0.5F,0.25F};CameraPostProcessProfile vignette;vignette.vignette=0.8F;vignette.exposure=1.0F;
    CameraPostProcessStack stack;stack.layers={{"Warm",warm,0.5F,true,CameraPostAll},{"Vignette",vignette,1.0F,true,CameraPostVignette}};require(world.cameras().set_post_process_stack(9,91,stack,&error),error.c_str());world.tick(0.016F);const auto*frame=world.cameras().frame(9);require(frame&&frame->postProcess.vignette>0.79F&&frame->postProcess.colorTint.y<1.0F,"post-process stack did not evaluate");
    CameraSequence sequence;sequence.name="Save";sequence.durationSeconds=2;CameraShot shot;shot.id=1;shot.name="Rig";shot.startSeconds=0;shot.durationSeconds=2;shot.rigId=91;shot.blendIn={CameraBlendCurve::Cut,0};sequence.shots.push_back(shot);require(world.cameras().set_sequence(9,sequence,&error),error.c_str());require(world.cameras().play_sequence(9),"sequence play failed");world.tick(0.5F);const std::string saved=world.cameras().serialize_state();require(world.cameras().seek_sequence(9,1.75F),"sequence seek failed");require(d->force_live(92,true),"force alternate failed");require(world.cameras().restore_state(saved,&error),error.c_str());require(std::abs(world.cameras().sequence_time(9)-0.5F)<0.02F&&world.cameras().sequence_playing(9),"camera runtime save state did not restore playback");require(d->telemetry().liveRig==91||d->telemetry().liveRig==0,"camera runtime state restored an unexpected rig");
    std::string corrupt=saved+"trailing";require(!world.cameras().restore_state(corrupt,&error),"corrupt camera runtime state was accepted");
}
}
int main(){try{test_runtime_ownership_split_screen_and_gpu_packets();test_collision_adapter();test_temporal_cut_generation();test_sequence_runtime_dof_guides_and_culling();test_accessibility_profiles_and_state_persistence();test_post_process_stack_and_runtime_state();std::cout<<"dve_camera_runtime_tests: PASS\n";return 0;}catch(const std::exception&e){std::cerr<<"dve_camera_runtime_tests: FAIL: "<<e.what()<<'\n';return 1;}}
