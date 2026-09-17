#include <cmath>
#include <iostream>
#include <stdexcept>

#include "dve/camera_sequence.hpp"

namespace { using namespace dve; using namespace dve::camera;
void require(bool c,const char*m){if(!c)throw std::runtime_error(m);}bool close(float a,float b,float e=1e-3F){return std::abs(a-b)<=e;}
CameraDollyKey key(float t,Float3 p,float focal,float exposure){CameraDollyKey k;k.timeSeconds=t;k.pose.position=p;k.pose.target={0,1,0};k.pose.lens.physical.enabled=true;k.pose.lens.physical.focalLengthMillimeters=focal;k.pose.lens.physical.focusDistanceMeters=5+t;k.postProcess.exposure=exposure;k.postProcess.focusDistanceMeters=5+t;return k;}
CameraSequence make_sequence(){CameraSequence q;q.name="Opening";q.durationSeconds=6;CameraDollySpline d;d.id=1;d.name="Crane";d.keys={key(0,{0,1,6},35,1),key(2,{2,3,4},50,1.5F),key(4,{0,5,2},70,2)};q.dollies.push_back(d);CameraShot s;s.id=10;s.name="Crane In";s.startSeconds=0;s.durationSeconds=4;s.dollyId=1;s.blendIn={CameraBlendCurve::Cut,0};s.marker="intro";s.stateTrigger="cinematic";q.shots.push_back(s);CameraShot s2;s2.id=11;s2.name="Gameplay";s2.startSeconds=4;s2.durationSeconds=2;s2.rigId=99;s2.blendIn={CameraBlendCurve::EaseInOut,0.5F};q.shots.push_back(s2);return q;}
void test_deterministic_spline_and_round_trip(){auto q=make_sequence();std::string error;require(q.validate(&error),error.c_str());auto a=q.evaluate(2.0F),b=q.evaluate(2.0F);require(a.pose&&b.pose,"dolly pose missing");require(close(a.pose->position.x,b.pose->position.x)&&close(a.postProcess.exposure,1.5F),"dolly evaluation not deterministic");auto parsed=CameraSequence::parse(q.serialize(),&error);require(parsed.has_value(),error.c_str());auto p=parsed->evaluate(2.0F);require(p.pose&&close(p.pose->lens.physical.focalLengthMillimeters,50.0F),"sequence round trip changed lens");require(p.marker=="intro"&&p.stateTrigger=="cinematic","sequence metadata lost");}
void test_player_scrub_loop_and_shot_arbitration(){auto q=make_sequence();CameraSequencePlayer player;player.set_sequence(&q);player.seek(4.5F);auto s=player.update(0);require(s.rigId==std::optional<CameraRigId>(99),"rig shot arbitration failed");player.seek(5.9F);player.play(true);s=player.update(0.3F);require(player.time_seconds()<1.0F&&s.pose.has_value(),"loop playback failed");player.pause();const float frozen=player.time_seconds();const auto pausedSample=player.update(1.0F);(void)pausedSample;require(close(player.time_seconds(),frozen),"paused sequence advanced");}
void test_validation_rejects_overlap(){auto q=make_sequence();q.shots[1].startSeconds=3.5F;std::string error;require(!q.validate(&error),"overlapping shots were accepted");}
void test_constant_speed_events_and_metadata(){
    CameraSequence q;q.name="Constant";q.durationSeconds=10;CameraDollySpline d;d.id=2;d.name="Uneven";d.arcLengthSamplesPerSegment=64;
    d.keys={key(0,{0,0,0},35,1),key(1,{9,0,0},35,1),key(10,{10,0,0},35,1)};q.dollies.push_back(d);
    CameraShot s;s.id=20;s.name="Constant move";s.startSeconds=0;s.durationSeconds=10;s.dollyId=2;s.constantSpeedDolly=true;s.blendIn={CameraBlendCurve::Cut,0};q.shots.push_back(s);
    q.events={{1,2.0F,"footstep","left"},{2,8.0F,"impact","heavy"}};q.metadata["vendor.extension"]="preserve me";
    std::string error;require(q.validate(&error),error.c_str());
    const auto quarter=q.evaluate(2.5F),half=q.evaluate(5.0F),threeQuarter=q.evaluate(7.5F);require(quarter.pose&&half.pose&&threeQuarter.pose,"constant-speed poses missing");
    const float a=length(subtract(half.pose->position,quarter.pose->position));const float b=length(subtract(threeQuarter.pose->position,half.pose->position));require(std::abs(a-b)<0.6F,"constant-speed spline has strongly uneven quarter distances");
    const auto parsed=CameraSequence::parse(q.serialize(),&error);require(parsed.has_value(),error.c_str());require(parsed->shots[0].constantSpeedDolly&&parsed->metadata.at("vendor.extension")=="preserve me"&&parsed->events.size()==2,"sequence v2 round trip lost constant speed, metadata, or events");
    CameraSequencePlayer player;player.set_sequence(&*parsed);player.play(false);auto sample=player.update(2.1F);require(sample.triggeredEvents.size()==1&&sample.triggeredEvents[0].name=="footstep","sequence event did not fire when crossed");sample=player.update(6.0F);require(sample.triggeredEvents.size()==1&&sample.triggeredEvents[0].name=="impact","second sequence event did not fire");
    player.seek(9.5F);player.play(true);sample=player.update(1.0F);require(player.time_seconds()<1.0F,"looping event test did not wrap");
}
}
int main(){try{test_deterministic_spline_and_round_trip();test_player_scrub_loop_and_shot_arbitration();test_validation_rejects_overlap();test_constant_speed_events_and_metadata();std::cout<<"dve_camera_sequence_tests: PASS\n";return 0;}catch(const std::exception&e){std::cerr<<"dve_camera_sequence_tests: FAIL: "<<e.what()<<'\n';return 1;}}
