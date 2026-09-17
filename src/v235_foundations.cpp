#include "dve/v235_foundations.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-6F;
constexpr std::array<char, 8> kPakMagic{'D','V','E','P','A','K','1','\0'};
constexpr std::array<char, 8> kSaveMagic{'D','V','E','S','A','V','E','1'};

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Float3 sub(Float3 a, Float3 b) noexcept { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Float3 mul(Float3 value, float scale) noexcept { return {value.x*scale,value.y*scale,value.z*scale}; }
Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
float distance(Float3 a, Float3 b) noexcept { return length(sub(a,b)); }

std::uint64_t fnv_bytes(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash=1469598103934665603ULL;
    for (std::byte value:bytes) { hash^=std::to_integer<std::uint8_t>(value); hash*=1099511628211ULL; }
    return hash;
}

void fnv_mix(std::uint64_t& hash, std::string_view text) noexcept {
    for (unsigned char value:text) { hash^=value; hash*=1099511628211ULL; }
}

std::optional<std::vector<std::byte>> read_file_bytes(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)return fail(error,"could not open file: "+path.string()),std::nullopt;
    const auto end=input.tellg();
    if(end<0)return fail(error,"could not determine file size: "+path.string()),std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));input.seekg(0);
    if(!bytes.empty()&&!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
        return fail(error,"could not read file: "+path.string()),std::nullopt;
    return bytes;
}

template<class T> bool write_value(std::ostream& output,const T& value) {
    output.write(reinterpret_cast<const char*>(&value),sizeof(value));return static_cast<bool>(output);
}
template<class T> bool read_value(std::istream& input,T& value) {
    input.read(reinterpret_cast<char*>(&value),sizeof(value));return static_cast<bool>(input);
}

bool path_has_prefix(std::string_view path,std::string_view prefix) noexcept {
    return path.size()>=prefix.size()&&std::equal(prefix.begin(),prefix.end(),path.begin());
}

std::string normalize_package_path(const std::filesystem::path& path) {
    auto normalized=path.lexically_normal().generic_string();
    while(path_has_prefix(normalized,"./"))normalized.erase(0,2);
    return normalized;
}

NavigationBounds triangle_bounds(const NavigationTriangle& triangle) noexcept {
    NavigationBounds out{triangle.vertices[0],triangle.vertices[0]};
    for(std::size_t i=1;i<3U;++i){
        const auto v=triangle.vertices[i];
        out.minimum={std::min(out.minimum.x,v.x),std::min(out.minimum.y,v.y),std::min(out.minimum.z,v.z)};
        out.maximum={std::max(out.maximum.x,v.x),std::max(out.maximum.y,v.y),std::max(out.maximum.z,v.z)};
    }
    return out;
}

bool overlaps(const NavigationBounds& a,const NavigationBounds& b) noexcept {
    return a.minimum.x<=b.maximum.x&&a.maximum.x>=b.minimum.x&&
           a.minimum.y<=b.maximum.y&&a.maximum.y>=b.minimum.y&&
           a.minimum.z<=b.maximum.z&&a.maximum.z>=b.minimum.z;
}

struct NavigationStitchPoint {
    std::int64_t x{};
    std::int64_t z{};
    auto operator<=>(const NavigationStitchPoint&) const = default;
};

struct NavigationStitchEdge {
    NavigationStitchPoint a{};
    NavigationStitchPoint b{};
    auto operator<=>(const NavigationStitchEdge&) const = default;
};

struct NavigationStitchReference {
    NavigationPolygonId polygon{};
    Float3 a{};
    Float3 b{};
};

NavigationStitchPoint quantize_navigation_point(Float3 point,float tolerance) noexcept {
    const double inverse=1.0/static_cast<double>(tolerance);
    return {static_cast<std::int64_t>(std::llround(static_cast<double>(point.x)*inverse)),
            static_cast<std::int64_t>(std::llround(static_cast<double>(point.z)*inverse))};
}

NavigationStitchEdge navigation_stitch_edge(Float3 a,Float3 b,float tolerance) noexcept {
    auto first=quantize_navigation_point(a,tolerance);
    auto second=quantize_navigation_point(b,tolerance);
    if(second<first)std::swap(first,second);
    return {first,second};
}

bool stitch_navigation_tiles(NavigationMesh& mesh,std::string* error) {
    if(mesh.polygons.size()>mesh.settings.maximumPolygons)
        return fail(error,"incremental navigation mesh exceeds maximumPolygons");
    std::map<NavigationStitchEdge,std::vector<NavigationStitchReference>> edges;
    for(NavigationPolygonId polygon=0U;polygon<mesh.polygons.size();++polygon){
        auto& item=mesh.polygons[polygon];
        item.portals.clear();
        for(std::size_t edge=0U;edge<3U;++edge){
            const Float3 a=item.vertices[edge];
            const Float3 b=item.vertices[(edge+1U)%3U];
            edges[navigation_stitch_edge(a,b,mesh.settings.edgeMatchToleranceMeters)].push_back({polygon,a,b});
        }
    }
    for(const auto& [key,references]:edges){
        (void)key;
        for(std::size_t first=0U;first<references.size();++first){
            for(std::size_t second=first+1U;second<references.size();++second){
                const auto& a=references[first];
                const auto& b=references[second];
                if(a.polygon==b.polygon)continue;
                const float samePair=std::abs(a.a.x-b.a.x)+std::abs(a.a.z-b.a.z)+
                    std::abs(a.b.x-b.b.x)+std::abs(a.b.z-b.b.z);
                const float reversePair=std::abs(a.a.x-b.b.x)+std::abs(a.a.z-b.b.z)+
                    std::abs(a.b.x-b.a.x)+std::abs(a.b.z-b.a.z);
                const Float3 b0=samePair<=reversePair?b.a:b.b;
                const Float3 b1=samePair<=reversePair?b.b:b.a;
                if(std::abs(a.a.y-b0.y)>mesh.settings.maximumStepHeightMeters+
                       mesh.settings.edgeMatchToleranceMeters||
                   std::abs(a.b.y-b1.y)>mesh.settings.maximumStepHeightMeters+
                       mesh.settings.edgeMatchToleranceMeters)continue;
                Float3 p0{0.5F*(a.a.x+b0.x),std::max(a.a.y,b0.y),0.5F*(a.a.z+b0.z)};
                Float3 p1{0.5F*(a.b.x+b1.x),std::max(a.b.y,b1.y),0.5F*(a.b.z+b1.z)};
                const float width=length(sub(p1,p0));
                if(width<=2.0F*mesh.settings.agentRadiusMeters+
                        mesh.settings.edgeMatchToleranceMeters)continue;
                const Float3 direction=mul(sub(p1,p0),1.0F/width);
                p0=add(p0,mul(direction,mesh.settings.agentRadiusMeters));
                p1=sub(p1,mul(direction,mesh.settings.agentRadiusMeters));
                const auto connect=[&](NavigationPolygonId from,NavigationPolygonId to){
                    const Float3 travel=sub(mesh.polygons[to].center,mesh.polygons[from].center);
                    const float side0=travel.x*(p0.z-mesh.polygons[from].center.z)-
                        travel.z*(p0.x-mesh.polygons[from].center.x);
                    const float side1=travel.x*(p1.z-mesh.polygons[from].center.z)-
                        travel.z*(p1.x-mesh.polygons[from].center.x);
                    NavigationPortal portal;portal.neighbour=to;
                    if(side0>=side1){portal.left=p0;portal.right=p1;}
                    else{portal.left=p1;portal.right=p0;}
                    auto& portals=mesh.polygons[from].portals;
                    if(std::none_of(portals.begin(),portals.end(),[&](const auto& current){
                        return current.neighbour==to;
                    }))portals.push_back(portal);
                };
                connect(a.polygon,b.polygon);
                connect(b.polygon,a.polygon);
            }
        }
    }
    for(auto& polygon:mesh.polygons){
        std::sort(polygon.portals.begin(),polygon.portals.end(),[](const auto& a,const auto& b){
            return a.neighbour<b.neighbour;
        });
    }
    mesh.bounds={};
    if(!mesh.polygons.empty()){
        mesh.bounds=mesh.polygons.front().bounds;
        for(const auto& polygon:mesh.polygons){
            mesh.bounds.minimum={std::min(mesh.bounds.minimum.x,polygon.bounds.minimum.x),
                std::min(mesh.bounds.minimum.y,polygon.bounds.minimum.y),
                std::min(mesh.bounds.minimum.z,polygon.bounds.minimum.z)};
            mesh.bounds.maximum={std::max(mesh.bounds.maximum.x,polygon.bounds.maximum.x),
                std::max(mesh.bounds.maximum.y,polygon.bounds.maximum.y),
                std::max(mesh.bounds.maximum.z,polygon.bounds.maximum.z)};
        }
    }
    mesh.contentHash=navigation_mesh_content_hash(mesh);
    std::string validation;
    if(!mesh.validate(&validation))return fail(error,"incremental navigation validation failed: "+validation);
    return true;
}

Quaternion quaternion_multiply(Quaternion a,Quaternion b) noexcept {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}

Quaternion quaternion_axis_angle(Float3 axis,float angle) noexcept {
    axis=normalize(axis);const float half=angle*0.5F,s=std::sin(half);
    return {axis.x*s,axis.y*s,axis.z*s,std::cos(half)};
}

std::string json_escape(std::string_view text) {
    std::string out;out.reserve(text.size()+8U);
    for(char c:text){
        switch(c){case '\\':out+="\\\\";break;case '"':out+="\\\"";break;
        case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;
        default:if(static_cast<unsigned char>(c)<0x20U)out+='?';else out+=c;}
    }return out;
}

} // namespace

bool NavigationAgentConfig::validate(std::string* error) const noexcept {
    if(!std::isfinite(maximumSpeedMetersPerSecond)||maximumSpeedMetersPerSecond<=0.0F)
        return fail(error,"navigation agent maximum speed must be positive");
    if(!std::isfinite(maximumAccelerationMetersPerSecondSquared)||maximumAccelerationMetersPerSecondSquared<=0.0F)
        return fail(error,"navigation agent maximum acceleration must be positive");
    if(!std::isfinite(waypointRadiusMeters)||waypointRadiusMeters<=0.0F||
       !std::isfinite(arrivalRadiusMeters)||arrivalRadiusMeters<=0.0F||
       !std::isfinite(slowdownRadiusMeters)||slowdownRadiusMeters<arrivalRadiusMeters)
        return fail(error,"navigation agent arrival radii are invalid");
    if(!std::isfinite(stuckDistanceMeters)||stuckDistanceMeters<0.0F||
       !std::isfinite(stuckSeconds)||stuckSeconds<=0.0F||
       !std::isfinite(automaticReplanSeconds)||automaticReplanSeconds<0.0F)
        return fail(error,"navigation agent stuck/replan settings are invalid");
    return true;
}

NavigationAgent::NavigationAgent(NavigationAgentConfig config):config_(config){
    if(!config_.validate())config_=NavigationAgentConfig{};
}

bool NavigationAgent::set_target(Float3 target,std::string* error){
    if(!finite(target))return fail(error,"navigation target is not finite");
    target_=target;status_=NavigationAgentStatus::ReplanPending;path_={};waypointIndex_=0U;
    stuckTimer_=0.0F;replanTimer_=config_.automaticReplanSeconds;haveProgressSample_=false;return true;
}

void NavigationAgent::clear_target() noexcept {
    target_.reset();path_={};waypointIndex_=0U;status_=NavigationAgentStatus::Idle;stuckTimer_=0.0F;
}

void NavigationAgent::notify_navigation_revision(std::uint64_t revision) noexcept {
    observedRevision_=std::max(observedRevision_,revision);
    if(target_&&plannedRevision_!=observedRevision_&&status_!=NavigationAgentStatus::WaitingForOffMeshTraversal)
        status_=NavigationAgentStatus::ReplanPending;
}

bool NavigationAgent::replan(const NavigationMesh& mesh,std::uint64_t meshRevision,Float3 position,
                             const NavigationQueryFilter& filter,std::span<const NavigationObstacle> obstacles){
    ++replanCount_;plannedRevision_=meshRevision;observedRevision_=meshRevision;replanTimer_=0.0F;
    path_=find_navigation_path(mesh,position,*target_,filter,obstacles);waypointIndex_=0U;
    if(!path_){status_=NavigationAgentStatus::Failed;return false;}
    while(waypointIndex_<path_.points.size()&&distance(position,path_.points[waypointIndex_].position)<=config_.waypointRadiusMeters)
        ++waypointIndex_;
    status_=waypointIndex_>=path_.points.size()?NavigationAgentStatus::Arrived:NavigationAgentStatus::Following;
    stuckTimer_=0.0F;lastProgressPosition_=position;haveProgressSample_=true;return true;
}

NavigationAgentOutput NavigationAgent::tick(const NavigationMesh& mesh,std::uint64_t meshRevision,Float3 position,
    Float3 currentVelocity,float deltaSeconds,const NavigationQueryFilter& filter,
    std::span<const NavigationObstacle> obstacles){
    NavigationAgentOutput out{};out.status=status_;out.waypointIndex=waypointIndex_;
    if(!target_||!finite(position)||!finite(currentVelocity)||!std::isfinite(deltaSeconds)||deltaSeconds<=0.0F)return out;
    notify_navigation_revision(meshRevision);replanTimer_+=deltaSeconds;
    if(status_==NavigationAgentStatus::ReplanPending||status_==NavigationAgentStatus::Failed||status_==NavigationAgentStatus::Stuck){
        if(replanTimer_>=config_.automaticReplanSeconds){out.pathChanged=replan(mesh,meshRevision,position,filter,obstacles);}
        else return out;
    }
    if(status_==NavigationAgentStatus::WaitingForOffMeshTraversal){
        out.status=status_;out.waypointIndex=waypointIndex_;
        if(waypointIndex_<path_.points.size())out.offMeshTraversal=path_.points[waypointIndex_];
        return out;
    }
    while(waypointIndex_<path_.points.size()){
        const auto& waypoint=path_.points[waypointIndex_];
        const float radius=waypointIndex_+1U==path_.points.size()?config_.arrivalRadiusMeters:config_.waypointRadiusMeters;
        if(distance(position,waypoint.position)>radius)break;
        if(waypoint.kind==NavigationPathPointKind::OffMeshStart){
            status_=NavigationAgentStatus::WaitingForOffMeshTraversal;out.offMeshTraversal=waypoint;
            out.status=status_;out.waypointIndex=waypointIndex_;return out;
        }
        ++waypointIndex_;
    }
    if(waypointIndex_>=path_.points.size()){
        status_=NavigationAgentStatus::Arrived;out.status=status_;out.waypointIndex=waypointIndex_;return out;
    }
    const Float3 delta=sub(path_.points[waypointIndex_].position,position);const float remaining=length(delta);
    float speed=config_.maximumSpeedMetersPerSecond;
    if(waypointIndex_+1U==path_.points.size()&&remaining<config_.slowdownRadiusMeters)
        speed*=std::clamp(remaining/config_.slowdownRadiusMeters,0.05F,1.0F);
    const Float3 targetVelocity=mul(normalize(delta),speed);
    Float3 velocityDelta=sub(targetVelocity,currentVelocity);const float maximumDelta=config_.maximumAccelerationMetersPerSecondSquared*deltaSeconds;
    if(length(velocityDelta)>maximumDelta)velocityDelta=mul(normalize(velocityDelta),maximumDelta);
    out.desiredVelocity=add(currentVelocity,velocityDelta);
    if(haveProgressSample_){
        if(distance(position,lastProgressPosition_)<config_.stuckDistanceMeters&&length(out.desiredVelocity)>0.1F)stuckTimer_+=deltaSeconds;
        else{stuckTimer_=0.0F;lastProgressPosition_=position;}
    }else{lastProgressPosition_=position;haveProgressSample_=true;}
    if(stuckTimer_>=config_.stuckSeconds){status_=NavigationAgentStatus::Stuck;replanTimer_=config_.automaticReplanSeconds;out.desiredVelocity={};}
    else status_=NavigationAgentStatus::Following;
    out.status=status_;out.waypointIndex=waypointIndex_;return out;
}

bool NavigationAgent::complete_off_mesh_traversal(Float3 landedPosition) noexcept {
    if(status_!=NavigationAgentStatus::WaitingForOffMeshTraversal||!finite(landedPosition))return false;
    while(waypointIndex_<path_.points.size()&&path_.points[waypointIndex_].kind!=NavigationPathPointKind::OffMeshEnd)++waypointIndex_;
    if(waypointIndex_<path_.points.size())++waypointIndex_;
    lastProgressPosition_=landedPosition;stuckTimer_=0.0F;
    status_=waypointIndex_>=path_.points.size()?NavigationAgentStatus::Arrived:NavigationAgentStatus::Following;return true;
}

DynamicNavigationWorld::DynamicNavigationWorld(NavigationBuildSettings settings):settings_(settings){}

bool DynamicNavigationWorld::set_source_geometry(std::vector<NavigationTriangle> triangles,
    std::vector<NavigationOffMeshLink> links,std::string* error){
    auto result=build_navigation_mesh(triangles,settings_,links);
    if(!result.success())return fail(error,result.diagnostics.empty()?"navigation build failed":result.diagnostics.back().message);
    source_=std::move(triangles);links_=std::move(links);mesh_=std::move(result.mesh);dirty_.clear();
    std::set<NavigationDirtyTile> builtTiles;
    for(const auto& polygon:mesh_.polygons)builtTiles.insert({polygon.tileX,polygon.tileZ});
    ++telemetry_.revision;++telemetry_.rebuilds;telemetry_.polygonCount=mesh_.polygons.size();
    telemetry_.dirtyTileCount=0U;telemetry_.lastRebuiltTileCount=builtTiles.size();
    telemetry_.lastCandidateTriangleCount=source_.size();telemetry_.lastReusedPolygonCount=0U;
    telemetry_.lastRebuiltPolygonCount=mesh_.polygons.size();return true;
}

void DynamicNavigationWorld::mark_dirty(const NavigationBounds& bounds){
    if(!finite(bounds.minimum)||!finite(bounds.maximum)||settings_.tileSizeMeters<=0.0F)return;
    const auto minX=static_cast<std::int32_t>(std::floor(bounds.minimum.x/settings_.tileSizeMeters));
    const auto maxX=static_cast<std::int32_t>(std::floor(bounds.maximum.x/settings_.tileSizeMeters));
    const auto minZ=static_cast<std::int32_t>(std::floor(bounds.minimum.z/settings_.tileSizeMeters));
    const auto maxZ=static_cast<std::int32_t>(std::floor(bounds.maximum.z/settings_.tileSizeMeters));
    const std::int64_t width=static_cast<std::int64_t>(maxX)-static_cast<std::int64_t>(minX)+1LL;
    const std::int64_t depth=static_cast<std::int64_t>(maxZ)-static_cast<std::int64_t>(minZ)+1LL;
    if(width<=0LL||depth<=0LL||width>1'000'000LL||depth>1'000'000LL||
       width*depth>1'000'000LL)return;
    for(std::int32_t z=minZ;;++z){
        for(std::int32_t x=minX;;++x){dirty_.insert({x,z});if(x==maxX)break;}
        if(z==maxZ)break;
    }
    ++telemetry_.editsObserved;telemetry_.dirtyTileCount=dirty_.size();
}

void DynamicNavigationWorld::mark_voxel_edit(Int3 minimumVoxel,Int3 maximumVoxel,float metersPerVoxel){
    if(!std::isfinite(metersPerVoxel)||metersPerVoxel<=0.0F)return;
    NavigationBounds bounds{{minimumVoxel.x*metersPerVoxel,minimumVoxel.y*metersPerVoxel,minimumVoxel.z*metersPerVoxel},
                            {(maximumVoxel.x+1)*metersPerVoxel,(maximumVoxel.y+1)*metersPerVoxel,(maximumVoxel.z+1)*metersPerVoxel}};
    mark_dirty(bounds);
}

bool DynamicNavigationWorld::replace_region(const NavigationBounds& bounds,
    std::span<const NavigationTriangle> replacement,std::string* error){
    if(!finite(bounds.minimum)||!finite(bounds.maximum))return fail(error,"replacement bounds are invalid");
    source_.erase(std::remove_if(source_.begin(),source_.end(),[&](const auto& triangle){return overlaps(triangle_bounds(triangle),bounds);}),source_.end());
    source_.insert(source_.end(),replacement.begin(),replacement.end());mark_dirty(bounds);return true;
}

bool DynamicNavigationWorld::rebuild_dirty(std::string* error){
    if(dirty_.empty())return true;
    std::vector<NavigationTriangle> candidates;
    candidates.reserve(source_.size());
    for(const auto& triangle:source_){
        const auto bounds=triangle_bounds(triangle);
        const auto minX=static_cast<std::int32_t>(std::floor(bounds.minimum.x/settings_.tileSizeMeters));
        const auto maxX=static_cast<std::int32_t>(std::floor(bounds.maximum.x/settings_.tileSizeMeters));
        const auto minZ=static_cast<std::int32_t>(std::floor(bounds.minimum.z/settings_.tileSizeMeters));
        const auto maxZ=static_cast<std::int32_t>(std::floor(bounds.maximum.z/settings_.tileSizeMeters));
        const std::int64_t tileWidth=static_cast<std::int64_t>(maxX)-
            static_cast<std::int64_t>(minX)+1LL;
        const std::int64_t tileDepth=static_cast<std::int64_t>(maxZ)-
            static_cast<std::int64_t>(minZ)+1LL;
        const bool wideRange=tileWidth>4096LL||tileDepth>4096LL;
        const std::int64_t tileCount=wideRange?4097LL:tileWidth*tileDepth;
        bool touchesDirty=false;
        if(tileCount<=4096){
            for(std::int32_t z=minZ;!touchesDirty;){
                for(std::int32_t x=minX;;++x){
                    if(dirty_.contains({x,z})){touchesDirty=true;break;}
                    if(x==maxX)break;
                }
                if(z==maxZ)break;
                ++z;
            }
        }else{
            touchesDirty=std::any_of(dirty_.begin(),dirty_.end(),[&](const auto& tile){
                return tile.x>=minX&&tile.x<=maxX&&tile.z>=minZ&&tile.z<=maxZ;
            });
        }
        if(touchesDirty)candidates.push_back(triangle);
    }
    auto local=build_navigation_mesh(candidates,settings_);
    if(!local.success()){
        ++telemetry_.rejectedRebuilds;
        return fail(error,local.diagnostics.empty()?"dynamic navigation tile rebuild failed":
            local.diagnostics.back().message);
    }
    NavigationMesh replacement;replacement.settings=settings_;replacement.offMeshLinks=links_;
    replacement.polygons.reserve(mesh_.polygons.size()+local.mesh.polygons.size());
    std::size_t reused=0U;
    for(auto polygon:mesh_.polygons){
        if(dirty_.contains({polygon.tileX,polygon.tileZ}))continue;
        polygon.portals.clear();replacement.polygons.push_back(std::move(polygon));++reused;
    }
    std::size_t rebuilt=0U;
    for(auto polygon:local.mesh.polygons){
        if(!dirty_.contains({polygon.tileX,polygon.tileZ}))continue;
        polygon.portals.clear();replacement.polygons.push_back(std::move(polygon));++rebuilt;
    }
    if(!stitch_navigation_tiles(replacement,error)){
        ++telemetry_.rejectedRebuilds;return false;
    }
    const std::size_t rebuiltTiles=dirty_.size();
    mesh_=std::move(replacement);dirty_.clear();++telemetry_.revision;++telemetry_.rebuilds;
    telemetry_.dirtyTileCount=0U;telemetry_.polygonCount=mesh_.polygons.size();
    telemetry_.lastRebuiltTileCount=rebuiltTiles;
    telemetry_.lastCandidateTriangleCount=candidates.size();
    telemetry_.lastReusedPolygonCount=reused;
    telemetry_.lastRebuiltPolygonCount=rebuilt;return true;
}

std::vector<NavigationDirtyTile> DynamicNavigationWorld::dirty_tiles() const{return {dirty_.begin(),dirty_.end()};}

bool build_dvepak(const std::filesystem::path& root,std::span<const std::filesystem::path> inputs,
    const std::filesystem::path& output,const DvePakBuildOptions& options,DvePakManifest* manifest,std::string* error){
    struct Pending{std::string path;std::vector<std::byte> bytes;std::uint64_t hash{};};std::vector<Pending> pending;
    for(const auto& inputPath:inputs){
        std::error_code ec;auto absolute=inputPath.is_absolute()?inputPath:root/inputPath;
        auto relative=std::filesystem::relative(absolute,root,ec);if(ec)return fail(error,"package input is outside root: "+absolute.string());
        const auto normalized=normalize_package_path(relative);
        if(normalized.empty()||path_has_prefix(normalized,"../")||normalized=="..")return fail(error,"unsafe package path: "+normalized);
        if(options.stripEditorOnly&&std::any_of(options.editorOnlyPrefixes.begin(),options.editorOnlyPrefixes.end(),[&](const auto& p){return path_has_prefix(normalized,p);}))continue;
        auto bytes=read_file_bytes(absolute,error);if(!bytes)return false;pending.push_back({normalized,std::move(*bytes),0U});pending.back().hash=fnv_bytes(pending.back().bytes);
    }
    std::sort(pending.begin(),pending.end(),[](const auto& a,const auto& b){return a.path<b.path;});
    if(std::adjacent_find(pending.begin(),pending.end(),[](const auto& a,const auto& b){return a.path==b.path;})!=pending.end())return fail(error,"duplicate path in package");
    DvePakManifest built;std::uint64_t packageHash=1469598103934665603ULL,directoryBytes=0U;
    for(const auto& item:pending){fnv_mix(packageHash,item.path);for(unsigned shift=0;shift<64;shift+=8){packageHash^=static_cast<std::uint8_t>(item.hash>>shift);packageHash*=1099511628211ULL;}directoryBytes+=4U+8U+8U+8U+item.path.size();}
    built.packageHash=packageHash;built.entries.resize(pending.size());
    const std::uint64_t headerBytes=8U+4U+4U+8U+8U;std::uint64_t offset=headerBytes+directoryBytes;
    for(std::size_t i=0;i<pending.size();++i){built.entries[i]={pending[i].path,offset,pending[i].bytes.size(),pending[i].hash};offset+=pending[i].bytes.size();}
    if(options.incremental&&std::filesystem::exists(output)){
        std::string ignored;auto existing=inspect_dvepak(output,&ignored);
        if(existing&&existing->packageHash==built.packageHash){if(manifest)*manifest=*existing;return true;}
    }
    std::filesystem::create_directories(output.parent_path());auto temporary=output;temporary+=".tmp";
    std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);if(!stream)return fail(error,"could not create package");
    stream.write(kPakMagic.data(),kPakMagic.size());const std::uint32_t version=1U,count=static_cast<std::uint32_t>(pending.size());
    if(!write_value(stream,version)||!write_value(stream,count)||!write_value(stream,directoryBytes)||!write_value(stream,packageHash))return fail(error,"could not write package header");
    for(const auto& entry:built.entries){const auto length=static_cast<std::uint32_t>(entry.path.size());
        if(!write_value(stream,length)||!write_value(stream,entry.offset)||!write_value(stream,entry.size)||!write_value(stream,entry.contentHash))return fail(error,"could not write package directory");
        stream.write(entry.path.data(),length);if(!stream)return fail(error,"could not write package path");}
    for(const auto& item:pending)if(!item.bytes.empty()){stream.write(reinterpret_cast<const char*>(item.bytes.data()),static_cast<std::streamsize>(item.bytes.size()));if(!stream)return fail(error,"could not write package payload");}
    stream.close();if(!stream)return fail(error,"could not finalize package");
    std::error_code ec;std::filesystem::rename(temporary,output,ec);if(ec){std::filesystem::remove(output,ec);ec.clear();std::filesystem::rename(temporary,output,ec);}if(ec)return fail(error,"could not publish package: "+ec.message());
    if(manifest)*manifest=std::move(built);
    return true;
}

std::optional<DvePakManifest> inspect_dvepak(const std::filesystem::path& package,std::string* error){
    std::ifstream stream(package,std::ios::binary|std::ios::ate);if(!stream)return fail(error,"could not open package"),std::nullopt;
    const auto end=stream.tellg();if(end<static_cast<std::streamoff>(32))return fail(error,"package is truncated"),std::nullopt;stream.seekg(0);
    std::array<char,8> magic{};stream.read(magic.data(),magic.size());if(magic!=kPakMagic)return fail(error,"invalid package magic"),std::nullopt;
    DvePakManifest result;std::uint32_t count{};std::uint64_t directoryBytes{};
    if(!read_value(stream,result.version)||!read_value(stream,count)||!read_value(stream,directoryBytes)||!read_value(stream,result.packageHash)||result.version!=1U||count>1'000'000U)
        return fail(error,"invalid package header"),std::nullopt;
    result.entries.reserve(count);std::string previous;
    for(std::uint32_t i=0;i<count;++i){std::uint32_t length{};DvePakEntry entry;
        if(!read_value(stream,length)||length==0U||length>1U*1024U*1024U||!read_value(stream,entry.offset)||!read_value(stream,entry.size)||!read_value(stream,entry.contentHash))return fail(error,"invalid package directory"),std::nullopt;
        entry.path.resize(length);stream.read(entry.path.data(),length);if(!stream||entry.path<=previous||path_has_prefix(entry.path,"../"))return fail(error,"unsafe or unsorted package directory"),std::nullopt;
        if(entry.offset>static_cast<std::uint64_t>(end)||entry.size>static_cast<std::uint64_t>(end)-entry.offset)return fail(error,"package entry exceeds file"),std::nullopt;
        previous=entry.path;result.entries.push_back(std::move(entry));}
    return result;
}

bool DvePakMount::mount(const std::filesystem::path& package,std::string* error){auto parsed=inspect_dvepak(package,error);if(!parsed)return false;package_=package;manifest_=std::move(*parsed);return true;}
bool DvePakMount::contains(std::string_view path) const noexcept {return std::any_of(manifest_.entries.begin(),manifest_.entries.end(),[&](const auto& e){return e.path==path;});}
std::optional<std::vector<std::byte>> DvePakMount::read(std::string_view path,std::string* error) const{
    const auto it=std::lower_bound(manifest_.entries.begin(),manifest_.entries.end(),path,[](const auto& e,std::string_view p){return e.path<p;});
    if(it==manifest_.entries.end()||it->path!=path)return fail(error,"package entry not found"),std::nullopt;
    std::ifstream stream(package_,std::ios::binary);if(!stream)return fail(error,"could not open mounted package"),std::nullopt;
    stream.seekg(static_cast<std::streamoff>(it->offset));std::vector<std::byte> bytes(static_cast<std::size_t>(it->size));
    if(!bytes.empty()&&!stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))return fail(error,"could not read package entry"),std::nullopt;
    if(fnv_bytes(bytes)!=it->contentHash)return fail(error,"package entry integrity check failed"),std::nullopt;
    return bytes;
}

struct Profiler::Impl{mutable std::mutex mutex;bool enabled{true};std::uint64_t frame{};std::vector<ProfileEvent> events;std::map<std::string,double,std::less<>> counters;std::map<std::string,std::int64_t,std::less<>> memory;};
Profiler::Profiler():impl_(std::make_unique<Impl>()){}Profiler::~Profiler()=default;
Profiler& Profiler::instance(){static Profiler profiler;return profiler;}
void Profiler::set_enabled(bool enabled)noexcept{std::scoped_lock lock(impl_->mutex);impl_->enabled=enabled;}
bool Profiler::enabled()const noexcept{std::scoped_lock lock(impl_->mutex);return impl_->enabled;}
void Profiler::begin_frame(std::uint64_t frameIndex){std::scoped_lock lock(impl_->mutex);impl_->frame=frameIndex;impl_->events.clear();impl_->counters.clear();}
ProfileFrame Profiler::end_frame(){std::scoped_lock lock(impl_->mutex);return {impl_->frame,impl_->events,impl_->counters,impl_->memory};}
void Profiler::record(ProfileEvent event){std::scoped_lock lock(impl_->mutex);if(impl_->enabled)impl_->events.push_back(std::move(event));}
void Profiler::set_counter(std::string name,double value){std::scoped_lock lock(impl_->mutex);if(impl_->enabled)impl_->counters[std::move(name)]=value;}
void Profiler::add_memory(std::string category,std::int64_t deltaBytes){std::scoped_lock lock(impl_->mutex);if(impl_->enabled)impl_->memory[std::move(category)]+=deltaBytes;}
std::string Profiler::frame_json(const ProfileFrame& frame)const{std::ostringstream out;out<<"{\"frame\":"<<frame.frameIndex<<",\"events\":[";for(std::size_t i=0;i<frame.events.size();++i){const auto& e=frame.events[i];if(i)out<<',';out<<"{\"name\":\""<<json_escape(e.name)<<"\",\"thread\":"<<e.threadId<<",\"start_ns\":"<<e.startNanoseconds<<",\"duration_ns\":"<<e.durationNanoseconds<<'}';}out<<"],\"counters\":{";bool first=true;for(const auto& [name,value]:frame.counters){if(!first)out<<',';first=false;out<<'"'<<json_escape(name)<<"\":"<<std::setprecision(17)<<value;}out<<"},\"memory_bytes\":{";first=true;for(const auto& [name,value]:frame.memoryBytes){if(!first)out<<',';first=false;out<<'"'<<json_escape(name)<<"\":"<<value;}out<<"}}";return out.str();}
ProfileScope::ProfileScope(std::string_view name)noexcept:name_(name),start_(std::chrono::steady_clock::now()),active_(Profiler::instance().enabled()){}
ProfileScope::~ProfileScope(){if(!active_)return;const auto end=std::chrono::steady_clock::now();const auto startNs=std::chrono::duration_cast<std::chrono::nanoseconds>(start_.time_since_epoch()).count();const auto duration=std::chrono::duration_cast<std::chrono::nanoseconds>(end-start_).count();Profiler::instance().record({name_,std::hash<std::thread::id>{}(std::this_thread::get_id()),static_cast<std::uint64_t>(startNs),static_cast<std::uint64_t>(duration)});}

bool AssetDependencyGraph::upsert(AssetDependencyNode node,std::string* error){if(node.id.empty())return fail(error,"asset id is empty");std::sort(node.dependencies.begin(),node.dependencies.end());node.dependencies.erase(std::unique(node.dependencies.begin(),node.dependencies.end()),node.dependencies.end());if(std::binary_search(node.dependencies.begin(),node.dependencies.end(),node.id))return fail(error,"asset cannot depend on itself");auto it=nodes_.find(node.id);if(it!=nodes_.end())node.generation=it->second.generation+1U;nodes_[node.id]=std::move(node);return true;}
bool AssetDependencyGraph::erase(std::string_view id){return nodes_.erase(std::string(id))!=0U;}
bool AssetDependencyGraph::rename(std::string_view oldId,std::string newId,std::string* error){if(newId.empty()||nodes_.contains(newId))return fail(error,"new asset id is invalid or already exists");auto node=nodes_.extract(std::string(oldId));if(node.empty())return fail(error,"asset to rename was not found");node.key()=newId;node.mapped().id=newId;++node.mapped().generation;nodes_.insert(std::move(node));for(auto& [id,item]:nodes_)for(auto& dep:item.dependencies)if(dep==oldId){dep=newId;++item.generation;}return true;}
std::vector<AssetDependencyIssue> AssetDependencyGraph::validate()const{std::vector<AssetDependencyIssue> out;for(const auto& [id,node]:nodes_)for(const auto& dependency:node.dependencies)if(!nodes_.contains(dependency))out.push_back({id,dependency,"missing dependency"});std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::tie(a.asset,a.dependency)<std::tie(b.asset,b.dependency);});return out;}
std::vector<std::string> AssetDependencyGraph::dependency_closure(
    std::span<const std::string> roots, std::string* error) const {
    enum class Visit : std::uint8_t { Active, Complete };
    std::map<std::string, Visit, std::less<>> visited;
    std::vector<std::string> order;
    std::function<bool(const std::string&)> visit = [&](const std::string& id) {
        const auto state = visited.find(id);
        if (state != visited.end()) {
            if (state->second == Visit::Active) return fail(error, "asset dependency cycle");
            return true;
        }
        const auto node = nodes_.find(id);
        if (node == nodes_.end()) return fail(error, "missing asset dependency: " + id);
        visited.emplace(id, Visit::Active);
        for (const std::string& dependency : node->second.dependencies)
            if (!visit(dependency)) return false;
        visited[id] = Visit::Complete;
        order.push_back(id);
        return true;
    };
    std::vector<std::string> sortedRoots(roots.begin(), roots.end());
    std::sort(sortedRoots.begin(), sortedRoots.end());
    sortedRoots.erase(std::unique(sortedRoots.begin(), sortedRoots.end()), sortedRoots.end());
    for (const std::string& root : sortedRoots)
        if (!visit(root)) return {};
    return order;
}
std::vector<std::string> AssetDependencyGraph::deterministic_reimport_order(std::span<const std::string> changed,std::string* error)const{
    std::set<std::string> affected;for(const auto& id:changed)if(nodes_.contains(id))affected.insert(id);bool grew=true;while(grew){grew=false;for(const auto& [id,node]:nodes_)if(!affected.contains(id)&&std::any_of(node.dependencies.begin(),node.dependencies.end(),[&](const auto& dep){return affected.contains(dep);})){affected.insert(id);grew=true;}}
    std::map<std::string,std::size_t,std::less<>> indegree;for(const auto& id:affected)indegree[id]=0U;for(const auto& id:affected)for(const auto& dep:nodes_.at(id).dependencies)if(affected.contains(dep))++indegree[id];std::set<std::string> ready;for(const auto& [id,count]:indegree)if(count==0U)ready.insert(id);std::vector<std::string> out;while(!ready.empty()){auto id=*ready.begin();ready.erase(ready.begin());out.push_back(id);for(const auto& candidate:affected){const auto& deps=nodes_.at(candidate).dependencies;if(std::find(deps.begin(),deps.end(),id)!=deps.end()&&--indegree[candidate]==0U)ready.insert(candidate);}}if(out.size()!=affected.size()){fail(error,"asset dependency cycle prevents deterministic reimport");return {};}return out;
}
const AssetDependencyNode* AssetDependencyGraph::find(std::string_view id)const noexcept{const auto it=nodes_.find(id);return it==nodes_.end()?nullptr:&it->second;}

std::vector<SourceFingerprint> SourceMonitor::poll(std::span<const std::filesystem::path> files,bool hashContents){std::vector<SourceFingerprint> changed;for(const auto& path:files){std::error_code ec;if(!std::filesystem::is_regular_file(path,ec))continue;SourceFingerprint current;current.path=path;current.size=std::filesystem::file_size(path,ec);if(ec)continue;current.modifiedTicks=std::filesystem::last_write_time(path,ec).time_since_epoch().count();if(ec)continue;if(hashContents){auto bytes=read_file_bytes(path,nullptr);if(!bytes)continue;current.contentHash=fnv_bytes(*bytes);}const auto it=known_.find(path);if(it==known_.end()||it->second.size!=current.size||it->second.modifiedTicks!=current.modifiedTicks||it->second.contentHash!=current.contentHash)changed.push_back(current);known_[path]=current;}std::sort(changed.begin(),changed.end(),[](const auto& a,const auto& b){return a.path.generic_string()<b.path.generic_string();});return changed;}

bool InputActionSystem::set_context(InputContext context,std::string* error){if(context.name.empty())return fail(error,"input context name is empty");for(const auto& binding:context.bindings)if(binding.action.empty()||binding.primary.empty()||!std::isfinite(binding.scale)||!std::isfinite(binding.holdSeconds)||!std::isfinite(binding.doubleTapSeconds))return fail(error,"input binding is invalid");contexts_[context.name]=std::move(context);return true;}
bool InputActionSystem::remove_context(std::string_view name){return contexts_.erase(std::string(name))!=0U;}
void InputActionSystem::begin_frame(float deltaSeconds){if(!std::isfinite(deltaSeconds)||deltaSeconds<0.0F)deltaSeconds=0.0F;actions_.clear();for(auto& [name,state]:history_){state.previous=state.current;state.current=controls_[name];if(state.current>0.5F)state.heldSeconds+=deltaSeconds;else{if(state.previous>0.5F){state.sinceRelease=0.0F;state.releasedHeldSeconds=state.heldSeconds;}else state.sinceRelease+=deltaSeconds;state.heldSeconds=0.0F;}}for(const auto& [name,value]:controls_)if(!history_.contains(name)){auto& h=history_[name];h.current=value;h.previous=0.0F;h.heldSeconds=value>0.5F?deltaSeconds:0.0F;}
    std::vector<const InputContext*> ordered;for(const auto& [name,context]:contexts_)if(context.enabled)ordered.push_back(&context);std::sort(ordered.begin(),ordered.end(),[](auto* a,auto* b){return a->priority!=b->priority?a->priority>b->priority:a->name<b->name;});std::set<std::string> consumed;
    for(const auto* context:ordered)for(const auto& binding:context->bindings){if(consumed.contains(binding.primary))continue;const auto& h=history_[binding.primary];const bool chord=std::all_of(binding.chord.begin(),binding.chord.end(),[&](const auto& key){return history_[key].current>0.5F;});if(!chord)continue;const bool down=h.current>0.5F,pressed=down&&h.previous<=0.5F,released=!down&&h.previous>0.5F;bool fire=false,held=false;switch(binding.trigger){case InputTrigger::Press:fire=pressed;break;case InputTrigger::Release:fire=released;break;case InputTrigger::Hold:fire=held=down&&h.heldSeconds>=binding.holdSeconds;break;case InputTrigger::Tap:fire=released&&h.releasedHeldSeconds<=binding.holdSeconds;break;case InputTrigger::DoubleTap:fire=pressed&&h.sinceRelease<=binding.doubleTapSeconds;break;}if(fire||down){auto& action=actions_[binding.action];action.value+=h.current*binding.scale;action.pressed|=fire&&(binding.trigger==InputTrigger::Press||binding.trigger==InputTrigger::DoubleTap||binding.trigger==InputTrigger::Tap);action.released|=fire&&binding.trigger==InputTrigger::Release;action.held|=held;if(context->consume)consumed.insert(binding.primary);}}
}
const InputActionState* InputActionSystem::action(std::string_view name)const noexcept{const auto it=actions_.find(name);return it==actions_.end()?nullptr:&it->second;}
std::vector<std::string> InputActionSystem::conflicts(const InputBinding& candidate)const{std::vector<std::string> out;for(const auto& [name,context]:contexts_)for(const auto& binding:context.bindings)if(binding.primary==candidate.primary&&binding.chord==candidate.chord)out.push_back(name+":"+binding.action);return out;}
bool InputActionSystem::rebind(std::string_view contextName,std::string_view actionName,InputBinding replacement,bool allowConflict,std::string* error){auto it=contexts_.find(contextName);if(it==contexts_.end())return fail(error,"input context was not found");if(!allowConflict&&!conflicts(replacement).empty())return fail(error,"input binding conflicts with an existing binding");for(auto& binding:it->second.bindings)if(binding.action==actionName){replacement.action=std::string(actionName);binding=std::move(replacement);return true;}return fail(error,"input action was not found");}
bool InputActionSystem::save_bindings(const std::filesystem::path& path,std::string* error)const{std::ofstream out(path,std::ios::trunc);if(!out)return fail(error,"could not save input bindings");for(const auto& [name,context]:contexts_)for(const auto& binding:context.bindings){auto valid=[](std::string_view s){return s.find_first_of("\t\r\n")==std::string_view::npos;};if(!valid(name)||!valid(binding.action)||!valid(binding.primary))return fail(error,"input binding contains an unsupported control character");out<<name<<'\t'<<context.priority<<'\t'<<context.enabled<<'\t'<<context.consume<<'\t'<<binding.action<<'\t'<<binding.primary<<'\t'<<static_cast<int>(binding.trigger)<<'\t'<<binding.holdSeconds<<'\t'<<binding.doubleTapSeconds<<'\t'<<binding.scale<<'\t';for(std::size_t i=0;i<binding.chord.size();++i){if(i)out<<',';out<<binding.chord[i];}out<<'\n';}return static_cast<bool>(out)||fail(error,"could not finalize input bindings");}
bool InputActionSystem::load_bindings(const std::filesystem::path& path,std::string* error){std::ifstream in(path);if(!in)return fail(error,"could not load input bindings");std::map<std::string,InputContext,std::less<>> loaded;std::string line;while(std::getline(in,line)){std::vector<std::string> fields;std::size_t start=0;for(;;){const auto tab=line.find('\t',start);fields.push_back(line.substr(start,tab-start));if(tab==std::string::npos)break;start=tab+1;}if(fields.size()!=11U)return fail(error,"malformed input binding file");try{auto& context=loaded[fields[0]];context.name=fields[0];context.priority=std::stoi(fields[1]);context.enabled=std::stoi(fields[2])!=0;context.consume=std::stoi(fields[3])!=0;InputBinding binding;binding.action=fields[4];binding.primary=fields[5];binding.trigger=static_cast<InputTrigger>(std::stoi(fields[6]));binding.holdSeconds=std::stof(fields[7]);binding.doubleTapSeconds=std::stof(fields[8]);binding.scale=std::stof(fields[9]);std::size_t pos=0;while(pos<fields[10].size()){const auto comma=fields[10].find(',',pos);binding.chord.push_back(fields[10].substr(pos,comma-pos));if(comma==std::string::npos)break;pos=comma+1;}context.bindings.push_back(std::move(binding));}catch(...){return fail(error,"malformed numeric input binding field");}}contexts_=std::move(loaded);return true;}

bool SaveGameStore::register_migration(std::uint32_t fromVersion,SaveMigration migration,std::string* error){if(fromVersion>=currentVersion_||!migration||migrations_.contains(fromVersion))return fail(error,"save migration registration is invalid");migrations_[fromVersion]=std::move(migration);return true;}
bool SaveGameStore::write_atomic(const std::filesystem::path& slot,SaveGameDocument document,std::string* error)const{document.schemaVersion=currentVersion_;std::filesystem::create_directories(slot.parent_path());auto temp=slot;temp+=".tmp";std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out)return fail(error,"could not create save file");out.write(kSaveMagic.data(),kSaveMagic.size());const std::uint32_t count=static_cast<std::uint32_t>(document.sections.size());std::uint64_t hash=1469598103934665603ULL;for(const auto& [name,bytes]:document.sections){fnv_mix(hash,name);const auto sectionHash=fnv_bytes(bytes);for(unsigned shift=0;shift<64;shift+=8){hash^=static_cast<std::uint8_t>(sectionHash>>shift);hash*=1099511628211ULL;}}if(!write_value(out,document.schemaVersion)||!write_value(out,document.sequence)||!write_value(out,count)||!write_value(out,hash))return fail(error,"could not write save header");for(const auto& [name,bytes]:document.sections){const auto length=static_cast<std::uint32_t>(name.size());const auto size=static_cast<std::uint64_t>(bytes.size());const auto sectionHash=fnv_bytes(bytes);if(!write_value(out,length)||!write_value(out,size)||!write_value(out,sectionHash))return fail(error,"could not write save directory");out.write(name.data(),length);if(!bytes.empty())out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!out)return fail(error,"could not write save payload");}out.close();if(!out)return fail(error,"could not finalize save");std::error_code ec;auto backup=slot;backup+=".bak";if(std::filesystem::exists(slot)){std::filesystem::remove(backup,ec);ec.clear();std::filesystem::rename(slot,backup,ec);if(ec)return fail(error,"could not rotate save backup");}std::filesystem::rename(temp,slot,ec);if(ec)return fail(error,"could not publish save: "+ec.message());return true;}
std::optional<SaveGameDocument> SaveGameStore::read_recover(const std::filesystem::path& slot,std::string* error)const{auto load=[&](const std::filesystem::path& path)->std::optional<SaveGameDocument>{std::ifstream in(path,std::ios::binary);if(!in)return std::nullopt;std::array<char,8> magic{};in.read(magic.data(),magic.size());SaveGameDocument doc;std::uint32_t count{};std::uint64_t expected{};if(magic!=kSaveMagic||!read_value(in,doc.schemaVersion)||!read_value(in,doc.sequence)||!read_value(in,count)||!read_value(in,expected)||count>100000U)return std::nullopt;std::uint64_t actual=1469598103934665603ULL;for(std::uint32_t i=0;i<count;++i){std::uint32_t length{};std::uint64_t size{},sectionHash{};if(!read_value(in,length)||!read_value(in,size)||!read_value(in,sectionHash)||length==0U||length>1U*1024U*1024U||size>1ULL*1024ULL*1024ULL*1024ULL)return std::nullopt;std::string name(length,'\0');in.read(name.data(),length);std::vector<std::byte> bytes(static_cast<std::size_t>(size));if(!bytes.empty())in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!in||fnv_bytes(bytes)!=sectionHash||doc.sections.contains(name))return std::nullopt;fnv_mix(actual,name);for(unsigned shift=0;shift<64;shift+=8){actual^=static_cast<std::uint8_t>(sectionHash>>shift);actual*=1099511628211ULL;}doc.sections.emplace(std::move(name),std::move(bytes));}if(actual!=expected)return std::nullopt;return doc;};auto document=load(slot);if(!document){auto backup=slot;backup+=".bak";document=load(backup);}if(!document)return fail(error,"save and backup are unreadable"),std::nullopt;if(document->schemaVersion>currentVersion_)return fail(error,"save was written by a newer schema"),std::nullopt;while(document->schemaVersion<currentVersion_){const auto it=migrations_.find(document->schemaVersion);if(it==migrations_.end())return fail(error,"missing save migration"),std::nullopt;const auto before=document->schemaVersion;if(!it->second(*document,error))return std::nullopt;if(document->schemaVersion!=before+1U)return fail(error,"save migration did not advance exactly one version"),std::nullopt;}return document;}

AnimationValidationResult validate_humanoid_rig(const SkeletonAsset& skeleton,const HumanoidRigMap& rig)noexcept{auto base=validate_skeleton(skeleton);if(!base)return base;if(!std::isfinite(rig.referenceHeightMeters)||rig.referenceHeightMeters<=0.0F)return {false,"humanoid reference height must be positive"};std::set<BoneIndex> used;for(const auto& [role,bone]:rig.bones){(void)role;if(bone>=skeleton.bones.size())return {false,"humanoid rig bone is outside skeleton"};if(!used.insert(bone).second)return {false,"humanoid rig maps one bone to multiple roles"};}for(auto required:{HumanoidBone::Hips,HumanoidBone::Head,HumanoidBone::LeftFoot,HumanoidBone::RightFoot})if(!rig.bones.contains(required))return {false,"humanoid rig is missing a required role"};return {true,{}};}
LocalPose retarget_humanoid_pose(const SkeletonAsset& sourceSkeleton,std::span<const RigidTransform> sourcePose,const HumanoidRigMap& sourceRig,const SkeletonAsset& targetSkeleton,const HumanoidRigMap& targetRig,std::string* error){if(!validate_humanoid_rig(sourceSkeleton,sourceRig)||!validate_humanoid_rig(targetSkeleton,targetRig)||sourcePose.size()!=sourceSkeleton.bones.size()){fail(error,"retarget inputs are invalid");return{};}auto out=make_bind_pose(targetSkeleton);const float scale=targetRig.referenceHeightMeters/sourceRig.referenceHeightMeters;for(const auto& [role,targetBone]:targetRig.bones){const auto it=sourceRig.bones.find(role);if(it==sourceRig.bones.end())continue;out[targetBone].rotation=sourcePose[it->second].rotation;if(role==HumanoidBone::Hips)out[targetBone].position=mul(sourcePose[it->second].position,scale);}return out;}
bool apply_morph_targets(std::span<const SkinVertex> base,std::span<const MorphTarget> targets,const std::map<std::string,float,std::less<>>& weights,std::vector<SkinVertex>& output,std::string* error){output.assign(base.begin(),base.end());for(const auto& target:targets){const auto weightIt=weights.find(target.name);if(weightIt==weights.end()||std::abs(weightIt->second)<=kEpsilon)continue;if(!std::isfinite(weightIt->second))return fail(error,"morph weight is not finite");for(const auto& delta:target.deltas){if(delta.vertex>=output.size()||!finite(delta.position)||!finite(delta.normal))return fail(error,"morph delta is invalid");output[delta.vertex].position=add(output[delta.vertex].position,mul(delta.position,weightIt->second));output[delta.vertex].normal=normalize(add(output[delta.vertex].normal,mul(delta.normal,weightIt->second)));}}return true;}
bool solve_ccd_ik(const SkeletonAsset& skeleton,LocalPose& pose,const CcdIkRequest& request,std::string* error){if(request.chain.size()<2U||pose.size()!=skeleton.bones.size()||request.iterations==0U||!finite(request.targetModel)||!std::isfinite(request.weight)||request.weight<0.0F||request.weight>1.0F)return fail(error,"CCD IK request is invalid");for(auto bone:request.chain)if(bone>=pose.size())return fail(error,"CCD IK chain contains an invalid bone");const BoneIndex endBone=request.chain.back();for(std::uint32_t iteration=0;iteration<request.iterations;++iteration){auto model=compute_model_pose(skeleton,pose,error);if(model.empty())return false;if(distance(model[endBone].position,request.targetModel)<=request.toleranceMeters)return true;for(std::size_t reverse=request.chain.size()-1U;reverse-->0U;){const auto bone=request.chain[reverse];model=compute_model_pose(skeleton,pose,error);const Float3 joint=model[bone].position,toEnd=normalize(sub(model[endBone].position,joint)),toTarget=normalize(sub(request.targetModel,joint));const float cosine=std::clamp(dot(toEnd,toTarget),-1.0F,1.0F);if(cosine>0.999999F)continue;Float3 axis=cross(toEnd,toTarget);if(length_squared(axis)<kEpsilon)continue;const auto delta=quaternion_axis_angle(axis,std::acos(cosine)*request.weight);pose[bone].rotation=quaternion_multiply(delta,pose[bone].rotation);}}return true;}

bool PhysicsMaterialLibrary::set(std::uint16_t id,PhysicsMaterial material,std::string* error){if(material.name.empty()||!std::isfinite(material.friction)||material.friction<0.0F||!std::isfinite(material.restitution)||material.restitution<0.0F||material.restitution>1.0F||!std::isfinite(material.densityKilogramsPerCubicMeter)||material.densityKilogramsPerCubicMeter<=0.0F)return fail(error,"physics material is invalid");materials_[id]=std::move(material);return true;}
const PhysicsMaterial* PhysicsMaterialLibrary::get(std::uint16_t id)const noexcept{const auto it=materials_.find(id);return it==materials_.end()?nullptr:&it->second;}
std::vector<PhysicsQueryHit> filter_and_sort_physics_hits(std::span<const PhysicsQueryHit> hits,const PhysicsQueryFilter& filter){std::vector<PhysicsQueryHit> out;for(const auto& hit:hits)if(hit.body!=kInvalidRigidBodyHandle&&!filter.ignoredBodies.contains(hit.body)&&(filter.acceptedMaterials.empty()||filter.acceptedMaterials.contains(hit.material))&&std::isfinite(hit.fraction)&&std::isfinite(hit.distance))out.push_back(hit);std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::tie(a.fraction,a.distance,a.body,a.subShape)<std::tie(b.fraction,b.distance,b.body,b.subShape);});return out;}

const BlackboardValue* Blackboard::get(std::string_view key)const noexcept{const auto it=values_.find(key);return it==values_.end()?nullptr:&it->second;}bool Blackboard::erase(std::string_view key){return values_.erase(std::string(key))!=0U;}
BehaviorTree::NodeId BehaviorTree::add(Node node){nodes_.push_back(std::move(node));return static_cast<NodeId>(nodes_.size()-1U);}void BehaviorTree::reset()noexcept{runningChild_.clear();}
BehaviorStatus BehaviorTree::tick(Blackboard& blackboard,float deltaSeconds){if(nodes_.empty()||root_>=nodes_.size())return BehaviorStatus::Failure;return tick_node(root_,blackboard,deltaSeconds);}
BehaviorStatus BehaviorTree::tick_node(NodeId id,Blackboard& blackboard,float deltaSeconds){if(id>=nodes_.size())return BehaviorStatus::Failure;auto& node=nodes_[id];if(node.kind==Kind::Condition)return node.condition&&node.condition(blackboard)?BehaviorStatus::Success:BehaviorStatus::Failure;if(node.kind==Kind::Action)return node.action?node.action(blackboard,deltaSeconds):BehaviorStatus::Failure;auto& cursor=runningChild_[id];while(cursor<node.children.size()){const auto result=tick_node(node.children[cursor],blackboard,deltaSeconds);if(result==BehaviorStatus::Running)return result;if(node.kind==Kind::Sequence&&result==BehaviorStatus::Failure){cursor=0U;return result;}if(node.kind==Kind::Selector&&result==BehaviorStatus::Success){cursor=0U;return result;}++cursor;}cursor=0U;return node.kind==Kind::Sequence?BehaviorStatus::Success:BehaviorStatus::Failure;}
Float3 steering_arrive(const SteeringRequest& request)noexcept{const Float3 delta=sub(request.target,request.position);const float d=length(delta);if(d<kEpsilon)return mul(request.velocity,-1.0F);const float speed=request.maximumSpeed*std::clamp(d/std::max(request.slowdownRadius,kEpsilon),0.0F,1.0F);Float3 acceleration=sub(mul(delta,speed/d),request.velocity);const float magnitude=length(acceleration);if(magnitude>request.maximumAcceleration)acceleration=mul(acceleration,request.maximumAcceleration/magnitude);return acceleration;}
std::vector<PerceptionHit> query_perception(Float3 origin,Float3 forward,float radius,float halfAngleRadians,std::span<const PerceptionCandidate> candidates,const std::function<bool(Float3,Float3)>& visible){std::vector<PerceptionHit> out;if(radius<=0.0F||halfAngleRadians<0.0F)return out;forward=normalize(forward);const float minimumCosine=std::cos(halfAngleRadians);for(const auto& candidate:candidates){const Float3 delta=sub(candidate.position,origin);const float d=length(delta);if(d>radius||d<kEpsilon||dot(normalize(delta),forward)<minimumCosine)continue;if(visible&&!visible(origin,candidate.position))continue;const float angular=std::clamp(dot(normalize(delta),forward),-1.0F,1.0F);out.push_back({candidate.id,candidate.position,d,(1.0F-d/radius)*0.6F+(angular-minimumCosine)/std::max(1.0F-minimumCosine,kEpsilon)*0.4F});}std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.score!=b.score?a.score>b.score:a.id<b.id;});return out;}

bool NetworkTransformInterpolator::push(NetworkTransformSample sample)noexcept{if(!finite(sample.position)||!finite(sample.velocity)||capacity_==0U)return false;auto it=std::lower_bound(samples_.begin(),samples_.end(),sample.tick,[](const auto& value,std::uint64_t tick){return value.tick<tick;});if(it!=samples_.end()&&it->tick==sample.tick)*it=sample;else samples_.insert(it,sample);if(samples_.size()>capacity_)samples_.erase(samples_.begin(),samples_.begin()+static_cast<std::ptrdiff_t>(samples_.size()-capacity_));return true;}
std::optional<NetworkTransformSample> NetworkTransformInterpolator::sample(double tick)const noexcept{if(samples_.empty()||!std::isfinite(tick))return std::nullopt;if(tick<=samples_.front().tick)return samples_.front();if(tick>=samples_.back().tick){auto result=samples_.back();const float dt=static_cast<float>(tick-result.tick);result.position=add(result.position,mul(result.velocity,dt));return result;}const auto upper=std::upper_bound(samples_.begin(),samples_.end(),tick,[](double value,const auto& sample){return value<sample.tick;});const auto& b=*upper;const auto& a=*(upper-1);const float alpha=static_cast<float>((tick-a.tick)/static_cast<double>(b.tick-a.tick));return NetworkTransformSample{static_cast<std::uint64_t>(tick),add(a.position,mul(sub(b.position,a.position),alpha)),add(a.velocity,mul(sub(b.velocity,a.velocity),alpha))};}
void ReplicationRelevancyGrid::upsert(NetworkObjectId id,Float3 position){if(id!=kInvalidNetworkObjectId&&finite(position))objects_[id]=position;}void ReplicationRelevancyGrid::erase(NetworkObjectId id){objects_.erase(id);}std::vector<NetworkObjectId> ReplicationRelevancyGrid::query(Float3 center,float radius)const{std::vector<NetworkObjectId> out;if(!finite(center)||!std::isfinite(radius)||radius<0.0F)return out;const float squared=radius*radius;for(const auto& [id,position]:objects_)if(length_squared(sub(position,center))<=squared)out.push_back(id);return out;}

bool EditorOperationHistory::execute(EditorOperation operation,std::string* error){if(operation.label.empty()||!operation.apply||!operation.revert)return fail(error,"editor operation is incomplete");if(!operation.apply(error))return false;undo_.push_back(std::move(operation));redo_.clear();return true;}
bool EditorOperationHistory::undo(std::string* error){if(undo_.empty())return fail(error,"nothing to undo");auto operation=std::move(undo_.back());undo_.pop_back();if(!operation.revert(error)){undo_.push_back(std::move(operation));return false;}redo_.push_back(std::move(operation));return true;}
bool EditorOperationHistory::redo(std::string* error){if(redo_.empty())return fail(error,"nothing to redo");auto operation=std::move(redo_.back());redo_.pop_back();if(!operation.apply(error)){redo_.push_back(std::move(operation));return false;}undo_.push_back(std::move(operation));return true;}
bool PluginRegistry::register_extension(PluginDescriptor descriptor,std::string* error){if(descriptor.owner.empty()||descriptor.id.empty()||descriptor.displayName.empty()||descriptor.kind.empty())return fail(error,"plugin extension descriptor is incomplete");const auto key=std::pair{descriptor.kind,descriptor.id};if(extensions_.contains(key))return fail(error,"plugin extension id already exists for this kind");extensions_.emplace(key,std::move(descriptor));return true;}
std::size_t PluginRegistry::unregister_owner(std::string_view owner){std::size_t removed{};for(auto it=extensions_.begin();it!=extensions_.end();)if(it->second.owner==owner){it=extensions_.erase(it);++removed;}else++it;return removed;}
const PluginDescriptor* PluginRegistry::find(std::string_view kind,std::string_view id)const noexcept{const auto it=extensions_.find({std::string(kind),std::string(id)});return it==extensions_.end()?nullptr:&it->second;}
std::vector<PluginDescriptor> PluginRegistry::list(std::string_view kind)const{std::vector<PluginDescriptor> out;for(const auto& [key,value]:extensions_)if(kind.empty()||key.first==kind)out.push_back(value);return out;}

} // namespace dve
