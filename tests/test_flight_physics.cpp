#include "dve/flight_wind.hpp"
#include "dve/flight_aerodynamics.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace dve;
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
bool near(float a,float b,float tolerance=0.001F){return std::abs(a-b)<=tolerance;}
void wind_field_tests(){
 FlightWindField field;
 field.configure({0,0,0});
 require(near(length(field.sample({0,0,80},0).velocity),0),"calm weather has no hidden F1 updraft");
 FlightWindZone thermal{};
 thermal.stableId=10;thermal.kind=FlightWindKind::Thermal;
 thermal.transform.position={0,0,0};thermal.radiusMeters=80;
 thermal.heightMeters=300;thermal.strengthMetersPerSecond=8;thermal.sinkFraction=0.30F;
 require(validate_flight_wind_zone(thermal),"valid thermal");
 field.set_zones({thermal});
 const auto core=field.sample({0,0,80},0);
 const auto edge=field.sample({80,0,80},0);
 const auto sink=field.sample({112,0,80},0);
 const auto outside=field.sample({180,0,80},0);
 require(core.thermalVertical>7.9F,"thermal core reaches authored wind speed");
 require(near(edge.thermalVertical,0,.01F),"thermal boundary continuous");
 require(sink.thermalVertical<-.5F,"thermal includes compensating surrounding sink");
 require(near(outside.velocity.z,0),"thermal compact support");
 require(field.sample({0,0,400},0).velocity.z==0,"thermal capped at authored height");
 require(near(field.sample({0,0,80},0).velocity.z,core.velocity.z),"deterministic repeated wind sample");
 field.configure({6,0,0},3,1);
 const auto a=field.sample({30,20,100},7.125F),b=field.sample({30,20,100},7.125F);
 require(a.velocity.x==b.velocity.x&&a.velocity.y==b.velocity.y&&a.velocity.z==b.velocity.z,
         "gusts are pure functions of world position and saved flight clock");
 const auto high=field.sample({300,0,500},7.125F).velocity;
 const auto low=field.sample({300,0,0},7.125F).velocity;
 require(high.x>low.x,"altitude shear changes wind");
 FlightWindZone invalid=thermal;invalid.strengthMetersPerSecond=-1;
 require(!validate_flight_wind_zone(invalid),"invalid negative wind intensity refused");
 // Many overlapping editor-authored sources must stay bounded, and F3 HUD
 // thermal/ridge attribution must agree with the wind actually simulated.
 std::vector<FlightWindZone> stack;
 for(std::uint64_t i=1;i<=128;++i){
   auto z=thermal;z.stableId=i;z.strengthMetersPerSecond=30.0F;
   stack.push_back(z);
 }
 field.configure({0,0,0});
 field.set_zones(std::move(stack));
 const auto saturated=field.sample({0,0,100},0);
 require(length(saturated.velocity)<=150.001F,"overlapping wind volumes stay bounded");
 require(std::abs(saturated.velocity.z-saturated.thermalVertical-
                  saturated.ridgeVertical-saturated.backgroundVertical)<.02F,
         "bounded wind-vector value matches updraft instrumentation");
}
void ridge_tests(){
 FlightWindField field;
 FlightWindZone ridge{};
 ridge.kind=FlightWindKind::Ridge;ridge.stableId=42;ridge.radiusMeters=100;
 ridge.heightMeters=110;ridge.strengthMetersPerSecond=8;ridge.sinkFraction=.5F;
 ridge.transform.rotation=quaternion_from_axis_angle({0,0,1},1.5707963268F);
 ridge.transform.position={0,0,50};
 field.set_zones({ridge});field.configure({10,0,0});
 const auto front=field.sample({-20,0,50},0);
 const auto back=field.sample({20,0,50},0);
 require(front.ridgeVertical>0.25F,"windward ridge lift rises");
 require(back.ridgeVertical<-.1F,"lee produces descending air");
 field.configure({-10,0,0});
 const auto reversedFront=field.sample({20,0,50},0);
 const auto reversedBack=field.sample({-20,0,50},0);
 require(reversedFront.ridgeVertical>0.25F&&reversedBack.ridgeVertical<-.1F,
         "reversing prevailing wind swaps windward and lee sides");
 require(near(field.sample({0,500,50},0).ridgeVertical,0),"finite ridge span");
}
void aerodynamic_energy_tests(){
 auto glider=flight_profile(FlightMode::Glider);
 const Quaternion forward{};
 const auto baseline=flight_aerodynamics(glider,{0,20,-1},forward);
 require(baseline.airspeed>20&&baseline.dynamicPressure>0,"dynamic pressure from air-relative velocity");
 require(dot(baseline.forceNewton,Float3{0,20,-1})<=.0005F,
         "wind-relative aerodynamic force never creates energy");
 // A rising air mass can transfer kinetic energy to a glider without
 // constituting unpowered energy creation: the relevant rest frame is air.
 const Float3 airRelative{0,22,-1};
 const Float3 inUpdraft{0,22,7}; // air mass rises at 8 m/s
 const auto liftFromAir=flight_aerodynamics(glider,airRelative,forward);
 require(dot(liftFromAir.forceNewton,airRelative)<=.0005F,
         "drag remains dissipative in the moving air frame");
 require(dot(liftFromAir.forceNewton,inUpdraft)>0,
         "rising atmosphere can legitimately supply energy to a glider");
 const auto windReferenced=flight_aerodynamics(glider,{0,10,-1},forward);
 require(windReferenced.dynamicPressure<baseline.dynamicPressure,"tailwind reduces airspeed at same groundspeed");
 const auto trim=flight_aerodynamics(glider,{0,20,-1},forward);
 const Quaternion pitched=quaternion_from_axis_angle({1,0,0},.95F);
 const auto stalled=flight_aerodynamics(glider,{0,20,-1},pitched);
 require(stalled.stalled,"critical angle of attack causes stall");
 require(std::abs(stalled.liftCoefficient)<std::abs(trim.liftCoefficient),
         "post-stall lift collapses rather than remaining saturated");
 require(stalled.dragCoefficient>trim.dragCoefficient,"post-stall drag rises");
 Float3 velocity{0,22,-1};Float3 position{0,0,180};
 constexpr float dt=1.0F/120.0F,mass=140.0F,g=9.81F;
 float previous=0.5F*dot(velocity,velocity)+g*position.z;
 for(unsigned i=0;i<1200;++i){
   const auto force=flight_aerodynamics(glider,velocity,Quaternion{}).forceNewton;
   // Symplectic Euler, whose finite-step potential/kinetic cross-term is small.
   const Float3 acceleration=add(multiply(force,1.0F/mass),{0,0,-g});
   velocity=add(velocity,multiply(acceleration,dt));
   position=add(position,multiply(velocity,dt));
   const float energy=0.5F*dot(velocity,velocity)+g*position.z;
   require(energy<=previous+.012F,"glider may not generate total specific energy in still air");
   previous=energy;
 }
 require(position.z<180,"unpowered glider loses altitude in still air");
 require(length(velocity)<300&&std::isfinite(position.z),"unpowered glide numerically stable");
}
}
int main(){
 try{wind_field_tests();ridge_tests();aerodynamic_energy_tests();
     std::cout<<"F2 wind, ridge reversal and aerodynamic energy checks passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"F2 physics: "<<e.what()<<'\n';return 1;}
}
