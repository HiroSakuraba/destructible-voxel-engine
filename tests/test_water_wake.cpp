#include "dve/water_ocean.hpp"
#include "dve/water_hydrodynamics.hpp"
#include "dve/water_wake.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace dve;
namespace {
void check(bool yes,const char* what){if(!yes)throw std::runtime_error(what);}
WaterWakeEvent event(){
 WaterWakeEvent e;
 e.position={0,0};e.direction={0,1};
 e.bornSeconds=1;e.amplitude=.8F;e.wavelength=4.0F;
 e.width=2.0F;e.speed=2.5F;e.lifetime=5.0F;
 return e;
}
void mathematics(){
 OceanField ocean;check(ocean.set_preset(OceanPreset::Calm),"calm ocean");
 const auto e=event();
 check(ocean.emit_wake(e),"accept bounded wake");
 const auto a=ocean.sample(0,-1.1F,1.4),b=ocean.sample(0,-1.1F,1.4);
 check(a.height==b.height&&a.waterVelocity.z==b.waterVelocity.z&&a.normal.x==b.normal.x,
       "analytic local wake is repeatable");
 check(std::abs(a.height)>.02F&&a.foam>0,"wake changes ocean height and foam");
 check(std::abs(a.waterVelocity.z)>.01F,"wake carries nonzero vertical fluid velocity");
 check(std::abs(a.normal.z-1.F)>.0001F,"wake affects sea-surface normal");
 const auto far=ocean.sample(400,400,1.4);
 check(std::abs(far.height)<.0001F&&far.foam<.0001F,"wake disturbance has compact support");
 check(ocean.sample(0,-1.1F,20).height==0,"expired wake produces no force");
 ocean.prune_wakes(20);
 check(ocean.wakes().empty(),"expired wake events pruned");
}
void physics(){
 OceanField ocean;check(ocean.set_preset(OceanPreset::Calm),"calm");
 WaterHull h;h.massKg=480;h.displacementCubicMetres=.56F;
 WaterRigidState boat;boat.position={0,-1.1F,.12F};
 const auto flat=evaluate_water_hull(ocean,1.4,h,boat);
 const auto e=event();check(ocean.emit_wake(e),"wake emit");
 const auto disturbed=evaluate_water_hull(ocean,1.4,h,boat);
 check(std::abs(disturbed.forceNewton.z-flat.forceNewton.z)>1.F||
       std::abs(disturbed.torqueNewtonMetres.x-flat.torqueNewtonMetres.x)>1.F,
       "real buoyancy probes respond to another vessel's wake");
 const auto a=ocean.sample(0,-1.1F,1.4);
 check(std::abs(a.height)>.02F,"physics probes use the same disturbed ocean as rendering");
}
void safety(){
 WaterWakeField f;auto e=event();
 e.amplitude=-2;
 check(!f.emit(e),"reject invalid negative height");
 e=event();
 for(unsigned i=0;i<200;++i){e.bornSeconds=double(i)*.01;check(f.emit(e),"valid bounded emit");}
 check(f.events().size()==WaterWakeField::MaxEvents,"bounded event capacity");
 const std::vector<WaterWakeEvent> snapshot(f.events().begin(),f.events().end());
 WaterWakeField restored;
 check(restored.restore(snapshot,5.0),"snapshot reload exact active events");
 check(restored.events().size()==snapshot.size(),"restore exact wake count");
 check(restored.sample(0,-1.1F,5).height==f.sample(0,-1.1F,5).height,
       "wake snapshots reproduce identical physical surface");
 auto corrupted=snapshot;corrupted[0].direction={2,3};
 check(!restored.restore(corrupted,5),"reject corrupt event before mutating wake state");
 check(restored.events().size()==snapshot.size(),"restore remains transactional on failure");
}
}
int main(){
 try{mathematics();physics();safety();
     std::cout<<"W3 water disturbance, ship-to-ship buoyancy and snapshot tests passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"W3 wakes: "<<e.what()<<'\n';return 1;}
}
