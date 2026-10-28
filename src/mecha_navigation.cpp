#include "dve/mecha_navigation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>
namespace dve {
namespace {
struct QueueNode {float score;int idx;};
struct ByScore {
 bool operator()(const QueueNode& a,const QueueNode& b)const noexcept {
    if(a.score!=b.score)return a.score>b.score;
    return a.idx>b.idx;
 }
};
bool valid(Float3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
}
MechNavigationResult plan_mech_navigation_xy(
 Float3 start,Float3 goal,const std::function<bool(Float3)>& passable,MechNavigationConfig cfg){
 MechNavigationResult out{};
 if(!valid(start)||!valid(goal)||!passable||
    !std::isfinite(cfg.cellMeters)||cfg.cellMeters<.5F||cfg.cellMeters>20.F||
    !std::isfinite(cfg.marginMeters)||cfg.marginMeters<0||cfg.marginMeters>50||
    cfg.maximumCells<16||cfg.maximumCells>4096)return out;
 // Grid covers both actors and enough padding for routes around an obstacle. A hard cell
 // budget prevents route requests from monopolizing the fixed-step simulation.
 const float pad=cfg.marginMeters+cfg.cellMeters;
 const float minX=std::floor((std::min(start.x,goal.x)-pad)/cfg.cellMeters)*cfg.cellMeters;
 const float minY=std::floor((std::min(start.y,goal.y)-pad)/cfg.cellMeters)*cfg.cellMeters;
 const float maxX=std::ceil((std::max(start.x,goal.x)+pad)/cfg.cellMeters)*cfg.cellMeters;
 const float maxY=std::ceil((std::max(start.y,goal.y)+pad)/cfg.cellMeters)*cfg.cellMeters;
 const auto w=static_cast<int>(std::lround((maxX-minX)/cfg.cellMeters))+1;
 const auto h=static_cast<int>(std::lround((maxY-minY)/cfg.cellMeters))+1;
 if(w<=1||h<=1||std::size_t(w)*std::size_t(h)>cfg.maximumCells)return out;
 const auto node=[&](int x,int y){return y*w+x;};
 const auto wx=[&](int x){return minX+float(x)*cfg.cellMeters;};
 const auto wy=[&](int y){return minY+float(y)*cfg.cellMeters;};
 const auto coord=[&](Float3 p){return std::pair{
    std::clamp(int(std::lround((p.x-minX)/cfg.cellMeters)),0,w-1),
    std::clamp(int(std::lround((p.y-minY)/cfg.cellMeters)),0,h-1)};
 };
 const auto [sx,sy]=coord(start);const auto [gx,gy]=coord(goal);
 const int startNode=node(sx,sy),goalNode=node(gx,gy);
 const auto count=std::size_t(w)*std::size_t(h);
 std::vector<std::int8_t> blocked(count,-1);
 auto freeAt=[&](int x,int y){
  if(x<0||y<0||x>=w||y>=h)return false;
  const int n=node(x,y);if(n==startNode)return true;
  if(blocked[std::size_t(n)]<0){
   blocked[std::size_t(n)]=passable({wx(x),wy(y),start.z})?0:1;
   ++out.examinedCells;
  }
  return blocked[std::size_t(n)]==0;
 };
 const auto heuristic=[&](int x,int y){return std::hypot(float(gx-x),float(gy-y));};
 constexpr float infinity=std::numeric_limits<float>::infinity();
 std::vector<float> cost(count,infinity);
 std::vector<int> parent(count,-1);
 std::vector<bool> closed(count,false);
 std::priority_queue<QueueNode,std::vector<QueueNode>,ByScore> open;
 cost[std::size_t(startNode)]=0;open.push({heuristic(sx,sy),startNode});
 const std::array<std::pair<int,int>,8> steps{{{1,0},{0,1},{-1,0},{0,-1},{1,1},{-1,1},{-1,-1},{1,-1}}};
 int reached=-1;
 while(!open.empty()){
  const int current=open.top().idx;open.pop();
  if(closed[std::size_t(current)])continue;
  closed[std::size_t(current)]=true;
  if(current==goalNode){reached=current;break;}
  const int x=current%w,y=current/w;
  for(const auto [dx,dy]:steps){
   const int nx=x+dx,ny=y+dy;
   if(!freeAt(nx,ny)||closed[std::size_t(node(nx,ny))])continue;
   if(dx!=0&&dy!=0&&(!freeAt(x+dx,y)||!freeAt(x,y+dy)))continue;
   const int n=node(nx,ny);const float step=dx&&dy?1.41421356237F:1.F;
   const float g=cost[std::size_t(current)]+step;
   if(g>=cost[std::size_t(n)])continue;
   cost[std::size_t(n)]=g;parent[std::size_t(n)]=current;
   open.push({g+heuristic(nx,ny),n});
  }
 }
 if(reached<0)return out;
 out.reachedGoal=true;
 std::vector<int> reversed;
 for(int n=reached;n!=startNode;n=parent[std::size_t(n)]){
  if(n<0||reversed.size()>count)return {};
  reversed.push_back(n);
 }
 std::reverse(reversed.begin(),reversed.end());
 out.waypoints.reserve(reversed.size()+1U);
 for(int n:reversed)out.waypoints.push_back({wx(n%w),wy(n/w),goal.z});
 // Exact goal must also be walkable, otherwise stop at the final grid cell.
 if(passable(goal))out.waypoints.push_back(goal);
 return out;
}
} // namespace dve
