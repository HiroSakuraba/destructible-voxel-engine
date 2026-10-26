#include "dve/underwater_optics.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void attenuation(){
 OceanField sea;check(sea.set_preset(OceanPreset::Calm),"calm water");
 WaterVolumeSettings settings;
 auto clear=sample_water_volume(sea,settings,{0,0,-12},0);
 const auto near=water_transmittance(clear,2);
 const auto far=water_transmittance(clear,20);
 check(near.x>far.x&&near.y>far.y&&near.z>far.z,
      "longer underwater travel loses more light in each band");
 check(far.x<far.y&&far.y<far.z,
      "clear-water red extinction is stronger than blue");
 const auto sunlightNear=underwater_daylight(clear,2);
 const auto sunlightDeep=underwater_daylight(clear,40);
 check(sunlightDeep.x<sunlightNear.x&&sunlightDeep.z<sunlightNear.z,
      "solar irradiance decreases with submerged object depth");
 check(sunlightDeep.z>sunlightDeep.x,
      "blue penetrates deeper than red");
 check(water_transmittance(clear,0).x==1,
      "zero water path causes exactly zero water absorption");
 settings.clarity=WaterClarity::Turbid;
 const auto muddy=sample_water_volume(sea,settings,{0,0,-12},0);
 check(water_transmittance(muddy,10).z<far.z,
      "turbidity drastically shortens underwater visibility");
 settings.customOptics=true;
 settings.absorptionRGB={.8F,.2F,.04F};
 settings.scatteringRGB={.1F,.1F,.1F};
 settings.diffuseKdRGB={.9F,.25F,.09F};
 check(valid_water_volume_settings(settings),"positive custom spectral RGB profiles validate");
 const auto custom=sample_water_volume(sea,settings,{0,0,-12},0);
 check(std::abs(custom.absorptionRGB.x-.8F)<1.e-5F&&
       std::abs(custom.diffuseKdRGB.y-.25F)<1.e-5F,
       "custom absorption and diffuse attenuation are independent");
 settings.scatteringRGB.x=-1;
 check(!valid_water_volume_settings(settings),"negative measured coefficients rejected");
}
void color_and_lamps(){
 OceanField ocean;check(ocean.set_preset(OceanPreset::Calm),"calm");
 WaterVolumeSettings cfg;
 const auto water=sample_water_volume(ocean,cfg,{0,0,-20},0);
 UnderwaterOpticsInput input{};
 input.objectRadiance={2.F,.4F,.15F};
 input.ambientSurfaceLight={.6F,.6F,.6F};
 input.objectDepthMeters=20;
 input.eyeDepthMeters=20;
 input.objectSubmerged=true;
 input.viewPathMeters=1.5F;
 input.rayDirectionZ=0.F;
 const auto near=shade_underwater_radiance(water,input);
 input.viewPathMeters=25.F;
 const auto far=shade_underwater_radiance(water,input);
 check(near.x>far.x,"red reflectance fades with viewing distance");
 check(std::isfinite(far.z)&&far.z>=0,"in-scattering remains finite");
 UnderwaterLamp lamp;
 lamp.position={0,0,-20};lamp.direction={0,1,0};
 lamp.range=30;lamp.intensity=30;
 auto nearLamp=evaluate_underwater_lamp(lamp,water,{0,3,-20});
 auto distantLamp=evaluate_underwater_lamp(lamp,water,{0,31,-20});
 check(nearLamp.x>0&&nearLamp.y>0&&nearLamp.z>0,"lamp emits filtered near-field color");
 check(distantLamp.x==0&&distantLamp.y==0,"lamp stops at finite range");
 check(evaluate_underwater_lamp(lamp,water,{0,-3,-20}).x==0,
      "lamp cone has dark backwards direction");
 input.viewPathMeters=2;
 input.lampIrradiance=nearLamp;
 const auto lit=shade_underwater_radiance(water,input);
 input.lampIrradiance={};
 const auto dark=shade_underwater_radiance(water,input);
 check(lit.x>dark.x*1.2F,"local artificial lamps restore red nearby at depth");
}
void surface(){
 OceanField sea;check(sea.set_preset(OceanPreset::Calm),"calm");
 const auto below=water_ray_segment(sea,{0,0,-8},{0,0,1},16,0);
 const auto above=water_ray_segment(sea,{0,0,8},{0,0,-1},16,0);
 check(below.cameraSubmerged&&below.hitsSurface,"ray exits water from below");
 check(!above.cameraSubmerged&&above.hitsSurface,"ray enters water from air");
 check(std::abs(below.lengthMeters-8)<.03F&&std::abs(above.startMeters-8)<.03F,
       "surface crossing preserves physical underwater path length");
 check(std::abs(above.lengthMeters-8)<.03F,"above-water camera attenuates underwater half only");
 const auto dry=water_ray_segment(sea,{0,0,8},{0,0,1},12,0);
 check(dry.lengthMeters==0,"air-to-air path is not tinted blue");
 const auto wet=water_ray_segment(sea,{0,0,-8},{0,0,-1},12,0);
 check(wet.lengthMeters==12&&wet.cameraSubmerged,"fully submerged path");
 check(water_to_air_fresnel(.3F)==1.F,"critical-angle total internal reflection");
 const float normal=water_to_air_fresnel(1.F);
 check(normal>.015F&&normal<.04F,"normal-incidence water/air Fresnel about two percent");
}
}
int main(){
 try{attenuation();color_and_lamps();surface();
   std::cout<<"S2 optics, lamps, wavelength attenuation and surface-crossing tests passed\n";
   return 0;
 }catch(const std::exception& e){std::cerr<<"S2: "<<e.what()<<'\n';return 1;}
}
