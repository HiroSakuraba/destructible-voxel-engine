#include "dve/water_vessel_dynamics.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
namespace {
void check(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
WaterHull small_hull(){
    WaterHull h;h.massKg=1800;h.displacementCubicMetres=2.25F;
    h.lengthMetres=8;h.beamMetres=2.6F;h.draftMetres=1.2F;
    return h;
}
void sailing(){
    WaterHull hull=small_hull();WaterRigidState state{};
    OceanSample water{};water.normal={0,0,1};
    VesselDynamics sail; sail.type=WatercraftType::Sailboat;
    sail.sailArea=29;sail.keelArea=1.6F;sail.mastHeight=4.2F;
    sail.thrustNewtons=0;sail.rudderNewtons=700;
    VesselInput input;input.throttle=1;input.sailTrim=.60F;
    check(valid_vessel_dynamics(sail),"sailing coefficients accepted");
    const auto calm=evaluate_vessel_dynamics(sail,hull,state,water,{0,0,0},input,1);
    check(length(calm.force)<.001F,"sailing vessel not propelled in still air at rest");
    const auto windy=evaluate_vessel_dynamics(sail,hull,state,water,{5,8,0},input,1);
    check(windy.apparentWind>9&&length(windy.force)>30,
          "apparent wind generates a real sail force");
    check(length(windy.torque)>10,"mast height creates heeling torque");
    input.sailTrim=-.60F;
    const auto sheeted=evaluate_vessel_dynamics(sail,hull,state,water,{5,8,0},input,1);
    check(length(subtract(windy.force,sheeted.force))>5,
          "sail angle changes generated force and course");
    input.sailTrim=.6F;input.throttle=0;
    const auto furled=evaluate_vessel_dynamics(sail,hull,state,water,{5,8,0},input,1);
    check(length(furled.force)<length(windy.force),
          "less deployed sail reduces aerodynamic force");
    state.linearVelocity={3,0,0};
    const auto sideslip=evaluate_vessel_dynamics(sail,hull,state,water,{0,0,0},{},1);
    check(sideslip.keelForce<0&&sideslip.force.x<0,
          "keel resists starboard sideslip");
    const auto dry=evaluate_vessel_dynamics(sail,hull,state,water,{0,0,0},{},0);
    check(dry.keelForce==0,"keel cannot resist sideslip while airborne");
}
void steering_and_motors(){
    WaterHull hull=small_hull();WaterRigidState state{};
    OceanSample ocean{};ocean.normal={0,0,1};
    VesselDynamics boat;boat.type=WatercraftType::Dinghy;
    boat.thrustNewtons=1200;boat.rudderNewtons=380;
    VesselInput input;input.throttle=1;input.helm=1;
    const auto atRest=evaluate_vessel_dynamics(boat,hull,state,ocean,{},input,1);
    check(atRest.force.y>500&&std::abs(atRest.rudderMoment)<.01F,
          "motor thrust at rest, no rudder lift without water flow");
    state.linearVelocity={0,12,0};
    const auto fast=evaluate_vessel_dynamics(boat,hull,state,ocean,{},input,1);
    check(std::abs(fast.rudderMoment)>300,
          "rudder torque depends on forward water speed");
    input.throttle=0;
    const auto idle=evaluate_vessel_dynamics(boat,hull,state,ocean,{},input,1);
    check(idle.force.y==0,"idle engine applies no thrust");
    boat.type=WatercraftType::WaveRunner;boat.thrustNewtons=5500;
    input.throttle=1;
    const auto jet=evaluate_vessel_dynamics(boat,hull,state,ocean,{},input,1);
    check(std::abs(jet.force.x)>500,
          "wave runner pump jet vectors thrust under helm");
    const auto airborne=evaluate_vessel_dynamics(boat,hull,state,ocean,{},input,0);
    check(length(airborne.force)<.01F,
          "pump jet requires immersed water intake");
}
void planing(){
    auto h=small_hull();WaterRigidState s{};
    OceanSample ocean{};ocean.normal={0,0,1};
    VesselDynamics fast;fast.type=WatercraftType::Speedboat;
    fast.planingFactor=.06F;fast.thrustNewtons=24000;
    s.linearVelocity={0,15,0};
    const auto wet=evaluate_vessel_dynamics(fast,h,s,ocean,{},VesselInput{},.65F);
    check(wet.planingLift>1000&&wet.planingLift<h.massKg*9.81F*1.21F,
          "planing force rises with speed but remains capped");
    s.linearVelocity={0,1,0};
    const auto slow=evaluate_vessel_dynamics(fast,h,s,ocean,{},VesselInput{},.65F);
    check(slow.planingLift==0,"no static planing lift");
    s.linearVelocity={0,-15,0};
    const auto reverse=evaluate_vessel_dynamics(fast,h,s,ocean,{},VesselInput{},.65F);
    check(reverse.planingLift==0,"no reverse speed hydrofoil lift");
    s.linearVelocity={0,15,0};
    const auto dry=evaluate_vessel_dynamics(fast,h,s,ocean,{},VesselInput{},0);
    check(dry.planingLift==0,"airborne boat receives no free planing force");
    const auto full=evaluate_vessel_dynamics(fast,h,s,ocean,{},VesselInput{},1);
    check(full.planingLift>=wet.planingLift,"in-water planing load increases with immersion");
}
void config(){
    VesselDynamics bad;bad.type=WatercraftType::Speedboat;bad.planingFactor=-.5F;
    check(!valid_vessel_dynamics(bad),"reject invalid planing coefficient");
    bad.planingFactor=0;bad.sailArea=20000;
    check(!valid_vessel_dynamics(bad),"reject implausible sail area");
    check(std::string(watercraft_name(WatercraftType::PirateShip))=="PIRATE SHIP",
          "pirate mode identifies independently");
}
}
int main(){
 try{sailing();steering_and_motors();planing();config();
     std::cout<<"W2 sail, keel, power, planing and pump jet checks passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"W2 dynamics: "<<e.what()<<'\n';return 1;}
}
