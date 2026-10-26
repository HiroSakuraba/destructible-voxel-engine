#include "dve/prototype_game.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
namespace dve {
namespace {
std::array<unsigned,7> glyph(char c){
 switch(c){
#define G(C,A,B,D,E,F,H,I) case C:return {A,B,D,E,F,H,I};
 G('A',14,17,17,31,17,17,17) G('B',30,17,17,30,17,17,30) G('C',14,17,16,16,16,17,14)
 G('D',30,17,17,17,17,17,30) G('E',31,16,16,30,16,16,31) G('F',31,16,16,30,16,16,16)
 G('G',14,17,16,23,17,17,15) G('H',17,17,17,31,17,17,17) G('I',14,4,4,4,4,4,14)
 G('J',7,2,2,2,18,18,12) G('K',17,18,20,24,20,18,17) G('L',16,16,16,16,16,16,31)
 G('M',17,27,21,21,17,17,17) G('N',17,25,21,19,17,17,17) G('O',14,17,17,17,17,17,14)
 G('P',30,17,17,30,16,16,16) G('Q',14,17,17,17,21,18,13) G('R',30,17,17,30,20,18,17)
 G('S',15,16,16,14,1,1,30) G('T',31,4,4,4,4,4,4) G('U',17,17,17,17,17,17,14)
 G('V',17,17,17,17,17,10,4) G('W',17,17,17,21,21,21,10) G('X',17,17,10,4,10,17,17)
 G('Y',17,17,10,4,4,4,4) G('Z',31,1,2,4,8,16,31)
 G('0',14,17,19,21,25,17,14) G('1',4,12,4,4,4,4,14) G('2',14,17,1,2,4,8,31)
 G('3',30,1,1,14,1,1,30) G('4',2,6,10,18,31,2,2) G('5',31,16,16,30,1,1,30)
 G('6',14,16,16,30,17,17,14) G('7',31,1,2,4,8,8,8) G('8',14,17,17,14,17,17,14)
 G('9',14,17,17,15,1,1,14) G(':',0,4,4,0,4,4,0) G('/',1,2,2,4,8,8,16)
 G('-',0,0,0,31,0,0,0) G('.',0,0,0,0,0,4,4) G('>',16,8,4,2,4,8,16)
#undef G
 default:return {};
 }
}
}
void draw_prototype_hud(std::span<std::uint8_t> rgba,unsigned w,unsigned h,const PrototypeHud& hud){
 if(!hud.active||w<32||h<32||w>16384||h>16384||rgba.size()!=std::size_t(w)*h*4)return;
 if(!std::isfinite(hud.health)||!std::isfinite(hud.maximumHealth)||hud.health<0||hud.health>hud.maximumHealth||hud.maximumHealth<=0||hud.maximumHealth>1000000)return;
 const unsigned scale=w>=640&&h>=360?2U:1U;auto rect=[&](unsigned x,unsigned y,unsigned width,unsigned height,std::array<std::uint8_t,3> color){for(unsigned yy=y;yy<(std::min)(h,y+height);++yy)for(unsigned xx=x;xx<(std::min)(w,x+width);++xx){auto n=(std::size_t(yy)*w+xx)*4;for(unsigned c=0;c<3;++c)rgba[n+c]=color[c];rgba[n+3]=255;}};
 auto text=[&](unsigned x,unsigned y,std::string_view str){unsigned xx=x;for(char raw:str){char c=raw>='a'&&raw<='z'?char(raw-'a'+'A'):raw;if(xx+5*scale>=w)break;auto rows=glyph(c);for(unsigned ry=0;ry<7;++ry)for(unsigned rx=0;rx<5;++rx)if(rows[ry]&(1U<<(4-rx)))rect(xx+rx*scale,y+ry*scale,scale,scale,{235,242,250});xx+=6*scale;}};
 const auto line=10*scale;rect(4,4,(std::min)(w-8,320*scale),4*line+8,{18,27,40});
 if(hud.water){text(8,8,hud.stats);text(8,8+line,hud.prompt);text(8,8+2*line,hud.message);
 text(8,8+3*line,hud.debug?hud.vehicleTelemetry:(hud.stats.find("MINI-SUB")!=std::string::npos?"W/S DRIVE SPACE/CTRL UP/DOWN Z/X BALLAST H HOLD":"W/S POWER/PADDLE A/D CARVE Q/E TRIM 1..6"));return;}
 if(hud.flying){
   if(hud.debug)rect(4,4,(std::min)(w-8,320*scale),5*line+8,{18,27,40});
   text(8,8,hud.stats);text(8,8+line,hud.prompt);text(8,8+2*line,hud.message);
   text(8,8+3*line,hud.debug?hud.vehicleTelemetry:"TAB MODES  WASD FLY  SHIFT/CTRL POWER");
   if(hud.debug)text(8,8+4*line,hud.flightWeather);
   return;}
 if(hud.racing){text(8,8,hud.stats);text(8,8+line,hud.prompt);text(8,8+2*line,hud.phase==PrototypePhase::Paused?"PAUSED - P TO RESUME":hud.message);text(8,8+3*line,hud.debug?hud.vehicleTelemetry:"B BRAKE SPACE HANDBRAKE A/D STEER");if(hud.debug&&!hud.trace.empty())text(8,8+5*line,hud.trace.back());return;}
 text(8,8,"HEALTH "+std::to_string(unsigned((std::max)(0.F,hud.health)))+" / "+std::to_string(unsigned((std::max)(0.F,hud.maximumHealth))));
 const unsigned bar=(std::min)(w-20,160*scale);rect(8,8+line,bar,5*scale,{65,76,90});float ratio=std::isfinite(hud.health)&&hud.maximumHealth>0?std::clamp(hud.health/hud.maximumHealth,0.F,1.F):0;rect(8,8+line,unsigned(float(bar)*ratio),5*scale,{85,205,145});
 text(8,8+2*line,"ITEMS "+std::to_string(hud.collected)+" / "+std::to_string(hud.required));
 text(8,8+3*line,hud.phase==PrototypePhase::Paused?"PAUSED - P TO RESUME":hud.phase==PrototypePhase::Won?"YOU WIN - R TO RESTART":hud.phase==PrototypePhase::Lost?"YOU LOST - R TO RESTART":"WASD MOVE  E USE  P PAUSE  R RESTART");
 rect(4,h-2*line-4,w-8,2*line,{18,27,40});text(8,h-2*line,hud.message);text(8,h-line,hud.prompt);
 if(hud.debug){unsigned y=6*line;auto start=hud.trace.size()>6?hud.trace.size()-6:0;for(auto n=start;n<hud.trace.size()&&y+3*line<h-2*line;++n){text(8,y,hud.trace[n]);y+=line;}}
}
}
