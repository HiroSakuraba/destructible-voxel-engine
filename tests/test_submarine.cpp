#include "dve/submarine_dynamics.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
namespace {
void test(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void water(){
 OceanField ocean;test(ocean.set_preset(OceanPreset::Breeze,{.6F,.2F,0}),"ocean preset");
 WaterVolumeSettings cfg;cfg.floorZ=-40;
 const auto surface=sample_water_volume(ocean,cfg,{0,0,-.1F},2);
 const auto deep=sample_water_volume(ocean,cfg,{0,0,-25.F},2);
 test(deep.depthBelowSurface>20&&deep.depthBelowSurface<31,"submerged depth from actual ocean");
 test(deep.pressurePascal>surface.pressurePascal+190000.F,"hydrostatic pressure with depth");
 test(std::abs(deep.flowVelocity.z)<std::abs(surface.flowVelocity.z)+.001F,
      "orbital wave motion attenuates underwater");
 test(deep.absorptionRGB.x>deep.absorptionRGB.z,"red light absorbed more than blue");
 test(deep.diffuseKdRGB.x>deep.diffuseKdRGB.z,"optical S2 sunlight interface");
 WaterVolumeSettings turbid=cfg;turbid.clarity=WaterClarity::Turbid;
 const auto t=sample_water_volume(ocean,turbid,{0,0,-20},2);
 test(t.scatteringRGB.x>deep.scatteringRGB.x&&t.turbidity>deep.turbidity,
      "water clarity profiles distinguish seawater scattering");
 WaterVolumeSettings invalid=cfg;invalid.floorZ=30;
 test(!valid_water_volume_settings(invalid),"reject nonphysical seafloor configuration");
}
void submarine(){
 OceanField sea;test(sea.set_preset(OceanPreset::Calm),"calm");
 WaterVolumeSettings vol;SubmarineConfig cfg;
 SubmarineState neutral;neutral.ballastKg=cfg.displacementM3*1025.F-cfg.dryMassKg;
 WaterRigidState pose;pose.position={0,0,-12};
 auto sample=sample_water_volume(sea,vol,pose.position,0);
 auto loads=evaluate_submarine(cfg,neutral,pose,sample,{},1.F/120.F);
 test(std::abs(loads.forceNewton.z)<1.0F,"neutral displacement cancels weight underwater");
 test(loads.submergedFraction==1.F,"fully submerged gives full displacement buoyancy");
 SubmarineState heavy=neutral;heavy.ballastKg+=90.F;
 const auto sink=evaluate_submarine(cfg,heavy,pose,sample,{},1.F/120.F);
 test(sink.forceNewton.z<-500.F,"flooding ballast tank causes descent");
 SubmarineState light=neutral;light.ballastKg-=50.F;
 const auto rise=evaluate_submarine(cfg,light,pose,sample,{},1.F/120.F);
 test(rise.forceNewton.z>300.F,"empty ballast causes ascent");
 SubmarineState pumping=neutral;const float start=pumping.ballastKg;
 (void)evaluate_submarine(cfg,pumping,pose,sample,SubmarineInput{0,0,0,0,0,1},1.F/120.F);
 test(pumping.ballastKg>start,"flood command fills at bounded pump rate");
 SubmarineState hold=neutral;hold.depthHold=true;hold.targetDepthM=10.F;
 const auto controller=evaluate_submarine(cfg,hold,pose,sample,{},1.F/120.F);
 test(controller.verticalThrust>0,"hold controller pushes up when too deep");
 hold.targetDepthM=20.F;
 const auto descend=evaluate_submarine(cfg,hold,pose,sample,{},1.F/120.F);
 test(descend.verticalThrust<0,"hold controller pushes down when too shallow");
 SubmarineState powered=neutral;const auto forward=evaluate_submarine(cfg,powered,pose,sample,
      SubmarineInput{1,0,0,0,0,0},1.F/120.F);
 test(forward.forceNewton.y>1000.F,"underwater forward thruster produces forward force");
 const auto initial=pose.position;
 for(unsigned i=0;i<240;++i){
    sample=sample_water_volume(sea,vol,pose.position,double(i)/120.F);
    loads=evaluate_submarine(cfg,powered,pose,sample,SubmarineInput{1,0,0,0,0,0},1.F/120.F);
    step_submarine(pose,cfg,powered,loads,1.F/120.F);
 }
 test(pose.position.y>initial.y+.2F&&std::isfinite(pose.position.z),
      "fixed-step underwater acceleration produces stable translation");
 test(powered.battery<neutral.battery,"thrusters consume battery");
 const auto dry=sample_water_volume(sea,vol,{0,0,3},4);
 SubmarineState zero=neutral;
 const auto above=evaluate_submarine(cfg,zero,WaterRigidState{{0,0,3}},dry,{},1.F/120.F);
 test(above.submergedFraction==0,"submarine above calm ocean receives no underwater buoyancy");
 const auto rated=sample_water_volume(sea,vol,{0,0,-100},4);
 test(evaluate_submarine(cfg,zero,WaterRigidState{{0,0,-100}},rated,{},1.F/120.F).ratedDepthExceeded,
      "rated operating depth warning");
}
}
int main(){
 try{water();submarine();std::cout<<"S1 seawater volume and submersible dynamics passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"S1: "<<e.what()<<'\n';return 1;}
}
