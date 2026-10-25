#include "dve/water_ocean.hpp"
#include "dve/water_hydrodynamics.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void waves(){
 OceanField ocean;
 for(const auto p:{OceanPreset::Calm,OceanPreset::Breeze,OceanPreset::Storm}){
  check(ocean.set_preset(p),"wave preset accepted");
  const auto a=ocean.sample(5,11,12.25),b=ocean.sample(5,11,12.25);
  check(a.height==b.height&&a.normal.x==b.normal.x,"pure repeatable ocean");
  check(std::isfinite(a.height)&&std::isfinite(a.normal.z)&&std::isfinite(a.waterVelocity.z),
        "all wave query channels finite");
  check(a.normal.z>.3F&&std::abs(length(a.normal)-1)<.001F,"wave normals normalized and upward");
  check(a.foam>=0&&a.foam<=1,"foam in unit range");
  check(std::abs(a.height)<=ocean.maximum_wave_height()+.01F,"height bounded by wave envelope");
  if(p==OceanPreset::Calm)check(a.height==0&&ocean.waves().empty(),"calm preset flat");
  else check(ocean.waves().size()>=5,"wind wave components present");
 }
 check(ocean.set_preset(OceanPreset::Calm,{2,3,0}),"current configurable");
 check(ocean.sample(1,2,3).waterVelocity.x==2,"calm surface carries current");
 const auto saved=ocean.sample(10,12,4.5);
 const std::array<OceanWave,1> invalid{{{{1,1},1,10,0,.4F}}};
 check(!ocean.set_waves(invalid),"non-unit wave heading rejected");
 check(ocean.sample(10,12,4.5).height==saved.height,"invalid edits transactional");
 const std::array<OceanWave,1> single{{{{0,1},.35F,16,0,.4F}}};
 check(ocean.set_waves(single),"custom artist wave accepted");
 const auto sample=ocean.sample(5,10,.25);
 check(std::isfinite(sample.height)&&sample.normal.z>0,"custom wave world-space inverted");
}
void buoyancy(){
 OceanField ocean;check(ocean.set_preset(OceanPreset::Calm),"calm");
 WaterHull small;small.massKg=480;small.displacementCubicMetres=.56F;
 small.lengthMetres=4.3F;small.beamMetres=1.8F;small.draftMetres=.8F;
 WaterHull large;large.massKg=24000;large.displacementCubicMetres=27;
 large.lengthMetres=26;large.beamMetres=7;large.draftMetres=3.2F;
 large.linearDamping=6500;large.angularDamping=85000;
 large.longitudinalProbes=9;large.lateralProbes=5;
 check(valid_water_hull(small)&&valid_water_hull(large),"valid hull presets");
 WaterHull bad=large;bad.longitudinalProbes=200;
 check(!valid_water_hull(bad),"reject runaway probe count");
 for(const auto& hull:{small,large}){
  WaterRigidState high;high.position={0,0,10};
  const auto dry=evaluate_water_hull(ocean,0,hull,high);
  check(dry.immersedProbes==0&&dry.forceNewton.z==0,"no lift above sea");
  WaterRigidState submerged;submerged.position={0,0,-5};
  const auto wet=evaluate_water_hull(ocean,0,hull,submerged);
  check(wet.immersedProbes==hull.longitudinalProbes*hull.lateralProbes,"fully submerged buoyancy");
  check(wet.forceNewton.z>hull.massKg*9.81F,"submerged hull lifts more than weight");
  WaterRigidState s;s.position={0,0,hull.draftMetres*.20F};
  for(unsigned i=0;i<600;++i){
   auto f=evaluate_water_hull(ocean,double(i)/120.,hull,s);
   step_water_hull(s,hull,f,1.F/120.F);
  }
  check(std::isfinite(s.position.z)&&std::abs(s.position.z)<5.F,"hull settles without numerical explosion");
  check(std::isfinite(s.orientation.w)&&std::abs(s.orientation.w)<=1.01F,"hull attitude remains finite");
 }
 check(ocean.set_preset(OceanPreset::Storm),"storm");
 WaterRigidState boat;boat.position={0,0,.2F};
 auto a=evaluate_water_hull(ocean,0,small,boat);
 auto b=evaluate_water_hull(ocean,3,small,boat);
 check(std::abs(a.meanWaterHeight-b.meanWaterHeight)>.001F,
       "physics probes observe time-varying water rather than a flat collision plane");
}
}
int main(){
 try{waves();buoyancy();std::cout<<"W1 ocean field and multi-probe hull tests passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"W1 ocean: "<<e.what()<<'\n';return 1;}
}
