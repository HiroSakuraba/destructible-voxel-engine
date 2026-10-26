#include "dve/water_ocean.hpp"
#include "dve/water_surf.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
namespace {
void check(bool p,const char* message){if(!p)throw std::runtime_error(message);}
WaterCoast beach(){
 WaterCoast c;c.shoreline={0,-240};c.halfWidth=150;
 c.offshoreLength=240;c.slope=.05F;c.swellHeight=1.8F;
 c.periodSeconds=9;c.peel=.08F;return c;
}
void coast_queries(){
 auto c=beach();
 check(valid_water_coast(c),"editable nearshore bathymetry valid");
 auto bad=c;bad.slope=-2.F;check(!valid_water_coast(bad),"invalid bathymetry refused");
 const auto deep=sample_water_coast(c,0,-400,3);
 const auto shallow=sample_water_coast(c,0,-280,3);
 check(deep.inZone&&shallow.inZone,"shore and offshore positions inside region");
 check(deep.depth>shallow.depth&&shallow.depth>0,"seabed shoals toward beach");
 const auto land=sample_water_coast(c,0,-235,3);
 check(land.inZone&&land.depth==0,"shoreline becomes dry");
 check(!sample_water_coast(c,400,-280,3).inZone,"finite lateral reach");
 check(!sample_water_coast(c,0,-600,3).inZone,"finite offshore reach");
 bool crest=false,move=false;
 for(int t=0;t<90;++t){
    const auto q=sample_water_coast(c,0,-275,double(t)*.13);
    const auto later=sample_water_coast(c,0,-275,double(t)*.13+.2);
    if(q.breaking>.20F&&q.foam>.1F&&q.flowY>.25F)crest=true;
    if(std::abs(q.height-later.height)>.01F&&std::abs(q.verticalVelocity)>.001F)move=true;
 }
 check(crest,"nearshore breaking face creates foam and shoreward water flow");
 check(move,"moving wave face has time-dependent elevation and vertical velocity");
 OceanField ocean;check(ocean.set_preset(OceanPreset::Calm),"calm baseline");
 const auto untouched=ocean.sample(0,-280,3);
 check(ocean.set_coast(c),"configured coast accepted");
 const auto near=ocean.sample(0,-280,3);
 check(near.depth<3.F&&std::isfinite(near.height)&&std::isfinite(near.normal.z),
     "authoritative ocean samples depth-aware surfing");
 check(near.height!=untouched.height,"coastal surf affects same physical water field");
 check(ocean.sample(0,-235,3).depth==0,"ocean reports dry land");
 ocean.clear_coast();
 check(ocean.sample(0,-280,3).depth>100,"disabled local coast preserves W1/W3 ocean");
 check(!ocean.set_coast(bad)&&!ocean.has_coast(),"invalid coast setup does not mutate current coast");
}
void rider_dynamics(){
 OceanField ocean;check(ocean.set_preset(OceanPreset::Calm),"baseline");
 check(ocean.set_coast(beach()),"coast accepted");
 WaterHull board;board.massKg=95;board.displacementCubicMetres=.12F;
 board.lengthMetres=2.2F;board.beamMetres=.73F;board.draftMetres=.24F;
 board.longitudinalProbes=4;board.lateralProbes=2;
 board.linearDamping=80;board.angularDamping=70;
 check(valid_water_hull(board),"realistic rider and board parameter range");
 WaterRigidState body;body.position={0,-275,.1F};body.linearVelocity={1.1F,4.F,0};
 const auto water=ocean.sample(0,-275,4);
 const auto noInput=evaluate_surfboard(board,body,water,{},.85F);
 const auto carve=evaluate_surfboard(board,body,water,{1,1},.85F);
 check(std::isfinite(carve.force.x)&&std::isfinite(carve.torque.z),
       "surfboard forces and moments finite");
 check(length(subtract(carve.force,noInput.force))>30,
       "rider paddling/lean changes force and direction");
 check(std::abs(carve.torque.z-noInput.torque.z)>1,
       "rear fins apply a real carving moment");
 const auto airborne=evaluate_surfboard(board,body,water,{1,1},0);
 check(length(airborne.force)==0&&length(airborne.torque)==0,
       "no free wave-face boost or fin force when airborne");
 WaterRigidState tipped=body;
 tipped.orientation=quaternion_from_axis_angle({0,1,0},1.25F);
 const auto fallen=evaluate_surfboard(board,tipped,water,{},.85F);
 check(fallen.wipeout,"large rail-overturn initiates a wipeout");
 const auto face=evaluate_surfboard(board,body,water,{},.85F);
 check(std::isfinite(face.faceDrive)&&std::isfinite(face.stability),
       "surfing reports finite energy and balance telemetry");
 // Surfing should never generate a horizontal wave-face force on flat water.
 OceanSample flat;flat.normal={0,0,1};flat.depth=100;
 const auto calm=evaluate_surfboard(board,body,flat,{},1);
 check(calm.faceDrive==0,"flat water provides no passive surfing propulsion");
}
}
int main(){
 try{coast_queries();rider_dynamics();
     std::cout<<"W4 coastal shoaling, surfboard carving and wipeout tests passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"W4: "<<e.what()<<'\n';return 1;}
}
