#include "dve/navigation_mesh.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>

namespace dve {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kEpsilon = 1.0e-6F;

[[nodiscard]] Float3 add(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] Float3 sub(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] Float3 mul(Float3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] float dot(Float3 a, Float3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }
[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
[[nodiscard]] float length_squared(Float3 v) noexcept { return dot(v, v); }
[[nodiscard]] float length(Float3 v) noexcept { return std::sqrt(length_squared(v)); }
[[nodiscard]] float distance_squared(Float3 a, Float3 b) noexcept { return length_squared(sub(a,b)); }
[[nodiscard]] bool finite(Float3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
[[nodiscard]] NavigationBounds triangle_bounds(const std::array<Float3,3>& v) noexcept {
    NavigationBounds b{v[0],v[0]};
    for(std::size_t i=1;i<3;++i){
        b.minimum={std::min(b.minimum.x,v[i].x),std::min(b.minimum.y,v[i].y),std::min(b.minimum.z,v[i].z)};
        b.maximum={std::max(b.maximum.x,v[i].x),std::max(b.maximum.y,v[i].y),std::max(b.maximum.z,v[i].z)};
    }
    return b;
}
[[nodiscard]] bool overlaps(const NavigationBounds& a,const NavigationBounds& b) noexcept {
    return a.minimum.x<=b.maximum.x&&a.maximum.x>=b.minimum.x&&
           a.minimum.y<=b.maximum.y&&a.maximum.y>=b.minimum.y&&
           a.minimum.z<=b.maximum.z&&a.maximum.z>=b.minimum.z;
}
[[nodiscard]] bool polygon_allowed(const NavigationPolygon& p,const NavigationQueryFilter& f,
                                   std::span<const NavigationObstacle> obstacles) noexcept {
    if((p.flags&f.includeFlags)==0U||(p.flags&f.excludeFlags)!=0U)return false;
    for(const auto& o:obstacles)if((p.flags&o.blockedFlags)!=0U&&overlaps(p.bounds,o.bounds))return false;
    return true;
}
[[nodiscard]] float cross2(Float3 a,Float3 b,Float3 c) noexcept {
    const float abx=b.x-a.x,abz=b.z-a.z,acx=c.x-a.x,acz=c.z-a.z;
    return abx*acz-abz*acx;
}
[[nodiscard]] bool same_xz(Float3 a,Float3 b,float tolerance=1.0e-5F) noexcept {
    return std::abs(a.x-b.x)<=tolerance&&std::abs(a.z-b.z)<=tolerance&&std::abs(a.y-b.y)<=tolerance;
}

struct QuantizedPoint { std::int64_t x{},z{}; auto operator<=>(const QuantizedPoint&)const=default; };
struct EdgeKey { QuantizedPoint a{},b{}; bool operator==(const EdgeKey&)const=default; };
struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k)const noexcept{
        std::size_t h=1469598103934665603ULL;
        auto mix=[&](std::int64_t v){h^=std::hash<std::int64_t>{}(v);h*=1099511628211ULL;};
        mix(k.a.x);mix(k.a.z);mix(k.b.x);mix(k.b.z);return h;
    }
};
struct EdgeRef { NavigationPolygonId polygon{}; Float3 a{},b{}; };
struct TileKey2 { std::int32_t x{},z{}; bool operator==(const TileKey2&)const=default; };
struct TileKey2Hash { std::size_t operator()(const TileKey2& k)const noexcept{
    const std::uint64_t packed=(static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x))<<32U)|static_cast<std::uint32_t>(k.z);
    return std::hash<std::uint64_t>{}(packed);
} };
struct ClearanceTriangle { std::array<Float3,3> vertices{}; NavigationBounds bounds{}; };

[[nodiscard]] std::optional<float> triangle_height_at_xz(const ClearanceTriangle& triangle,float x,float z,float tolerance) noexcept {
    const auto& a=triangle.vertices[0];const auto& b=triangle.vertices[1];const auto& c=triangle.vertices[2];
    const float denominator=(b.z-c.z)*(a.x-c.x)+(c.x-b.x)*(a.z-c.z);
    if(std::abs(denominator)<=tolerance*tolerance)return std::nullopt;
    const float u=((b.z-c.z)*(x-c.x)+(c.x-b.x)*(z-c.z))/denominator;
    const float v=((c.z-a.z)*(x-c.x)+(a.x-c.x)*(z-c.z))/denominator;
    const float w=1.0F-u-v;
    if(u<-tolerance||v<-tolerance||w<-tolerance)return std::nullopt;
    return u*a.y+v*b.y+w*c.y;
}
[[nodiscard]] QuantizedPoint quantize(Float3 p,float tolerance) noexcept {
    const double inv=1.0/static_cast<double>(tolerance);
    return {static_cast<std::int64_t>(std::llround(static_cast<double>(p.x)*inv)),
            static_cast<std::int64_t>(std::llround(static_cast<double>(p.z)*inv))};
}
[[nodiscard]] EdgeKey edge_key(Float3 a,Float3 b,float tolerance) noexcept {
    auto qa=quantize(a,tolerance),qb=quantize(b,tolerance);
    if(qb<qa)std::swap(qa,qb);
    return {qa,qb};
}

[[nodiscard]] Float3 closest_point_triangle(Float3 p,const std::array<Float3,3>& tri) noexcept {
    const Float3 a=tri[0],b=tri[1],c=tri[2];
    const Float3 ab=sub(b,a),ac=sub(c,a),ap=sub(p,a);
    const float d1=dot(ab,ap),d2=dot(ac,ap);
    if(d1<=0.0F&&d2<=0.0F)return a;
    const Float3 bp=sub(p,b); const float d3=dot(ab,bp),d4=dot(ac,bp);
    if(d3>=0.0F&&d4<=d3)return b;
    const float vc=d1*d4-d3*d2;
    if(vc<=0.0F&&d1>=0.0F&&d3<=0.0F){const float v=d1/(d1-d3);return add(a,mul(ab,v));}
    const Float3 cp=sub(p,c); const float d5=dot(ab,cp),d6=dot(ac,cp);
    if(d6>=0.0F&&d5<=d6)return c;
    const float vb=d5*d2-d1*d6;
    if(vb<=0.0F&&d2>=0.0F&&d6<=0.0F){const float w=d2/(d2-d6);return add(a,mul(ac,w));}
    const float va=d3*d6-d5*d4;
    if(va<=0.0F&&(d4-d3)>=0.0F&&(d5-d6)>=0.0F){
        const Float3 bc=sub(c,b);const float w=(d4-d3)/((d4-d3)+(d5-d6));return add(b,mul(bc,w));
    }
    const float denom=1.0F/(va+vb+vc);const float v=vb*denom,w=vc*denom;
    return add(a,add(mul(ab,v),mul(ac,w)));
}

[[nodiscard]] const NavigationPortal* portal_to(const NavigationPolygon& p,NavigationPolygonId neighbour) noexcept {
    for(const auto& portal:p.portals)if(portal.neighbour==neighbour)return &portal;
    return nullptr;
}

void append_unique(std::vector<NavigationPathPoint>& out,Float3 p,NavigationPathPointKind kind=NavigationPathPointKind::Surface,
                   std::uint64_t userId=0U){
    if(!out.empty()&&distance_squared(out.back().position,p)<1.0e-10F&&out.back().kind==kind)return;
    out.push_back({p,kind,userId});
}

[[nodiscard]] std::vector<Float3> funnel_path(const NavigationMesh& mesh,
                                               std::span<const NavigationPolygonId> corridor,
                                               Float3 start,Float3 end){
    std::vector<std::pair<Float3,Float3>> portals;
    portals.reserve(corridor.size()+1U);
    portals.emplace_back(start,start);
    for(std::size_t i=0;i+1U<corridor.size();++i){
        const auto* p=portal_to(mesh.polygons[corridor[i]],corridor[i+1U]);
        if(!p){portals.emplace_back(mesh.polygons[corridor[i]].center,mesh.polygons[corridor[i]].center);continue;}
        portals.emplace_back(p->left,p->right);
    }
    portals.emplace_back(end,end);
    std::vector<Float3> result;result.reserve(portals.size());result.push_back(start);
    Float3 apex=start,left=start,right=start;
    std::size_t apexIndex=0,leftIndex=0,rightIndex=0;
    for(std::size_t i=1;i<portals.size();++i){
        const Float3 newLeft=portals[i].first,newRight=portals[i].second;
        if(cross2(apex,right,newRight)<=0.0F){
            if(same_xz(apex,right)||cross2(apex,left,newRight)>0.0F){right=newRight;rightIndex=i;}
            else{result.push_back(left);apex=left;apexIndex=leftIndex;left=apex;right=apex;leftIndex=apexIndex;rightIndex=apexIndex;i=apexIndex;continue;}
        }
        if(cross2(apex,left,newLeft)>=0.0F){
            if(same_xz(apex,left)||cross2(apex,right,newLeft)<0.0F){left=newLeft;leftIndex=i;}
            else{result.push_back(right);apex=right;apexIndex=rightIndex;left=apex;right=apex;leftIndex=apexIndex;rightIndex=apexIndex;i=apexIndex;continue;}
        }
    }
    if(result.empty()||distance_squared(result.back(),end)>1.0e-10F)result.push_back(end);
    return result;
}

[[nodiscard]] float area_cost(const NavigationQueryFilter& f,std::uint16_t area) noexcept {
    const std::size_t i=std::min<std::size_t>(area,f.areaCosts.size()-1U);
    return std::max(0.001F,f.areaCosts[i]);
}

struct LinkConnection {NavigationPolygonId from{},to{};std::size_t link{};bool reverse{};float cost{};};
struct ParentInfo {NavigationPolygonId parent{kInvalidNavigationPolygon};std::int32_t link{-1};bool reverse{};};

void hash_bytes(std::uint64_t& h,const void* data,std::size_t size) noexcept {
    const auto* p=static_cast<const std::uint8_t*>(data);
    for(std::size_t i=0;i<size;++i){h^=p[i];h*=1099511628211ULL;}
}
template<class T>void hash_value(std::uint64_t& h,const T& v)noexcept{hash_bytes(h,&v,sizeof(v));}

bool fail(std::string* error,std::string message){if(error)*error=std::move(message);return false;}

template<class T>bool write_value(std::ofstream& out,const T& v){out.write(reinterpret_cast<const char*>(&v),sizeof(v));return static_cast<bool>(out);}
template<class T>bool read_value(std::ifstream& in,T& v){in.read(reinterpret_cast<char*>(&v),sizeof(v));return static_cast<bool>(in);}

} // namespace

bool NavigationBuildSettings::validate(std::string* error) const noexcept {
    auto bad=[&](const char* m){if(error)*error=m;return false;};
    if(!std::isfinite(maximumSlopeDegrees)||maximumSlopeDegrees<0.0F||maximumSlopeDegrees>=89.9F)return bad("maximumSlopeDegrees must be in [0, 89.9)");
    if(!std::isfinite(agentRadiusMeters)||agentRadiusMeters<0.0F)return bad("agentRadiusMeters must be finite and non-negative");
    if(!std::isfinite(agentHeightMeters)||agentHeightMeters<=0.0F)return bad("agentHeightMeters must be finite and positive");
    if(!std::isfinite(maximumStepHeightMeters)||maximumStepHeightMeters<0.0F)return bad("maximumStepHeightMeters must be finite and non-negative");
    if(!std::isfinite(edgeMatchToleranceMeters)||edgeMatchToleranceMeters<=0.0F)return bad("edgeMatchToleranceMeters must be finite and positive");
    if(!std::isfinite(tileSizeMeters)||tileSizeMeters<=0.0F)return bad("tileSizeMeters must be finite and positive");
    if(maximumPolygons==0U)return bad("maximumPolygons must be positive");
    return true;
}

bool NavigationMesh::validate(std::string* error) const noexcept {
    if(!settings.validate(error))return false;
    if(polygons.size()>settings.maximumPolygons)return fail(error,"navigation mesh exceeds maximumPolygons");
    for(std::size_t i=0;i<polygons.size();++i){
        const auto& p=polygons[i];
        for(auto v:p.vertices)if(!finite(v))return fail(error,"navigation polygon contains a non-finite vertex");
        if(!finite(p.center)||!finite(p.normal)||p.normal.y<=0.0F)return fail(error,"navigation polygon has an invalid center or normal");
        for(const auto& portal:p.portals){
            if(portal.neighbour>=polygons.size()||portal.neighbour==i)return fail(error,"navigation portal has an invalid neighbour");
            if(!finite(portal.left)||!finite(portal.right))return fail(error,"navigation portal is non-finite");
        }
    }
    for(const auto& l:offMeshLinks){
        if(!finite(l.start)||!finite(l.end)||!std::isfinite(l.radiusMeters)||l.radiusMeters<=0.0F||
           !std::isfinite(l.traversalCost)||l.traversalCost<=0.0F)return fail(error,"off-mesh link is invalid");
    }
    return true;
}

bool NavigationBuildResult::success() const noexcept {
    return std::none_of(diagnostics.begin(),diagnostics.end(),[](const auto& d){return d.severity==NavigationBuildDiagnostic::Severity::Error;});
}

NavigationBuildResult build_navigation_mesh(std::span<const NavigationTriangle> triangles,
                                             const NavigationBuildSettings& settings,
                                             std::span<const NavigationOffMeshLink> offMeshLinks){
    NavigationBuildResult out;out.mesh.settings=settings;
    std::string settingsError;
    if(!settings.validate(&settingsError)){out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Error,"SETTINGS",settingsError});return out;}
    const float minUp=std::cos(settings.maximumSlopeDegrees*kPi/180.0F);
    std::uint64_t degenerate=0U,steep=0U,invalid=0U,narrow=0U,nonManifold=0U,lowClearance=0U;
    std::vector<ClearanceTriangle> clearanceTriangles;clearanceTriangles.reserve(triangles.size());
    out.mesh.polygons.reserve(std::min<std::size_t>(triangles.size(),settings.maximumPolygons));
    for(const auto& input:triangles){
        if(out.mesh.polygons.size()>=settings.maximumPolygons){out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Error,"POLYGON_LIMIT","walkable triangle count exceeds maximumPolygons"});break;}
        auto v=input.vertices;
        if(!finite(v[0])||!finite(v[1])||!finite(v[2])){++invalid;continue;}
        Float3 n=cross(sub(v[1],v[0]),sub(v[2],v[0]));const float twiceArea=length(n);
        if(twiceArea<=settings.edgeMatchToleranceMeters*settings.edgeMatchToleranceMeters){++degenerate;continue;}
        n=mul(n,1.0F/twiceArea);
        clearanceTriangles.push_back({v,triangle_bounds(v)});
        if(n.y<minUp){++steep;continue;}
        NavigationPolygon p;p.vertices=v;p.normal=n;p.area=input.area;p.flags=input.flags;p.sourceId=input.sourceId;
        p.center=mul(add(add(v[0],v[1]),v[2]),1.0F/3.0F);p.bounds=triangle_bounds(v);
        p.tileX=static_cast<std::int32_t>(std::floor(p.center.x/settings.tileSizeMeters));
        p.tileZ=static_cast<std::int32_t>(std::floor(p.center.z/settings.tileSizeMeters));
        out.mesh.polygons.push_back(std::move(p));
    }
    std::unordered_map<TileKey2,std::vector<std::size_t>,TileKey2Hash> clearanceByTile;
    std::vector<std::size_t> globalClearance;
    for(std::size_t i=0;i<clearanceTriangles.size();++i){
        const auto& bounds=clearanceTriangles[i].bounds;
        const auto minX=static_cast<std::int32_t>(std::floor(bounds.minimum.x/settings.tileSizeMeters));
        const auto maxX=static_cast<std::int32_t>(std::floor(bounds.maximum.x/settings.tileSizeMeters));
        const auto minZ=static_cast<std::int32_t>(std::floor(bounds.minimum.z/settings.tileSizeMeters));
        const auto maxZ=static_cast<std::int32_t>(std::floor(bounds.maximum.z/settings.tileSizeMeters));
        const std::int64_t tileCount=static_cast<std::int64_t>(maxX-minX+1)*static_cast<std::int64_t>(maxZ-minZ+1);
        if(tileCount>4096){globalClearance.push_back(i);continue;}
        for(std::int32_t z=minZ;z<=maxZ;++z)for(std::int32_t x=minX;x<=maxX;++x)clearanceByTile[{x,z}].push_back(i);
    }
    std::vector<NavigationPolygon> clearanceFiltered;clearanceFiltered.reserve(out.mesh.polygons.size());
    for(auto& polygon:out.mesh.polygons){
        bool blocked=false;
        auto check=[&](std::size_t index){
            if(blocked)return;
            const auto height=triangle_height_at_xz(clearanceTriangles[index],polygon.center.x,polygon.center.z,settings.edgeMatchToleranceMeters);
            if(!height)return;
            const float gap=*height-polygon.center.y;
            if(gap>settings.edgeMatchToleranceMeters&&gap<settings.agentHeightMeters-settings.edgeMatchToleranceMeters)blocked=true;
        };
        if(const auto it=clearanceByTile.find({polygon.tileX,polygon.tileZ});it!=clearanceByTile.end())for(std::size_t index:it->second)check(index);
        for(std::size_t index:globalClearance)check(index);
        if(blocked)++lowClearance;else clearanceFiltered.push_back(std::move(polygon));
    }
    out.mesh.polygons=std::move(clearanceFiltered);

    std::unordered_map<EdgeKey,std::vector<EdgeRef>,EdgeKeyHash> edges;edges.reserve(out.mesh.polygons.size()*3U);
    for(NavigationPolygonId i=0;i<out.mesh.polygons.size();++i){
        const auto& v=out.mesh.polygons[i].vertices;
        for(std::size_t e=0;e<3;++e){const Float3 a=v[e],b=v[(e+1U)%3U];edges[edge_key(a,b,settings.edgeMatchToleranceMeters)].push_back({i,a,b});}
    }
    for(auto& [key,refs]:edges){
        (void)key;
        if(refs.size()<2U)continue;
        if(refs.size()>2U){++nonManifold;}
        for(std::size_t ai=0;ai<refs.size();++ai)for(std::size_t bi=ai+1U;bi<refs.size();++bi){
            const auto& a=refs[ai];const auto& b=refs[bi];if(a.polygon==b.polygon)continue;
            const float samePair=std::abs(a.a.x-b.a.x)+std::abs(a.a.z-b.a.z)+std::abs(a.b.x-b.b.x)+std::abs(a.b.z-b.b.z);
            const float reversePair=std::abs(a.a.x-b.b.x)+std::abs(a.a.z-b.b.z)+std::abs(a.b.x-b.a.x)+std::abs(a.b.z-b.a.z);
            const Float3 b0=samePair<=reversePair?b.a:b.b;
            const Float3 b1=samePair<=reversePair?b.b:b.a;
            if(std::abs(a.a.y-b0.y)>settings.maximumStepHeightMeters+settings.edgeMatchToleranceMeters||
               std::abs(a.b.y-b1.y)>settings.maximumStepHeightMeters+settings.edgeMatchToleranceMeters)continue;
            Float3 p0{0.5F*(a.a.x+b0.x),std::max(a.a.y,b0.y),0.5F*(a.a.z+b0.z)};
            Float3 p1{0.5F*(a.b.x+b1.x),std::max(a.b.y,b1.y),0.5F*(a.b.z+b1.z)};
            const float width=length(sub(p1,p0));
            if(width<=2.0F*settings.agentRadiusMeters+settings.edgeMatchToleranceMeters){++narrow;continue;}
            const Float3 edgeDir=mul(sub(p1,p0),1.0F/width);
            p0=add(p0,mul(edgeDir,settings.agentRadiusMeters));p1=sub(p1,mul(edgeDir,settings.agentRadiusMeters));
            auto make=[&](NavigationPolygonId from,NavigationPolygonId to){
                const Float3 direction=sub(out.mesh.polygons[to].center,out.mesh.polygons[from].center);
                const float c0=direction.x*(p0.z-out.mesh.polygons[from].center.z)-direction.z*(p0.x-out.mesh.polygons[from].center.x);
                const float c1=direction.x*(p1.z-out.mesh.polygons[from].center.z)-direction.z*(p1.x-out.mesh.polygons[from].center.x);
                NavigationPortal portal;portal.neighbour=to;
                if(c0>=c1){portal.left=p0;portal.right=p1;}else{portal.left=p1;portal.right=p0;}
                auto& portals=out.mesh.polygons[from].portals;
                if(std::none_of(portals.begin(),portals.end(),[&](const auto& p){return p.neighbour==to;}))portals.push_back(portal);
            };
            make(a.polygon,b.polygon);make(b.polygon,a.polygon);
        }
    }
    out.mesh.offMeshLinks.assign(offMeshLinks.begin(),offMeshLinks.end());
    if(!out.mesh.polygons.empty()){
        out.mesh.bounds=out.mesh.polygons.front().bounds;
        for(const auto& p:out.mesh.polygons){
            out.mesh.bounds.minimum={std::min(out.mesh.bounds.minimum.x,p.bounds.minimum.x),std::min(out.mesh.bounds.minimum.y,p.bounds.minimum.y),std::min(out.mesh.bounds.minimum.z,p.bounds.minimum.z)};
            out.mesh.bounds.maximum={std::max(out.mesh.bounds.maximum.x,p.bounds.maximum.x),std::max(out.mesh.bounds.maximum.y,p.bounds.maximum.y),std::max(out.mesh.bounds.maximum.z,p.bounds.maximum.z)};
        }
    }
    out.mesh.contentHash=navigation_mesh_content_hash(out.mesh);
    if(invalid)out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Warning,"INVALID_TRIANGLES",std::to_string(invalid)+" non-finite triangles were skipped"});
    if(degenerate)out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Information,"DEGENERATE_TRIANGLES",std::to_string(degenerate)+" degenerate triangles were skipped"});
    if(steep)out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Information,"STEEP_TRIANGLES",std::to_string(steep)+" triangles exceeded the maximum walkable slope"});
    if(narrow)out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Information,"NARROW_PORTALS",std::to_string(narrow)+" shared edges were too narrow for the configured agent radius"});
    if(lowClearance)out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Information,"LOW_CLEARANCE",std::to_string(lowClearance)+" walkable triangles were removed by the configured agent height"});
    if(nonManifold)out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Warning,"NON_MANIFOLD",std::to_string(nonManifold)+" non-manifold navigation edges were connected pairwise"});
    std::string validation;
    if(!out.mesh.validate(&validation))out.diagnostics.push_back({NavigationBuildDiagnostic::Severity::Error,"VALIDATION",validation});
    return out;
}

NavigationNearestPoint nearest_navigation_point(const NavigationMesh& mesh,Float3 point,
                                                 const NavigationQueryFilter& filter,
                                                 std::span<const NavigationObstacle> obstacles){
    NavigationNearestPoint best;const float maximum=filter.nearestSearchRadiusMeters>0.0F?
        filter.nearestSearchRadiusMeters*filter.nearestSearchRadiusMeters:std::numeric_limits<float>::infinity();
    for(NavigationPolygonId i=0;i<mesh.polygons.size();++i){const auto& p=mesh.polygons[i];if(!polygon_allowed(p,filter,obstacles))continue;
        const Float3 q=closest_point_triangle(point,p.vertices);const float d=distance_squared(point,q);
        if(d<best.distanceSquared&&d<=maximum){best={q,i,d};}}
    return best;
}

NavigationPath find_navigation_path(const NavigationMesh& mesh,Float3 start,Float3 end,
                                    const NavigationQueryFilter& filter,
                                    std::span<const NavigationObstacle> obstacles){
    NavigationPath out;
    const auto startHit=nearest_navigation_point(mesh,start,filter,obstacles),endHit=nearest_navigation_point(mesh,end,filter,obstacles);
    if(!startHit){out.error="no walkable navigation polygon near the start point";return out;}
    if(!endHit){out.error="no walkable navigation polygon near the end point";return out;}
    if(startHit.polygon==endHit.polygon){out.points={{startHit.position,NavigationPathPointKind::Surface,0U},{endHit.position,NavigationPathPointKind::Surface,0U}};out.corridor={startHit.polygon};out.totalCost=length(sub(endHit.position,startHit.position));return out;}

    std::vector<LinkConnection> links;
    for(std::size_t i=0;i<mesh.offMeshLinks.size();++i){const auto& l=mesh.offMeshLinks[i];
        NavigationQueryFilter lf=filter;lf.nearestSearchRadiusMeters=l.radiusMeters;
        const auto a=nearest_navigation_point(mesh,l.start,lf,obstacles),b=nearest_navigation_point(mesh,l.end,lf,obstacles);
        if(!a||!b||a.polygon==b.polygon)continue;
        if((l.flags&filter.includeFlags)==0U||(l.flags&filter.excludeFlags)!=0U)continue;
        links.push_back({a.polygon,b.polygon,i,false,l.traversalCost*length(sub(l.end,l.start))});
        if(l.bidirectional)links.push_back({b.polygon,a.polygon,i,true,l.traversalCost*length(sub(l.end,l.start))});
    }
    std::vector<std::vector<std::size_t>> linksFrom(mesh.polygons.size());for(std::size_t i=0;i<links.size();++i)linksFrom[links[i].from].push_back(i);

    struct QueueNode{float score{};NavigationPolygonId id{};bool operator>(const QueueNode& o)const noexcept{return score>o.score;}};
    std::priority_queue<QueueNode,std::vector<QueueNode>,std::greater<QueueNode>> open;
    const std::size_t n=mesh.polygons.size();
    std::vector<float> g(n,std::numeric_limits<float>::infinity());std::vector<ParentInfo> parent(n);std::vector<bool> closed(n,false);
    auto heuristic=[&](NavigationPolygonId id){return length(sub(mesh.polygons[id].center,endHit.position));};
    g[startHit.polygon]=0.0F;open.push({heuristic(startHit.polygon),startHit.polygon});
    NavigationPolygonId closest=startHit.polygon;float closestH=heuristic(closest);std::uint32_t visited=0U;
    auto relax=[&](NavigationPolygonId from,NavigationPolygonId to,float cost,std::int32_t link,bool reverse){
        if(to>=n||closed[to]||!polygon_allowed(mesh.polygons[to],filter,obstacles))return;
        const float candidate=g[from]+cost*area_cost(filter,mesh.polygons[to].area);
        if(candidate<g[to]){g[to]=candidate;parent[to]={from,link,reverse};open.push({candidate+heuristic(to),to});}
    };
    while(!open.empty()&&visited<filter.maximumVisitedPolygons){const auto current=open.top();open.pop();if(closed[current.id])continue;closed[current.id]=true;++visited;
        const float h=heuristic(current.id);if(h<closestH){closestH=h;closest=current.id;}if(current.id==endHit.polygon){closest=current.id;break;}
        for(const auto& portal:mesh.polygons[current.id].portals)relax(current.id,portal.neighbour,length(sub(mesh.polygons[portal.neighbour].center,mesh.polygons[current.id].center)),-1,false);
        for(std::size_t edge:linksFrom[current.id]){const auto& l=links[edge];relax(current.id,l.to,l.cost,static_cast<std::int32_t>(l.link),l.reverse);}
    }
    const NavigationPolygonId goal=closed[endHit.polygon]?endHit.polygon:closest;out.partial=goal!=endHit.polygon;
    if(goal==startHit.polygon&&goal!=endHit.polygon){out.error="navigation destination is unreachable";return out;}
    std::vector<NavigationPolygonId> reversed;std::vector<ParentInfo> transitions;
    for(NavigationPolygonId p=goal;;p=parent[p].parent){reversed.push_back(p);if(p==startHit.polygon)break;if(parent[p].parent==kInvalidNavigationPolygon){out.error="navigation corridor reconstruction failed";return out;}transitions.push_back(parent[p]);}
    std::reverse(reversed.begin(),reversed.end());std::reverse(transitions.begin(),transitions.end());out.corridor=reversed;out.totalCost=g[goal];
    Float3 currentStart=startHit.position;std::size_t segmentStart=0U;
    for(std::size_t i=1;i<reversed.size();++i){const auto& step=transitions[i-1U];if(step.link<0)continue;
        const auto& link=mesh.offMeshLinks[static_cast<std::size_t>(step.link)];const Float3 linkStart=step.reverse?link.end:link.start;const Float3 linkEnd=step.reverse?link.start:link.end;
        const auto segment=funnel_path(mesh,std::span<const NavigationPolygonId>(reversed.data()+segmentStart,i-segmentStart),currentStart,linkStart);
        for(Float3 p:segment)append_unique(out.points,p);
        append_unique(out.points,linkStart,NavigationPathPointKind::OffMeshStart,link.userId);
        append_unique(out.points,linkEnd,NavigationPathPointKind::OffMeshEnd,link.userId);
        currentStart=linkEnd;segmentStart=i;
    }
    const Float3 finalPoint=out.partial?mesh.polygons[goal].center:endHit.position;
    const auto segment=funnel_path(mesh,std::span<const NavigationPolygonId>(reversed.data()+segmentStart,reversed.size()-segmentStart),currentStart,finalPoint);
    for(Float3 p:segment)append_unique(out.points,p);
    return out;
}

std::uint64_t navigation_mesh_content_hash(const NavigationMesh& mesh) noexcept {
    std::uint64_t h=1469598103934665603ULL;
    hash_value(h,mesh.settings.maximumSlopeDegrees);hash_value(h,mesh.settings.agentRadiusMeters);hash_value(h,mesh.settings.agentHeightMeters);
    hash_value(h,mesh.settings.maximumStepHeightMeters);hash_value(h,mesh.settings.edgeMatchToleranceMeters);hash_value(h,mesh.settings.tileSizeMeters);
    const std::uint64_t count=mesh.polygons.size();hash_value(h,count);
    for(const auto& p:mesh.polygons){for(auto v:p.vertices){hash_value(h,v.x);hash_value(h,v.y);hash_value(h,v.z);}hash_value(h,p.area);hash_value(h,p.flags);hash_value(h,p.sourceId);}
    const std::uint64_t links=mesh.offMeshLinks.size();hash_value(h,links);
    for(const auto& l:mesh.offMeshLinks){hash_value(h,l.start.x);hash_value(h,l.start.y);hash_value(h,l.start.z);hash_value(h,l.end.x);hash_value(h,l.end.y);hash_value(h,l.end.z);hash_value(h,l.radiusMeters);hash_value(h,l.traversalCost);hash_value(h,l.bidirectional);hash_value(h,l.area);hash_value(h,l.flags);hash_value(h,l.userId);}
    return h;
}

bool write_dnav(const std::filesystem::path& path,const NavigationMesh& mesh,std::string* error){
    std::string validation;if(!mesh.validate(&validation))return fail(error,"invalid navigation mesh: "+validation);
    std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)return fail(error,"could not open navigation mesh for writing");
    const std::array<char,4> magic{'D','N','A','V'};out.write(magic.data(),magic.size());const std::uint32_t version=1U;
    if(!write_value(out,version)||!write_value(out,mesh.settings.maximumSlopeDegrees)||!write_value(out,mesh.settings.agentRadiusMeters)||
       !write_value(out,mesh.settings.agentHeightMeters)||!write_value(out,mesh.settings.maximumStepHeightMeters)||
       !write_value(out,mesh.settings.edgeMatchToleranceMeters)||!write_value(out,mesh.settings.tileSizeMeters)||!write_value(out,mesh.settings.maximumPolygons))return fail(error,"failed writing navigation settings");
    const std::uint64_t count=mesh.polygons.size(),links=mesh.offMeshLinks.size(),hash=navigation_mesh_content_hash(mesh);
    if(!write_value(out,count)||!write_value(out,links)||!write_value(out,hash))return fail(error,"failed writing navigation header");
    for(const auto& p:mesh.polygons){for(auto v:p.vertices)if(!write_value(out,v))return fail(error,"failed writing navigation vertices");if(!write_value(out,p.area)||!write_value(out,p.flags)||!write_value(out,p.sourceId))return fail(error,"failed writing navigation polygon");}
    for(const auto& l:mesh.offMeshLinks)if(!write_value(out,l.start)||!write_value(out,l.end)||!write_value(out,l.radiusMeters)||!write_value(out,l.traversalCost)||!write_value(out,l.bidirectional)||!write_value(out,l.area)||!write_value(out,l.flags)||!write_value(out,l.userId))return fail(error,"failed writing off-mesh link");
    return static_cast<bool>(out)||fail(error,"failed finalizing navigation mesh");
}

std::optional<NavigationMesh> read_dnav(const std::filesystem::path& path,std::string* error,std::uint64_t maximumBytes){
    std::error_code ec;const auto size=std::filesystem::file_size(path,ec);if(ec||size>maximumBytes){fail(error,ec?"could not stat navigation mesh":"navigation mesh exceeds maximumBytes");return std::nullopt;}
    std::ifstream in(path,std::ios::binary);if(!in){fail(error,"could not open navigation mesh");return std::nullopt;}
    std::array<char,4> magic{};in.read(magic.data(),magic.size());std::uint32_t version{};if(!in||magic!=std::array<char,4>{'D','N','A','V'}||!read_value(in,version)||version!=1U){fail(error,"unsupported or corrupt navigation mesh header");return std::nullopt;}
    NavigationBuildSettings s;if(!read_value(in,s.maximumSlopeDegrees)||!read_value(in,s.agentRadiusMeters)||!read_value(in,s.agentHeightMeters)||!read_value(in,s.maximumStepHeightMeters)||!read_value(in,s.edgeMatchToleranceMeters)||!read_value(in,s.tileSizeMeters)||!read_value(in,s.maximumPolygons)){fail(error,"truncated navigation settings");return std::nullopt;}
    std::uint64_t count{},linkCount{},storedHash{};if(!read_value(in,count)||!read_value(in,linkCount)||!read_value(in,storedHash)||count>s.maximumPolygons||linkCount>1'000'000ULL){fail(error,"invalid navigation counts");return std::nullopt;}
    std::vector<NavigationTriangle> triangles(static_cast<std::size_t>(count));for(auto& t:triangles){for(auto& v:t.vertices)if(!read_value(in,v)){fail(error,"truncated navigation vertices");return std::nullopt;}if(!read_value(in,t.area)||!read_value(in,t.flags)||!read_value(in,t.sourceId)){fail(error,"truncated navigation polygon");return std::nullopt;}}
    std::vector<NavigationOffMeshLink> links(static_cast<std::size_t>(linkCount));for(auto& l:links)if(!read_value(in,l.start)||!read_value(in,l.end)||!read_value(in,l.radiusMeters)||!read_value(in,l.traversalCost)||!read_value(in,l.bidirectional)||!read_value(in,l.area)||!read_value(in,l.flags)||!read_value(in,l.userId)){fail(error,"truncated off-mesh link");return std::nullopt;}
    auto built=build_navigation_mesh(triangles,s,links);if(!built.success()){fail(error,"navigation mesh rebuild failed");return std::nullopt;}if(navigation_mesh_content_hash(built.mesh)!=storedHash){fail(error,"navigation mesh hash mismatch");return std::nullopt;}return built.mesh;
}

} // namespace dve
