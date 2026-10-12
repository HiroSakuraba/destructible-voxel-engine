#include "dve/editor_mesh_authoring.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>
namespace dve::editor {
namespace {
Float2 uv(const PolygonVertex& v,std::uint32_t channel){return channel?v.texcoord1:v.texcoord;}
void set_uv(PolygonVertex& v,std::uint32_t channel,Float2 p){if(channel)v.texcoord1=p;else v.texcoord=p;}
Float3 cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
using Point=std::tuple<float,float,float,float,float>;
Point identity(const PolygonVertex& v,std::uint32_t channel){auto p=uv(v,channel);return {v.position.x,v.position.y,v.position.z,p.x,p.y};}
// LSCM: weighted Cauchy-Riemann residuals in each triangle, with two fixed chart
// vertices. Matrix-free preconditioned CG solves A^T A; every solve is checked.
// Reference: Levy et al., SIGGRAPH 2002, https://brunolevy.github.io/papers/LSCM_SIGGRAPH_2002.pdf
bool unwrap_chart(CookedPolygonAsset& mesh,const std::set<std::uint32_t>& faces,std::uint32_t channel,std::string& error,const std::map<std::uint32_t,Float2>& pins){
    std::map<Point,std::size_t> vertices;std::vector<std::uint32_t> representatives;std::map<std::uint32_t,std::size_t> corner;
    std::map<std::pair<std::size_t,std::size_t>,unsigned> edges;
    for(auto f:faces)for(unsigned j=0;j<3;++j){auto c=f*3+j,v=mesh.indices[c];auto [it,added]=vertices.emplace(identity(mesh.vertices[v],channel),vertices.size());if(added)representatives.push_back(v);corner[c]=it->second;}
    const auto n=vertices.size();if(n>20000){error="UV chart exceeds the 20,000-vertex solve limit";return false;}
    for(auto f:faces)for(unsigned j=0;j<3;++j){auto a=corner[f*3+j],b=corner[f*3+(j+1)%3];++edges[std::minmax(a,b)];}
    bool boundary=false;for(auto [e,count]:edges){(void)e;if(count==1)boundary=true;else if(count!=2){error="UV chart is nonmanifold";return false;}}
    if(!boundary||std::int64_t(n)-std::int64_t(edges.size())+std::int64_t(faces.size())!=1){error="UV chart needs a disk boundary; split it into islands first";return false;}
    auto position=[&](auto i){return mesh.vertices[representatives[i]].position;};
    auto farthest=[&](auto start){std::size_t result=0;float distance=-1;for(std::size_t i=0;i<n;++i){auto d=length_squared(subtract(position(i),position(start)));if(d>distance){distance=d;result=i;}}return result;};
    auto anchorA=farthest(std::size_t(0)),anchorB=farthest(anchorA);double distance=length(subtract(position(anchorA),position(anchorB)));
    if(!(distance>1e-10)){error="UV chart has coincident anchors";return false;}
    std::map<std::size_t,Float2> pinned;
    for(auto [c,node]:corner){auto it=pins.find(c);if(it==pins.end())continue;auto [previous,added]=pinned.emplace(node,it->second);
        if(!added&&(std::abs(previous->second.x-it->second.x)>1e-6F||std::abs(previous->second.y-it->second.y)>1e-6F)){error="Shared UV point has conflicting pins";return false;}}
    if(pinned.empty()){pinned[anchorA]={0,0};pinned[anchorB]={float(distance),0};}
    else if(pinned.size()==1){auto [a,p]=*pinned.begin();auto b=farthest(a);pinned[b]={p.x+float(length(subtract(position(a),position(b)))),p.y};}
    const auto size=n*2;std::vector<int> free(size,-1);std::vector<double> fixed(size,0);std::vector<bool> locked(size);std::size_t variables=0;
    for(auto [node,p]:pinned){locked[node]=locked[node+n]=true;fixed[node]=p.x;fixed[node+n]=p.y;}
    for(std::size_t i=0;i<size;++i)if(!locked[i])free[i]=int(variables++);
    struct Row{std::vector<std::pair<std::size_t,double>> coefficients;double rhs{};};std::vector<Row> rows;
    for(auto f:faces){std::array<std::size_t,3> ids{corner[f*3],corner[f*3+1],corner[f*3+2]};
        auto p0=position(ids[0]),e1=subtract(position(ids[1]),p0),e2=subtract(position(ids[2]),p0);double L=length(e1);auto normal=cross(e1,e2);double area2=length(normal);
        if(!(L>1e-10&&area2>1e-14)){error="Cannot unwrap a degenerate triangle";return false;}
        double x=dot(e2,multiply(e1,float(1/L))),y=area2/L,weight=std::sqrt(area2*.5);
        std::array<double,3> gx{-y/(L*y),y/(L*y),0},gy{(x-L)/(L*y),-x/(L*y),1/y};
        for(unsigned equation=0;equation<2;++equation){Row row;for(unsigned j=0;j<3;++j)for(unsigned coord=0;coord<2;++coord){auto index=ids[j]+coord*n;double value=weight*(equation==0?(coord==0?gx[j]:-gy[j]):(coord==0?gy[j]:gx[j]));
                    if(free[index]>=0)row.coefficients.emplace_back(std::size_t(free[index]),value);else row.rhs-=value*fixed[index];}rows.push_back(std::move(row));}}
    std::vector<double> b(variables),diagonal(variables);for(const auto& row:rows)for(auto [i,a]:row.coefficients){b[i]+=a*row.rhs;diagonal[i]+=a*a;}
    auto apply=[&](const std::vector<double>& x){std::vector<double> out(variables);for(const auto& row:rows){double v=0;for(auto [i,a]:row.coefficients)v+=a*x[i];for(auto [i,a]:row.coefficients)out[i]+=a*v;}return out;};
    auto dot=[](const auto& a,const auto& b){return std::inner_product(a.begin(),a.end(),b.begin(),0.0);};
    std::vector<double> answer(variables),residual=b,z(variables),direction;for(std::size_t i=0;i<variables;++i){if(!(diagonal[i]>0)){error="UV chart is singular";return false;}z[i]=residual[i]/diagonal[i];}direction=z;double rz=dot(residual,z),initial=dot(b,b);bool converged=initial<1e-24;
    for(std::size_t iteration=0;!converged&&iteration<std::min<std::size_t>(variables*4+32,4000);++iteration){auto product=apply(direction);double denominator=dot(direction,product);if(!(denominator>0))break;double alpha=rz/denominator;
        for(std::size_t i=0;i<variables;++i){answer[i]+=alpha*direction[i];residual[i]-=alpha*product[i];}if(dot(residual,residual)<=std::max(1e-24,initial*1e-16)){converged=true;break;}
        for(std::size_t i=0;i<variables;++i)z[i]=residual[i]/diagonal[i];double next=dot(residual,z),beta=next/rz;for(std::size_t i=0;i<variables;++i)direction[i]=z[i]+beta*direction[i];rz=next;}
    if(!converged){error="UV solver did not converge; split or simplify the chart";return false;}
    std::vector<Float2> coordinates(n);for(std::size_t i=0;i<n;++i){double u=free[i]<0?fixed[i]:answer[std::size_t(free[i])],v=free[i+n]<0?fixed[i+n]:answer[std::size_t(free[i+n])];if(!std::isfinite(u)||!std::isfinite(v)){error="UV solve produced nonfinite coordinates";return false;}coordinates[i]={float(u),float(v)};}
    for(auto f:faces){auto a=coordinates[corner[f*3]],b=coordinates[corner[f*3+1]],c=coordinates[corner[f*3+2]];if((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)<=1e-12F){error="UV chart folds or collapses; add seams before unwrapping";return false;}}
    for(auto [c,i]:corner)set_uv(mesh.vertices[mesh.indices[c]],channel,coordinates[i]);return true;
}
}
std::vector<std::set<std::uint32_t>> polygon_uv_islands(const CookedPolygonAsset& mesh,std::uint32_t channel){
    if(channel>1||!validate_polygon_asset(mesh))return {};
    std::map<Point,std::uint32_t> points;std::vector<std::uint32_t> identityIds;for(const auto& v:mesh.vertices){auto [it,added]=points.emplace(identity(v,channel),std::uint32_t(points.size()));(void)added;identityIds.push_back(it->second);}
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<std::uint32_t>> edges;
    for(std::uint32_t f=0;f<mesh.indices.size()/3;++f)for(unsigned j=0;j<3;++j){auto a=identityIds[mesh.indices[f*3+j]],b=identityIds[mesh.indices[f*3+(j+1)%3]];edges[std::minmax(a,b)].push_back(f);}
    std::vector<std::vector<std::uint32_t>> neighbors(mesh.indices.size()/3);for(const auto& [e,fs]:edges){(void)e;if(fs.size()==2){neighbors[fs[0]].push_back(fs[1]);neighbors[fs[1]].push_back(fs[0]);}}
    std::set<std::uint32_t> remaining;for(std::uint32_t f=0;f<neighbors.size();++f)remaining.insert(f);std::vector<std::set<std::uint32_t>> islands;
    while(!remaining.empty()){std::queue<std::uint32_t> q;std::set<std::uint32_t> faces;q.push(*remaining.begin());remaining.erase(remaining.begin());while(!q.empty()){auto f=q.front();q.pop();faces.insert(f);for(auto next:neighbors[f])if(remaining.erase(next))q.push(next);}islands.push_back(std::move(faces));}return islands;
}
PolygonEditResult edit_polygon_uv(const CookedPolygonAsset& source,const std::set<std::uint32_t>& corners,UvEdit operation,const UvEditSettings& settings){
    PolygonEditResult r;r.mesh=source;r.selection={source.objectId,PolygonSelectionMode::Face,{}};
    auto fail=[&](std::string e){r.mesh=source;r.error=std::move(e);return r;};
    if(!validate_polygon_asset(source)||corners.empty()||settings.channel>1)return fail("Select UV corners in channel 0 or 1");
    for(auto c:corners)if(c>=source.indices.size())return fail("UV selection is stale");
    if(!std::isfinite(settings.vector.x)||!std::isfinite(settings.vector.y)||!std::isfinite(settings.pivot.x)||!std::isfinite(settings.pivot.y)||!std::isfinite(settings.angle)||!std::isfinite(settings.padding))return fail("UV tool values must be finite");
    for(auto [c,p]:settings.pins)if(c>=source.indices.size()||!std::isfinite(p.x)||!std::isfinite(p.y))return fail("Invalid UV pin");
    if((operation==UvEdit::Unwrap||operation==UvEdit::Pack)&&corners.size()>60000)return fail("Select at most 20,000 UV faces per unwrap/pack operation");
    if(operation==UvEdit::Scale&&(!(settings.vector.x>0)||!(settings.vector.y>0)))return fail("UV scale factors must be positive");
    auto& m=r.mesh;std::set<std::uint32_t> outside;for(std::uint32_t c=0;c<source.indices.size();++c)if(!corners.contains(c))outside.insert(source.indices[c]);std::map<std::uint32_t,std::uint32_t> remap;
    for(auto c:corners){auto old=source.indices[c];auto [it,added]=remap.emplace(old,old);if(added&&outside.contains(old)){it->second=std::uint32_t(m.vertices.size());m.vertices.push_back(source.vertices[old]);}m.indices[c]=it->second;r.selection.components.insert(c/3);}
    std::set<std::uint32_t> selected;for(auto c:corners)selected.insert(m.indices[c]);
    if(operation==UvEdit::Unwrap||operation==UvEdit::Pack){
        std::set<std::uint32_t> faces;for(auto c:corners)faces.insert(c/3);for(auto f:faces)for(unsigned j=0;j<3;++j)if(!corners.contains(f*3+j))return fail("Select complete faces/islands for Unwrap or Pack");
        std::vector<std::set<std::uint32_t>> charts;for(auto island:polygon_uv_islands(m,settings.channel)){std::set<std::uint32_t> chart;std::set_intersection(island.begin(),island.end(),faces.begin(),faces.end(),std::inserter(chart,chart.end()));if(!chart.empty())charts.push_back(std::move(chart));}
        std::map<std::uint32_t,std::size_t> owner;
        for(std::size_t chartIndex=0;chartIndex<charts.size();++chartIndex){std::map<std::uint32_t,std::uint32_t> copies;
            for(auto f:charts[chartIndex])for(unsigned j=0;j<3;++j){auto c=f*3+j,index=m.indices[c];auto [it,added]=owner.emplace(index,chartIndex);
                if(!added&&it->second!=chartIndex){auto [copy,newCopy]=copies.emplace(index,std::uint32_t(m.vertices.size()));if(newCopy)m.vertices.push_back(m.vertices[index]);m.indices[c]=copy->second;}}}
        if(operation==UvEdit::Unwrap){for(auto& chart:charts){std::string error;auto temporary=m;auto pinsCopy=settings.pins;if(!unwrap_chart(temporary,chart,settings.channel,error,pinsCopy)){
                if(error.find("disk boundary")==std::string::npos)return fail(error);
                for(auto face:chart){for(unsigned j=0;j<3;++j){auto c=face*3+j;auto v=m.vertices[m.indices[c]];m.indices[c]=std::uint32_t(m.vertices.size());m.vertices.push_back(v);}std::string triangleError;
                    if(!unwrap_chart(m,{face},settings.channel,triangleError,pinsCopy))return fail(triangleError);}
            }else m=std::move(temporary);}}
        else {
            if(!(settings.padding>=0&&settings.padding<.25F))return fail("UV padding must be between 0 and 0.25");
            auto grid=std::size_t(std::ceil(std::sqrt(double(charts.size()))));double cell=1.0/double(grid),margin=settings.padding;
            if(margin*2>=cell)return fail("Padding leaves no room for the selected island count");
            for(std::size_t i=0;i<charts.size();++i){std::set<std::uint32_t> vertices;Float2 minimum{INFINITY,INFINITY},maximum{-INFINITY,-INFINITY};
                for(auto f:charts[i])for(unsigned j=0;j<3;++j)vertices.insert(m.indices[f*3+j]);for(auto v:vertices){auto p=uv(m.vertices[v],settings.channel);minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y)};maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y)};}
                double extent=std::max(double(maximum.x-minimum.x),double(maximum.y-minimum.y));if(!(extent>1e-12))return fail("Cannot pack a collapsed UV island; unwrap it first");double scale=(cell-2*margin)/extent;
                for(auto v:vertices){auto p=uv(m.vertices[v],settings.channel);set_uv(m.vertices[v],settings.channel,{float(double(i%grid)*cell+margin+(p.x-minimum.x)*scale),float(double(i/grid)*cell+margin+(p.y-minimum.y)*scale)});}}
        }
    }else if(operation==UvEdit::Stitch){
        std::map<std::tuple<float,float,float>,std::vector<std::uint32_t>> groups;for(auto v:selected){auto p=m.vertices[v].position;groups[{p.x,p.y,p.z}].push_back(v);}for(const auto& [p,vs]:groups){(void)p;Float2 average{};for(auto v:vs){auto x=uv(m.vertices[v],settings.channel);average.x+=x.x;average.y+=x.y;}average.x/=float(vs.size());average.y/=float(vs.size());for(auto v:vs)set_uv(m.vertices[v],settings.channel,average);}
    }else for(auto v:selected){auto p=uv(m.vertices[v],settings.channel);float x=p.x-settings.pivot.x,y=p.y-settings.pivot.y;
        if(operation==UvEdit::Translate||operation==UvEdit::Split){p.x+=settings.vector.x;p.y+=settings.vector.y;}
        else if(operation==UvEdit::Rotate){p={settings.pivot.x+x*std::cos(settings.angle)-y*std::sin(settings.angle),settings.pivot.y+x*std::sin(settings.angle)+y*std::cos(settings.angle)};}
        else if(operation==UvEdit::Scale){p={settings.pivot.x+x*settings.vector.x,settings.pivot.y+y*settings.vector.y};}
        else if(operation==UvEdit::FlipU)p.x=settings.pivot.x-x;else if(operation==UvEdit::FlipV)p.y=settings.pivot.y-y;
        set_uv(m.vertices[v],settings.channel,p);
    }
    if(m.vertices.size()>1000000)return fail("UV edit exceeds the geometry budget");refresh_editor_polygon(m);auto valid=validate_polygon_asset(m);if(!valid)return fail(valid.message);return r;
}
} // namespace dve::editor
