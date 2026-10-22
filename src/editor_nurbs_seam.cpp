#include "dve/editor_nurbs_seam.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
#include <tuple>
namespace dve::editor {
namespace {
bool fail(std::string* error,const std::string& text){if(error)*error=text;return false;}
bool v_edge(NurbsBoundary edge){return edge==NurbsBoundary::VMin||edge==NurbsBoundary::VMax;}
bool min_edge(NurbsBoundary edge){return edge==NurbsBoundary::UMin||edge==NurbsBoundary::VMin;}
const std::string* encoded(const EditorObject& object){auto* component=find_component_by_type(object.components,kNurbsComponent);if(!component)return nullptr;auto it=component->properties.find(kNurbsSeamProperty);return it==component->properties.end()?nullptr:std::get_if<std::string>(&it->second);}
std::string encode(const NurbsSeam& seam){
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<"DVE_NURBS_SEAM 1 "<<seam.reference<<' '<<seam.moving<<' '<<unsigned(seam.options.referenceBoundary)<<' '<<unsigned(seam.options.movingBoundary)<<' '<<seam.options.reverseBoundary<<' '<<seam.options.c1<<' '<<seam.referenceStamp<<' '<<seam.movingStamp<<' ';
    for(const auto* axis:{&seam.grid.u,&seam.grid.v,&seam.fractions}){out<<axis->size()<<' ';for(double x:*axis)out<<x<<' ';}
    for(auto p:seam.anchors)out<<p.x<<' '<<p.y<<' '<<p.z<<' ';return out.str();
}
bool same_frame(const EditorObject& a,const EditorObject& b){return nurbs_seam_same_frame(a,b);}
std::optional<NurbsSample> boundary(const NurbsSource& source,NurbsBoundary edge,double parameter){const auto& cross=v_edge(edge)?source.v:source.u;const double fixed=min_edge(edge)?cross.knots.front():cross.knots.back();return evaluate_nurbs(source,v_edge(edge)?parameter:fixed,v_edge(edge)?fixed:parameter);}
bool near_parameter(double a,double b){return std::abs(a-b)<=8*std::numeric_limits<double>::epsilon()*(std::max)({std::abs(a),std::abs(b),1e-300});}
std::vector<double> parameters(const NurbsSource& source,NurbsBoundary edge,const std::vector<double>& fractions,bool reverse){
    const auto& axis=v_edge(edge)?source.u:source.v;std::vector<double> out;
    for(double x:fractions){double value=std::lerp(axis.knots.front(),axis.knots.back(),reverse?1-x:x);std::optional<double> knot;
        for(double candidate:axis.knots)if(near_parameter(candidate,value)){if(knot&&*knot!=candidate)return {};knot=candidate;}
        out.push_back(knot.value_or(value));
    }
    auto sorted=out;std::sort(sorted.begin(),sorted.end());if(std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end())return {};return out;
}
void appearance(CookedPolygonAsset& mesh,const CookedPolygonAsset& old){mesh.materials=old.materials;mesh.materialBindings=old.materialBindings;mesh.images=old.images;mesh.samplers=old.samplers;mesh.textures=old.textures;mesh.contentHash=polygon_asset_content_hash(mesh);}
void set_source(EditorObject& object,const NurbsSource& source){find_component_by_type(std::span<Component>(object.components),kNurbsComponent)->properties["source"]=encode_nurbs(source);}
void set_seam(EditorObject& object,const NurbsSeam& seam){find_component_by_type(std::span<Component>(object.components),kNurbsComponent)->properties[std::string(kNurbsSeamProperty)]=encode(seam);}
void clear_seam(EditorObject& object){find_component_by_type(std::span<Component>(object.components),kNurbsComponent)->properties.erase(std::string(kNurbsSeamProperty));}
bool continuity(const NurbsSource& a,const NurbsSource& b,const NurbsSeam& seam,std::string* error,std::stop_token stop={}){
    std::vector<double> fractions=seam.fractions;
    for(std::size_t i=1;i<seam.fractions.size();++i)fractions.push_back(std::midpoint(seam.fractions[i-1],seam.fractions[i]));
    auto pa=parameters(a,seam.options.referenceBoundary,fractions,false),pb=parameters(b,seam.options.movingBoundary,fractions,seam.options.reverseBoundary);
    if(pa.size()!=fractions.size()||pb.size()!=fractions.size())return fail(error,"Seam continuity sample parameters lost precision");
    const auto& ca=v_edge(seam.options.referenceBoundary)?a.v:a.u;const auto& cb=v_edge(seam.options.movingBoundary)?b.v:b.u;
    const double scaleA=(min_edge(seam.options.referenceBoundary)?-1.:1.)*(ca.knots.back()-ca.knots.front()),scaleB=(min_edge(seam.options.movingBoundary)?1.:-1.)*(cb.knots.back()-cb.knots.front());
    for(std::size_t i=0;i<pa.size();++i){if(stop.stop_requested())return fail(error,"NURBS seam cancelled");auto x=boundary(a,seam.options.referenceBoundary,pa[i]),y=boundary(b,seam.options.movingBoundary,pb[i]);if(!x||!y)return fail(error,"Linked seam boundary evaluation failed");
        const auto da=v_edge(seam.options.referenceBoundary)?x->dv:x->du,db=v_edge(seam.options.movingBoundary)?y->dv:y->du;
        for(unsigned k=0;k<3;++k){if(std::abs(x->position[k]-y->position[k])>1e-8*(1+std::abs(x->position[k])))return fail(error,"Linked seam C0 continuity is stale");
            if(seam.options.c1&&std::abs(da[k]*scaleA-db[k]*scaleB)>1e-7*(1+std::abs(da[k]*scaleA)))return fail(error,"Linked seam C1 continuity is stale at fraction "+std::to_string(fractions[i])+", axis "+std::to_string(k)+": "+std::to_string(da[k]*scaleA)+" vs "+std::to_string(db[k]*scaleB));}
    }
    return true;
}
bool prepare_pair(EditorObject& a,EditorObject& b,NurbsAttachmentOptions options,std::string* error,std::stop_token stop={}){
    if(stop.stop_requested())return fail(error,"NURBS seam cancelled");
    if(!same_frame(a,b))return fail(error,"Linked patches require the same parent, pose and axis scale, without socket attachment; match boundaries first or unlink before independent transforms");
    auto sa=editor_nurbs_source(a,error),sb=editor_nurbs_source(b,error);if(!sa||!sb)return false;
    options.preserveWeightGauge=true;auto matched=attach_nurbs_boundaries(*sa,*sb,options,error,stop);if(!matched)return false;
    // Publish the prepared reference too: both sources then share the exact compatible basis.
    sa=std::move(matched->reference);sb=std::move(matched->moving);
    auto ga=nurbs_display_grid(*sa,nullptr,error,stop),gb=nurbs_display_grid(*sb,nullptr,error,stop);if(!ga||!gb)return false;
    std::vector<double> fractions;
    auto collect=[&](const NurbsSource& source,const NurbsDisplayGrid& grid,NurbsBoundary edge,bool reverse,const std::vector<double>& previous){
        const auto& axis=v_edge(edge)?source.u:source.v;const auto& values=v_edge(edge)?grid.u:grid.v;auto old=parameters(source,edge,previous,reverse);
        for(double t:values){auto it=std::find(old.begin(),old.end(),t);double x=it!=old.end()?previous[std::size_t(it-old.begin())]:(t-axis.knots.front())/(axis.knots.back()-axis.knots.front());if(it==old.end()&&reverse)x=1-x;fractions.push_back(x);}
    };
    auto canonicalize=[&]()->bool{
        std::sort(fractions.begin(),fractions.end());fractions.erase(std::unique(fractions.begin(),fractions.end()),fractions.end());
        std::vector<double> canonical;
        for(double x:fractions){bool duplicate=false;
            if(!canonical.empty()&&near_parameter(x,canonical.back())){
                auto ap=parameters(*sa,options.referenceBoundary,{canonical.back(),x},false),bp=parameters(*sb,options.movingBoundary,{canonical.back(),x},options.reverseBoundary);
                // One-ULP affine round trips may map to the same knot. Never erase
                // geometrically distinct samples merely because parameters are close.
                auto identical=[&](const NurbsSource& source,NurbsBoundary edge,const std::vector<double>& params){
                    if(params.empty())return true;auto a=boundary(source,edge,params[0]),b=boundary(source,edge,params[1]);if(!a||!b)return false;
                    for(unsigned k=0;k<3;++k)if(float(a->position[k])!=float(b->position[k]))return false;return true;
                };
                duplicate=identical(*sa,options.referenceBoundary,ap)&&identical(*sb,options.movingBoundary,bp);
            }
            if(!duplicate)canonical.push_back(x);
        }
        fractions=std::move(canonical);return fractions.size()>=2&&fractions.size()<=4096;
    };
    bool settled=false;
    for(unsigned round=0;round<12;++round){
        if(stop.stop_requested())return fail(error,"NURBS seam cancelled");
        auto previous=fractions;collect(*sa,*ga,options.referenceBoundary,false,previous);collect(*sb,*gb,options.movingBoundary,options.reverseBoundary,previous);
        if(!canonicalize())return fail(error,"Shared seam exceeds its sample budget");
        auto as=parameters(*sa,options.referenceBoundary,fractions,false),bs=parameters(*sb,options.movingBoundary,fractions,options.reverseBoundary);
        if(as.size()!=fractions.size()||bs.size()!=fractions.size())return fail(error,"Shared seam parameter mapping lost precision");
        auto seedA=*ga,seedB=*gb;(v_edge(options.referenceBoundary)?seedA.u:seedA.v)=as;auto sortedB=bs;std::sort(sortedB.begin(),sortedB.end());(v_edge(options.movingBoundary)?seedB.u:seedB.v)=std::move(sortedB);
        if(!validate_nurbs_display_grid(*sa,seedA,error)||!validate_nurbs_display_grid(*sb,seedB,error))return false;
        auto nextA=nurbs_display_grid(*sa,&seedA,error,stop),nextB=nurbs_display_grid(*sb,&seedB,error,stop);if(!nextA||!nextB)return false;
        previous=fractions;collect(*sa,*nextA,options.referenceBoundary,false,previous);collect(*sb,*nextB,options.movingBoundary,options.reverseBoundary,previous);
        if(!canonicalize())return fail(error,"Shared seam exceeds its sample budget");
        as=parameters(*sa,options.referenceBoundary,fractions,false);bs=parameters(*sb,options.movingBoundary,fractions,options.reverseBoundary);
        if(as.size()!=fractions.size()||bs.size()!=fractions.size())return fail(error,"Shared seam parameter mapping lost precision");
        (v_edge(options.referenceBoundary)?nextA->u:nextA->v)=std::move(as);std::sort(bs.begin(),bs.end());(v_edge(options.movingBoundary)?nextB->u:nextB->v)=std::move(bs);
        if(!validate_nurbs_display_grid(*sa,*nextA,error)||!validate_nurbs_display_grid(*sb,*nextB,error))return false;
        if(nextA->u==ga->u&&nextA->v==ga->v&&nextB->u==gb->u&&nextB->v==gb->v){ga=std::move(nextA);gb=std::move(nextB);settled=true;break;}
        ga=std::move(nextA);gb=std::move(nextB);
    }
    if(!settled)return fail(error,"Shared adaptive seam did not stabilize within 12 rounds");
    NurbsSeam seam;seam.reference=a.id;seam.moving=b.id;seam.options=options;seam.referenceStamp=nurbs_source_stamp(*sa);seam.movingStamp=nurbs_source_stamp(*sb);seam.fractions=std::move(fractions);
    if(!continuity(*sa,*sb,seam,error,stop))return false;
    auto positions=parameters(*sa,options.referenceBoundary,seam.fractions,false);for(double t:positions){auto p=boundary(*sa,options.referenceBoundary,t);if(!p)return fail(error,"Shared seam evaluation failed");seam.anchors.push_back({float(p->position[0]),float(p->position[1]),float(p->position[2])});}
    set_source(a,*sa);set_source(b,*sb);seam.grid=*ga;set_seam(a,seam);seam.grid=*gb;set_seam(b,seam);
    auto ma=tessellate_editor_nurbs_display(a,*sa,error,stop),mb=tessellate_editor_nurbs_display(b,*sb,error,stop);if(!ma||!mb)return false;
    appearance(*ma,*a.polygon);appearance(*mb,*b.polygon);if(!validate_polygon_asset(*ma)||!validate_polygon_asset(*mb))return fail(error,"Shared NURBS material/cache validation failed");a.polygon=std::move(ma);b.polygon=std::move(mb);return true;
}
using SeamKey=std::tuple<EditorObjectId,EditorObjectId,unsigned,unsigned>;
SeamKey seam_key(const NurbsSeam& s){return {s.reference,s.moving,unsigned(s.options.referenceBoundary),unsigned(s.options.movingBoundary)};}
std::optional<std::vector<NurbsSeam>> network_edges(const std::vector<const EditorObject*>& objects,std::string* error){
    std::vector<NurbsSeam> edges;
    for(auto* object:objects)if(has_nurbs_seam(*object)){
        auto seams=editor_nurbs_seams(*object,error);if(!seams)return {};
        for(auto& seam:*seams)if(seam.reference==object->id)edges.push_back(std::move(seam));
    }
    std::sort(edges.begin(),edges.end(),[](const auto& a,const auto& b){return seam_key(a)<seam_key(b);});return edges;
}
std::optional<std::vector<EditorObjectId>> network_order(const std::vector<const EditorObject*>& objects,const std::vector<NurbsSeam>& edges,std::string* error){
    if(objects.size()>16||edges.size()>24){fail(error,"NURBS network exceeds 16 patches or 24 seams");return {};}
    std::map<EditorObjectId,unsigned> incoming;std::set<std::pair<EditorObjectId,NurbsBoundary>> occupied;
    for(auto* object:objects){incoming[object->id]=0;if(!same_frame(*objects.front(),*object)){fail(error,"Linked network needs the same parent, pose and axis scale without socket attachments");return {};}}
    for(const auto& edge:edges){if(edge.reference==edge.moving||!incoming.contains(edge.reference)||!incoming.contains(edge.moving)||unsigned(edge.options.referenceBoundary)>3||unsigned(edge.options.movingBoundary)>3){fail(error,"NURBS network has invalid/missing endpoints");return {};}
        if(!occupied.emplace(edge.reference,edge.options.referenceBoundary).second||!occupied.emplace(edge.moving,edge.options.movingBoundary).second){fail(error,"A NURBS boundary already has a seam; choose a free boundary or unlink it first");return {};}
        ++incoming[edge.moving];
    }
    std::set<EditorObjectId> ready;for(auto [id,count]:incoming)if(!count)ready.insert(id);std::vector<EditorObjectId> order;
    while(!ready.empty()){auto id=*ready.begin();ready.erase(ready.begin());order.push_back(id);for(const auto& e:edges)if(e.reference==id&&!--incoming[e.moving])ready.insert(e.moving);}
    if(order.size()!=objects.size()){fail(error,"Cyclic NURBS driving constraints are not supported; reverse a link or unlink before closing this dependency cycle");return {};}
    return order;
}
void set_seams(EditorObject& object,const std::vector<NurbsSeam>& seams){
    if(seams.empty()){clear_seam(object);return;}if(seams.size()==1){set_seam(object,seams.front());return;}
    std::ostringstream out;out.imbue(std::locale::classic());out<<"DVE_NURBS_SEAMS 2 "<<seams.size();for(const auto& seam:seams)out<<' '<<std::quoted(encode(seam));
    find_component_by_type(std::span<Component>(object.components),kNurbsComponent)->properties[std::string(kNurbsSeamProperty)]=out.str();
}
bool synchronize_edge(const NurbsSource& a,const NurbsSource& b,NurbsDisplayGrid& ga,NurbsDisplayGrid& gb,NurbsSeam& seam,std::string* error){
    auto fractions=seam.fractions;
    auto collect=[&](const NurbsSource& source,const NurbsDisplayGrid& grid,NurbsBoundary edge,bool reverse){
        const auto& axis=v_edge(edge)?source.u:source.v;const auto& values=v_edge(edge)?grid.u:grid.v;auto old=parameters(source,edge,seam.fractions,reverse);
        for(double t:values){auto it=std::find(old.begin(),old.end(),t);double x=it!=old.end()?seam.fractions[std::size_t(it-old.begin())]:(t-axis.knots.front())/(axis.knots.back()-axis.knots.front());if(it==old.end()&&reverse)x=1-x;fractions.push_back(x);}
    };
    collect(a,ga,seam.options.referenceBoundary,false);collect(b,gb,seam.options.movingBoundary,seam.options.reverseBoundary);
    std::sort(fractions.begin(),fractions.end());fractions.erase(std::unique(fractions.begin(),fractions.end()),fractions.end());std::vector<double> canonical;
    for(double x:fractions){bool duplicate=false;
        if(!canonical.empty()&&near_parameter(x,canonical.back())){
            auto equal=[&](const NurbsSource& source,NurbsBoundary edge,bool reverse){auto params=parameters(source,edge,{canonical.back(),x},reverse);if(params.empty())return true;auto p=boundary(source,edge,params[0]),q=boundary(source,edge,params[1]);if(!p||!q)return false;for(unsigned k=0;k<3;++k)if(float(p->position[k])!=float(q->position[k]))return false;return true;};
            duplicate=equal(a,seam.options.referenceBoundary,false)&&equal(b,seam.options.movingBoundary,seam.options.reverseBoundary);
        }
        if(!duplicate)canonical.push_back(x);
    }
    if(canonical.size()<2||canonical.size()>4096)return fail(error,"Network seam sample budget exceeded");
    auto pa=parameters(a,seam.options.referenceBoundary,canonical,false),pb=parameters(b,seam.options.movingBoundary,canonical,seam.options.reverseBoundary);
    if(pa.size()!=canonical.size()||pb.size()!=canonical.size())return fail(error,"Network seam parameter mapping lost precision");
    (v_edge(seam.options.referenceBoundary)?ga.u:ga.v)=std::move(pa);std::sort(pb.begin(),pb.end());(v_edge(seam.options.movingBoundary)?gb.u:gb.v)=std::move(pb);seam.fractions=std::move(canonical);
    return validate_nurbs_display_grid(a,ga,error)&&validate_nurbs_display_grid(b,gb,error);
}
unsigned corner(NurbsBoundary edge,bool high){return v_edge(edge)?unsigned(high)+2*unsigned(!min_edge(edge)):unsigned(!min_edge(edge))+2*unsigned(high);}
using Corner=std::pair<EditorObjectId,unsigned>;
std::optional<std::map<Corner,Float3>> junction_positions(const std::map<EditorObjectId,NurbsSource>& sources,const std::vector<NurbsSeam>& edges){
    std::map<Corner,Corner> parents;
    auto root=[&](Corner c){while(parents.at(c)!=c)c=parents.at(c);return c;};
    for(const auto& edge:edges)for(bool high:{false,true}){Corner a{edge.reference,corner(edge.options.referenceBoundary,high)},b{edge.moving,corner(edge.options.movingBoundary,high!=edge.options.reverseBoundary)};parents.try_emplace(a,a);parents.try_emplace(b,b);a=root(a);b=root(b);if(a!=b)parents[(std::max)(a,b)]=(std::min)(a,b);}
    std::map<Corner,Float3> junctions;
    for(auto [key,parent]:parents){auto r=root(key);const auto& source=sources.at(r.first);auto p=evaluate_nurbs(source,(r.second&1)?source.u.knots.back():source.u.knots.front(),(r.second&2)?source.v.knots.back():source.v.knots.front());if(!p)return std::nullopt;junctions[key]={float(p->position[0]),float(p->position[1]),float(p->position[2])};}
    return junctions;
}
bool prepare_network(const std::vector<EditorObject*>& objects,std::vector<NurbsSeam> edges,std::string* error,std::stop_token stop={}){
    std::vector<const EditorObject*> immutable(objects.begin(),objects.end());auto order=network_order(immutable,edges,error);if(!order)return false;
    if(objects.size()==2&&edges.size()==1){auto a=std::find_if(objects.begin(),objects.end(),[&](auto* o){return o->id==edges[0].reference;});auto b=std::find_if(objects.begin(),objects.end(),[&](auto* o){return o->id==edges[0].moving;});return prepare_pair(**a,**b,edges[0].options,error,stop);}
    std::map<EditorObjectId,EditorObject*> nodes;std::map<EditorObjectId,NurbsSource> sources;
    for(auto* object:objects){nodes[object->id]=object;auto source=editor_nurbs_source(*object,error);if(!source||source->is_curve())return fail(error,"NURBS networks require untrimmed surface patches");sources[object->id]=std::move(*source);}
    std::map<EditorObjectId,std::size_t> rank;for(std::size_t i=0;i<order->size();++i)rank[(*order)[i]]=i;
    std::sort(edges.begin(),edges.end(),[&](const auto& a,const auto& b){return std::tuple{rank[a.moving],seam_key(a)}<std::tuple{rank[b.moving],seam_key(b)};});
    bool compatible=false;
    for(unsigned round=0;round<17;++round){auto previous=sources;
        for(auto& edge:edges){if(stop.stop_requested())return fail(error,"NURBS network cancelled");edge.options.preserveWeightGauge=true;
            auto matched=attach_nurbs_boundaries(sources.at(edge.reference),sources.at(edge.moving),edge.options,error,stop);if(!matched)return false;
            sources[edge.reference]=std::move(matched->reference);sources[edge.moving]=std::move(matched->moving);
        }
        compatible=true;for(const auto& [id,source]:sources){const auto& old=previous.at(id);if(source.u!=old.u||source.v!=old.v){compatible=false;break;}}
        if(compatible)break;
    }
    if(!compatible)return fail(error,"Network degree/knot compatibility did not stabilize");
    std::map<EditorObjectId,NurbsDisplayGrid> grids;for(auto& [id,source]:sources){auto grid=nurbs_display_grid(source,nullptr,error,stop);if(!grid)return false;grids[id]=std::move(*grid);}
    for(auto& edge:edges)edge.fractions.clear();bool settled=false;
    for(unsigned round=0;round<32;++round){if(stop.stop_requested())return fail(error,"NURBS network cancelled");auto previous=grids;
        for(auto& edge:edges)if(!synchronize_edge(sources.at(edge.reference),sources.at(edge.moving),grids.at(edge.reference),grids.at(edge.moving),edge,error))return false;
        std::size_t corners=0;
        for(auto& [id,grid]:grids){auto refined=nurbs_display_grid(sources.at(id),&grid,error,stop);if(!refined)return false;grid=std::move(*refined);corners+=4*(grid.u.size()-1)*(grid.v.size()-1);}
        if(corners>524288)return fail(error,"NURBS network exceeds 524288 display vertices");
        settled=true;for(const auto& [id,g]:grids)if(g.u!=previous.at(id).u||g.v!=previous.at(id).v){settled=false;break;}if(settled)break;
    }
    if(!settled)return fail(error,"Shared network tessellation did not stabilize within 32 rounds");
    auto sharedCorners=junction_positions(sources,edges);if(!sharedCorners)return fail(error,"Network junction evaluation failed");
    const auto& junctions=*sharedCorners;
    std::map<EditorObjectId,std::vector<NurbsSeam>> records;
    for(auto& edge:edges){const auto& a=sources.at(edge.reference);const auto& b=sources.at(edge.moving);edge.referenceStamp=nurbs_source_stamp(a);edge.movingStamp=nurbs_source_stamp(b);
        if(!continuity(a,b,edge,error,stop))return fail(error,"Incompatible NURBS junction constraints on "+std::to_string(edge.reference)+" -> "+std::to_string(edge.moving)+": "+(error?*error:std::string{}));
        edge.anchors.clear();auto params=parameters(a,edge.options.referenceBoundary,edge.fractions,false);
        for(double t:params){if(stop.stop_requested())return fail(error,"NURBS network cancelled");auto p=boundary(a,edge.options.referenceBoundary,t);if(!p)return false;edge.anchors.push_back({float(p->position[0]),float(p->position[1]),float(p->position[2])});}
        edge.anchors.front()=junctions.at({edge.reference,corner(edge.options.referenceBoundary,false)});edge.anchors.back()=junctions.at({edge.reference,corner(edge.options.referenceBoundary,true)});
        edge.grid=grids.at(edge.reference);records[edge.reference].push_back(edge);edge.grid=grids.at(edge.moving);records[edge.moving].push_back(edge);
    }
    for(auto* object:objects){set_source(*object,sources.at(object->id));auto& seams=records[object->id];std::sort(seams.begin(),seams.end(),[](const auto& a,const auto& b){return seam_key(a)<seam_key(b);});set_seams(*object,seams);}
    for(auto* object:objects){auto mesh=tessellate_editor_nurbs_display(*object,sources.at(object->id),error,stop);if(!mesh)return false;appearance(*mesh,*object->polygon);auto valid=validate_polygon_asset(*mesh);if(!valid)return fail(error,valid.message);object->polygon=std::move(mesh);}
    return true;
}
class SeamCommand final:public IEditorCommand {
public:
    enum class Mode{Link,Refresh,Unlink,Edit};
    SeamCommand(Mode mode,EditorObjectId id,EditorObjectId moving=0,NurbsAttachmentOptions options={},std::optional<NurbsSource> before={},std::optional<NurbsSource> after={}):mode_(mode),id_(id),moving_(moving),options_(options),expected_(std::move(before)),value_(std::move(after)){}
    std::string_view label() const noexcept override{return mode_==Mode::Unlink?"Unlink NURBS seam":"Update linked NURBS seam";}
    CommandResult execute(EditorDocument& document) override{
        if(before_.empty()){
            std::string error;auto* selected=document.find_object(id_);if(!selected)return CommandResult::fail("NURBS seam target is missing");
            auto ids=nurbs_seam_network_ids(document,id_,&error);if(ids.empty())return CommandResult::fail(error);
            if(mode_==Mode::Link){auto other=nurbs_seam_network_ids(document,moving_,&error);if(other.empty())return CommandResult::fail(error);ids.insert(ids.end(),other.begin(),other.end());std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
                if(!can_link_nurbs_seams(document,id_,moving_,options_,&error))return CommandResult::fail(error);
            }else if(!has_nurbs_seam(*selected))return CommandResult::fail("Select a linked NURBS patch");
            if(!validate_editor_nurbs_seams(document,&error))return CommandResult::fail(error);
            std::vector<EditorObject> after;std::vector<const EditorObject*> originals;
            for(auto id:ids){auto* object=document.find_object(id);if(!object||object->flags.locked||!object->flags.visible||object->prefabLink||!validate_editor_nurbs(*object,&error))return CommandResult::fail(error.empty()?"Every linked patch must be visible, unlocked and owned":error);originals.push_back(object);after.push_back(clone_editor_object(*object));}
            auto edges=network_edges(originals,&error);if(!edges)return CommandResult::fail(error);
            if(mode_==Mode::Link){NurbsSeam edge;edge.reference=id_;edge.moving=moving_;edge.options=options_;edges->push_back(edge);}
            if(mode_==Mode::Unlink)std::erase_if(*edges,[&](const auto& edge){return edge.reference==id_||edge.moving==id_;});
            if(mode_==Mode::Edit){auto current=editor_nurbs_source(*selected,&error);if(!current||!expected_||*current!=*expected_||!value_||!validate_nurbs(*value_,&error))return CommandResult::fail("Linked NURBS source changed; reopen the edit");
                if(current->u!=value_->u||current->v!=value_->v||current->columns!=value_->columns||current->rows!=value_->rows)return CommandResult::fail("Unlink the seam before changing degree, knots or parameterization");
                std::set<std::size_t> changed;for(std::size_t i=0;i<current->controls.size();++i)if(current->controls[i]!=value_->controls[i])changed.insert(i);
                if(nurbs_seam_controls_driven(*selected,changed))return CommandResult::fail("This boundary/tangent row is driven by a reference; edit that reference or unlink first");
                for(auto& object:after)if(object.id==id_)set_source(object,*value_);
            }
            std::vector<EditorObject*> outputs;for(auto& object:after)outputs.push_back(&object);
            if(!prepare_network(outputs,std::move(*edges),&error))return CommandResult::fail(error);
            for(const auto* object:originals)before_.push_back(clone_editor_object(*object));after_=std::move(after);
        }
        return apply(document,before_,after_);
    }
    CommandResult undo(EditorDocument& document) override{return apply(document,after_,before_);}
private:
    CommandResult apply(EditorDocument& document,const std::vector<EditorObject>& expected,const std::vector<EditorObject>& values){
        if(expected.empty()||expected.size()!=values.size())return CommandResult::fail("NURBS seam history is incomplete");
        for(const auto& old:expected){const auto* object=document.find_object(old.id);if(!object||!object->polygon||object->flags.locked||!object->flags.visible||object->prefabLink||!same_frame(*object,old)||editor_nurbs_source(*object)!=editor_nurbs_source(old)||has_nurbs_seam(*object)!=has_nurbs_seam(old)||
            (has_nurbs_seam(old)&&(!encoded(*object)||!encoded(old)||*encoded(*object)!=*encoded(old)))||!nurbs_display_geometry_matches(*object->polygon,*old.polygon))return CommandResult::fail("Linked NURBS source, placement or cache changed; history was not applied");}
        // Prepare both resource-preserving replacements before publishing either.
        std::vector<CookedPolygonAsset> meshes;std::vector<std::map<std::string,ComponentValue,std::less<>>> properties;
        for(const auto& value:values){auto* object=document.find_object(value.id);auto mesh=*value.polygon;appearance(mesh,*object->polygon);auto valid=validate_polygon_asset(mesh);if(!valid)return CommandResult::fail(valid.message);meshes.push_back(std::move(mesh));auto props=find_component_by_type(std::span<const Component>(object->components),kNurbsComponent)->properties;const auto& saved=find_component_by_type(value.components,kNurbsComponent)->properties;
            props["source"]=saved.at("source");if(auto it=saved.find(kNurbsSeamProperty);it!=saved.end())props[std::string(kNurbsSeamProperty)]=it->second;else props.erase(std::string(kNurbsSeamProperty));properties.push_back(std::move(props));}
        for(std::size_t i=0;i<values.size();++i){auto* object=document.find_object(values[i].id);object->polygon=std::move(meshes[i]);find_component_by_type(std::span<Component>(object->components),kNurbsComponent)->properties=std::move(properties[i]);}
        document.mark_dirty();return CommandResult::ok();
    }
    Mode mode_;EditorObjectId id_,moving_;NurbsAttachmentOptions options_;std::optional<NurbsSource> expected_,value_;std::vector<EditorObject> before_,after_;
};
}
bool nurbs_seam_same_frame(const EditorObject& a,const EditorObject& b){auto x=a.transform,y=b.transform;return a.parent==b.parent&&!a.attachment&&!b.attachment&&x.position.x==y.position.x&&x.position.y==y.position.y&&x.position.z==y.position.z&&x.rotation.x==y.rotation.x&&x.rotation.y==y.rotation.y&&x.rotation.z==y.rotation.z&&x.rotation.w==y.rotation.w&&a.voxelScale.x==b.voxelScale.x&&a.voxelScale.y==b.voxelScale.y&&a.voxelScale.z==b.voxelScale.z;}
bool has_nurbs_seam(const EditorObject& object) noexcept{const auto* c=find_component_by_type(object.components,kNurbsComponent);return c&&c->properties.contains(kNurbsSeamProperty);}
std::optional<NurbsSeam> decode_seam_record(const EditorObject& object,const std::string& value,std::string* error){
    auto invalid=[&]()->std::optional<NurbsSeam>{fail(error,"Invalid or unsupported NURBS seam metadata");return {};};const auto* text=&value;if(text->size()>1024*1024)return invalid();
    std::istringstream in(*text);in.imbue(std::locale::classic());std::string magic;unsigned version,a,b,reverse,c1;NurbsSeam seam;
    if(!(in>>magic>>version>>seam.reference>>seam.moving>>a>>b>>reverse>>c1>>seam.referenceStamp>>seam.movingStamp)||magic!="DVE_NURBS_SEAM"||version!=1||a>3||b>3||reverse>1||c1>1||!seam.reference||!seam.moving||seam.reference==seam.moving||(object.id!=seam.reference&&object.id!=seam.moving))return invalid();
    seam.options={static_cast<NurbsBoundary>(a),static_cast<NurbsBoundary>(b),reverse!=0,c1!=0,true};
    for(auto* values:{&seam.grid.u,&seam.grid.v,&seam.fractions}){unsigned count;if(!(in>>count)||count<2||count>4096)return invalid();values->resize(count);for(std::size_t i=0;i<count;++i)if(!(in>>(*values)[i])||!std::isfinite((*values)[i])||(i&&!((*values)[i]>(*values)[i-1])))return invalid();}
    if(seam.fractions.front()!=0||seam.fractions.back()!=1)return invalid();seam.anchors.resize(seam.fractions.size());for(auto& p:seam.anchors)if(!(in>>p.x>>p.y>>p.z)||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||std::abs(p.x)>1e8||std::abs(p.y)>1e8||std::abs(p.z)>1e8)return invalid();in>>std::ws;if(!in.eof())return invalid();return seam;
}
std::optional<std::vector<NurbsSeam>> editor_nurbs_seams(const EditorObject& object,std::string* error){
    const auto* text=encoded(object);if(!text||text->size()>1024*1024){fail(error,"Invalid NURBS seam metadata");return {};}
    std::vector<NurbsSeam> result;
    if(text->starts_with("DVE_NURBS_SEAMS ")){
        std::istringstream in(*text);in.imbue(std::locale::classic());std::string magic;unsigned version,count;
        if(!(in>>magic>>version>>count)||version!=2||count<2||count>4){fail(error,"Invalid NURBS incident seam count/version");return {};}
        for(unsigned i=0;i<count;++i){std::string record;if(!(in>>std::quoted(record))){fail(error,"Incomplete NURBS network record");return {};}auto seam=decode_seam_record(object,record,error);if(!seam)return {};result.push_back(std::move(*seam));}
        in>>std::ws;if(!in.eof()){fail(error,"Trailing NURBS network metadata");return {};}
    }else {auto seam=decode_seam_record(object,*text,error);if(!seam)return {};result.push_back(std::move(*seam));}
    std::set<NurbsBoundary> occupied;
    for(const auto& seam:result){const auto edge=object.id==seam.reference?seam.options.referenceBoundary:seam.options.movingBoundary;
        if(!occupied.insert(edge).second||seam.grid.u!=result.front().grid.u||seam.grid.v!=result.front().grid.v){fail(error,"NURBS network repeats a boundary or disagrees on its patch grid");return {};}}
    return result;
}
std::optional<NurbsSeam> editor_nurbs_seam(const EditorObject& object,std::string* error){auto seams=editor_nurbs_seams(object,error);if(!seams)return {};return seams->front();}
std::optional<CookedPolygonAsset> tessellate_editor_nurbs_display(const EditorObject& object,const NurbsSource& source,std::string* error,std::stop_token stop){
    if(!has_nurbs_seam(object))return tessellate_nurbs(source,object.id,error,stop);
    auto seams=editor_nurbs_seams(object,error);if(!seams)return {};
    auto mesh=tessellate_nurbs_grid(source,object.id,seams->front().grid,error,stop);if(!mesh)return {};
    std::map<std::size_t,Float3> assigned;
    for(const auto& record:*seams){const auto* seam=&record;const bool reference=object.id==seam->reference;const auto stamp=reference?seam->referenceStamp:seam->movingStamp;if(stamp!=nurbs_source_stamp(source)){fail(error,"Linked NURBS source is stale; use a coordinated seam edit");return {};}
    const auto edge=reference?seam->options.referenceBoundary:seam->options.movingBoundary;auto params=parameters(source,edge,seam->fractions,!reference&&seam->options.reverseBoundary);const auto& values=v_edge(edge)?seam->grid.u:seam->grid.v;
    if(params.size()!=values.size()){fail(error,"Shared seam schedule is incomplete");return {};}
    for(double p:params)if(std::find(values.begin(),values.end(),p)==values.end()){fail(error,"Shared seam schedule lost a parameter");return {};}
    for(std::size_t i=0;i<params.size();++i){if(stop.stop_requested())return {};auto p=boundary(source,edge,params[i]);if(!p)return {};auto a=seam->anchors[i];if(reference&&i>0&&i+1<params.size()&&(float(p->position[0])!=a.x||float(p->position[1])!=a.y||float(p->position[2])!=a.z)){fail(error,"Reference seam anchors are stale");return {};}for(unsigned k=0;k<3;++k){double x=p->position[k],y=k==0?a.x:k==1?a.y:a.z;if(std::abs(x-y)>8*std::numeric_limits<float>::epsilon()*(1+std::abs(x))){fail(error,"Shared seam anchor diverged from its NURBS boundary");return {};}}}
    const auto columns=seam->grid.u.size()-1;
    for(std::size_t j=0;j+1<seam->grid.v.size();++j)for(std::size_t i=0;i+1<seam->grid.u.size();++i)for(unsigned k=0;k<4;++k){const auto u=i+(k==1||k==2),v=j+(k>=2);const bool on=v_edge(edge)?(min_edge(edge)?v==0:v+1==seam->grid.v.size()):(min_edge(edge)?u==0:u+1==seam->grid.u.size());if(!on)continue;
        const double t=v_edge(edge)?seam->grid.u[u]:seam->grid.v[v];auto it=std::find(params.begin(),params.end(),t);if(it==params.end()){fail(error,"Shared seam vertex is not scheduled");return {};}const auto index=(j*columns+i)*4+k;auto anchor=seam->anchors[std::size_t(it-params.begin())];
        if(auto old=assigned.find(index);old!=assigned.end()&&(old->second.x!=anchor.x||old->second.y!=anchor.y||old->second.z!=anchor.z)){fail(error,"Incident seam anchors disagree at a patch junction");return {};}
        assigned[index]=anchor;mesh->vertices[index].position=anchor;}
    }
    for(std::size_t i=0;i<mesh->indices.size();i+=3){if(stop.stop_requested())return {};auto a=mesh->vertices[mesh->indices[i]].position,b=mesh->vertices[mesh->indices[i+1]].position,c=mesh->vertices[mesh->indices[i+2]].position;const double x=(double(b.y)-a.y)*(double(c.z)-a.z)-(double(b.z)-a.z)*(double(c.y)-a.y),y=(double(b.z)-a.z)*(double(c.x)-a.x)-(double(b.x)-a.x)*(double(c.z)-a.z),z=(double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x);if(!(std::hypot(x,y,z)>0)){fail(error,"Shared seam triangles collapse at float precision");return {};}}
    mesh->bounds.minimum=mesh->bounds.maximum=mesh->vertices.front().position;for(const auto& v:mesh->vertices){auto p=v.position;mesh->bounds.minimum={(std::min)(mesh->bounds.minimum.x,p.x),(std::min)(mesh->bounds.minimum.y,p.y),(std::min)(mesh->bounds.minimum.z,p.z)};mesh->bounds.maximum={(std::max)(mesh->bounds.maximum.x,p.x),(std::max)(mesh->bounds.maximum.y,p.y),(std::max)(mesh->bounds.maximum.z,p.z)};}mesh->contentHash=polygon_asset_content_hash(*mesh);return mesh;
}
std::vector<EditorObjectId> nurbs_seam_network_ids(const EditorDocument& document,EditorObjectId id,std::string* error){
    if(!document.find_object(id)){fail(error,"NURBS network patch is missing");return {};}
    std::set<EditorObjectId> visited;std::vector<EditorObjectId> pending{id};
    while(!pending.empty()){auto next=pending.back();pending.pop_back();if(!visited.insert(next).second)continue;
        if(visited.size()>16){fail(error,"NURBS network exceeds 16 patches");return {};}
        const auto* object=document.find_object(next);if(!object){fail(error,"NURBS network partner is missing");return {};}
        if(has_nurbs_seam(*object)){auto seams=editor_nurbs_seams(*object,error);if(!seams)return {};for(const auto& s:*seams){pending.push_back(s.reference);pending.push_back(s.moving);}}
    }
    return {visited.begin(),visited.end()};
}
bool can_link_nurbs_seams(const EditorDocument& document,EditorObjectId a,EditorObjectId b,NurbsAttachmentOptions options,std::string* error){
    auto ids=nurbs_seam_network_ids(document,a,error),other=nurbs_seam_network_ids(document,b,error);if(ids.empty()||other.empty())return false;ids.insert(ids.end(),other.begin(),other.end());std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
    std::vector<const EditorObject*> nodes;for(auto id:ids)nodes.push_back(document.find_object(id));auto edges=network_edges(nodes,error);if(!edges)return false;NurbsSeam edge;edge.reference=a;edge.moving=b;edge.options=options;edges->push_back(edge);return bool(network_order(nodes,*edges,error));
}
bool validate_editor_nurbs_seams(const EditorDocument& document,std::string* error){
    std::set<EditorObjectId> checked;
    for(const auto& [id,object]:document.objects())if(has_nurbs_seam(object)){
        auto seams=editor_nurbs_seams(object,error);if(!seams)return false;std::map<unsigned,Float3> corners;
        for(const auto& seam:*seams){const auto* a=document.find_object(seam.reference);const auto* b=document.find_object(seam.moving);
            if(!a||!b||!same_frame(*a,*b))return fail(error,"NURBS seam partner/frame is missing or changed; unlink before independent placement or prefab operations");
            if(bool(a->prefabLink)!=bool(b->prefabLink)||(a->prefabLink&&(a->prefabLink->instanceId!=b->prefabLink->instanceId||a->prefabLink->prefabAsset!=b->prefabLink->prefabAsset)))return fail(error,"Linked patches must share the same prefab instance or both be owned");
            auto partners=editor_nurbs_seams(id==a->id?*b:*a,error);if(!partners)return false;
            auto it=std::find_if(partners->begin(),partners->end(),[&](const auto& s){return seam_key(s)==seam_key(seam);});
            if(it==partners->end()||it->options.reverseBoundary!=seam.options.reverseBoundary||it->options.c1!=seam.options.c1||it->referenceStamp!=seam.referenceStamp||it->movingStamp!=seam.movingStamp||it->fractions!=seam.fractions||it->anchors.size()!=seam.anchors.size())return fail(error,"NURBS seam relationship is not reciprocal");
            for(std::size_t i=0;i<seam.anchors.size();++i){auto x=seam.anchors[i],y=it->anchors[i];if(x.x!=y.x||x.y!=y.y||x.z!=y.z)return fail(error,"NURBS seam anchors disagree");}
            auto sa=editor_nurbs_source(*a,error),sb=editor_nurbs_source(*b,error);if(!sa||!sb||nurbs_source_stamp(*sa)!=seam.referenceStamp||nurbs_source_stamp(*sb)!=seam.movingStamp)return fail(error,"Linked NURBS source stamps are stale");
            if(id==seam.reference&&!continuity(*sa,*sb,seam,error))return false;
            for(bool high:{false,true}){const bool reference=id==seam.reference;auto c=corner(reference?seam.options.referenceBoundary:seam.options.movingBoundary,reference?high:high!=seam.options.reverseBoundary);auto value=high?seam.anchors.back():seam.anchors.front();auto old=corners.find(c);
                if(old!=corners.end()&&(old->second.x!=value.x||old->second.y!=value.y||old->second.z!=value.z))return fail(error,"NURBS corner junction anchors disagree");corners[c]=value;}
        }
        if(!checked.contains(id)){auto ids=nurbs_seam_network_ids(document,id,error);if(ids.empty())return false;std::vector<const EditorObject*> nodes;for(auto next:ids){checked.insert(next);nodes.push_back(document.find_object(next));}auto edges=network_edges(nodes,error);if(!edges||!network_order(nodes,*edges,error))return false;
            std::map<EditorObjectId,NurbsSource> sources;for(auto* node:nodes){auto source=editor_nurbs_source(*node,error);if(!source)return false;sources[node->id]=std::move(*source);}
            auto canonical=junction_positions(sources,*edges);if(!canonical)return fail(error,"Network corner evaluation failed");
            for(const auto& edge:*edges)for(bool high:{false,true}){auto expected=canonical->at({edge.reference,corner(edge.options.referenceBoundary,high)});
                const auto actual=high?edge.anchors.back():edge.anchors.front();bool matches=expected.x==actual.x&&expected.y==actual.y&&expected.z==actual.z;
                // Version-one isolated pairs used the directed reference's endpoint.
                if(!matches&&edges->size()==1){auto parameter=parameters(sources.at(edge.reference),edge.options.referenceBoundary,{high?1.:0.},false);auto point=boundary(sources.at(edge.reference),edge.options.referenceBoundary,parameter[0]);if(!point)return false;matches=float(point->position[0])==actual.x&&float(point->position[1])==actual.y&&float(point->position[2])==actual.z;}
                if(!matches)return fail(error,"Canonical NURBS junction anchor is stale");}
        }
    }
    return true;
}
std::unique_ptr<IEditorCommand> link_nurbs_seam_command(EditorObjectId a,EditorObjectId b,NurbsAttachmentOptions options){return std::make_unique<SeamCommand>(SeamCommand::Mode::Link,a,b,options);}
std::unique_ptr<IEditorCommand> unlink_nurbs_seam_command(EditorObjectId id){return std::make_unique<SeamCommand>(SeamCommand::Mode::Unlink,id);}
std::unique_ptr<IEditorCommand> refresh_nurbs_seam_command(EditorObjectId id){return std::make_unique<SeamCommand>(SeamCommand::Mode::Refresh,id);}
std::unique_ptr<IEditorCommand> edit_nurbs_seam_command(EditorObjectId id,const NurbsSource& before,const NurbsSource& after){return std::make_unique<SeamCommand>(SeamCommand::Mode::Edit,id,0,NurbsAttachmentOptions{},before,after);}
namespace {
bool pair_state_matches(const EditorDocument& document,const NurbsSeamDragSnapshot& pair,bool editable,bool allowMissing=false){
    for(const auto* saved:pair.objects()){
        const auto* current=document.find_object(saved->id);
        if(!current&&allowMissing)continue;
        if(!current||!current->polygon||current->subdivision||current->modelingTopology||
           find_component_by_type(current->components,"dve.exact_brep")||!encoded(*current)||!encoded(*saved)||
           *encoded(*current)!=*encoded(*saved)||editor_nurbs_source(*current)!=editor_nurbs_source(*saved)||
           !nurbs_display_geometry_matches(*current->polygon,*saved->polygon))return false;
        if(editable&&(current->flags.locked||!current->flags.visible||current->prefabLink||!same_frame(*current,*saved)))return false;
    }
    return true;
}
bool publish_pair(EditorDocument& document,const NurbsSeamDragSnapshot& pair,std::string* error,bool allowMissing=false){
    // Allocate/validate both replacements before touching either live object.
    std::vector<CookedPolygonAsset> meshes;
    std::vector<std::map<std::string,ComponentValue,std::less<>>> properties;
    std::vector<EditorObjectId> ids;
    for(const auto* saved:pair.objects()){
        auto* current=document.find_object(saved->id);
        if(!current&&allowMissing)continue;
        ids.push_back(saved->id);
        if(!current||!current->polygon||!find_component_by_type(std::span<const Component>(current->components),kNurbsComponent))return fail(error,"Linked patch disappeared");
        auto mesh=*saved->polygon;appearance(mesh,*current->polygon);
        auto valid=validate_polygon_asset(mesh);if(!valid)return fail(error,valid.message);
        auto props=find_component_by_type(std::span<const Component>(current->components),kNurbsComponent)->properties;
        const auto& savedProps=find_component_by_type(saved->components,kNurbsComponent)->properties;
        props["source"]=savedProps.at("source");props[std::string(kNurbsSeamProperty)]=savedProps.at("seam");
        meshes.push_back(std::move(mesh));properties.push_back(std::move(props));
    }
    for(std::size_t i=0;i<ids.size();++i){
        auto* current=document.find_object(ids[i]);current->polygon=std::move(meshes[i]);
        find_component_by_type(std::span<Component>(current->components),kNurbsComponent)->properties=std::move(properties[i]);
    }
    return true;
}
}
namespace {
bool driven_control(const NurbsSource& source,const NurbsSeam& seam,EditorObjectId id,std::size_t index){
    if(index>=source.controls.size())return true;
    if(id==seam.reference)return false;
    const bool v=v_edge(seam.options.movingBoundary),minimum=min_edge(seam.options.movingBoundary);
    const auto cross=v?unsigned(index/source.columns):unsigned(index%source.columns);
    const auto count=v?source.rows:source.columns;
    return (minimum?cross:count-1-cross)<=(seam.options.c1?1U:0U);
}
}
bool nurbs_seam_controls_driven(const EditorObject& object,const std::set<std::size_t>& indices){
    if(!has_nurbs_seam(object))return false;
    auto seams=editor_nurbs_seams(object);auto source=editor_nurbs_source(object);
    if(!seams||!source)return true;
    return std::any_of(indices.begin(),indices.end(),[&](auto i){return std::any_of(seams->begin(),seams->end(),[&](const auto& seam){return driven_control(*source,seam,object.id,i);});});
}
std::vector<const EditorObject*> NurbsSeamDragSnapshot::objects() const {std::vector<const EditorObject*> result{&reference,&moving};for(const auto& object:additional)result.push_back(&object);return result;}
const EditorObject* NurbsSeamDragSnapshot::find(EditorObjectId id) const {for(const auto* object:objects())if(object->id==id)return object;return nullptr;}
bool nurbs_seam_snapshot_matches(const EditorDocument& document,const NurbsSeamDragSnapshot& snapshot,bool editable){return pair_state_matches(document,snapshot,editable);}
std::shared_ptr<const NurbsSeamDragSnapshot> capture_nurbs_seam_drag(const EditorDocument& document,EditorObjectId id,std::string* error){
    const auto* object=document.find_object(id);if(!object)return {};
    auto seam=editor_nurbs_seam(*object,error);if(!seam||!validate_editor_nurbs_seams(document,error))return {};
    auto ids=nurbs_seam_network_ids(document,id,error);if(ids.empty())return {};
    auto pair=std::make_shared<NurbsSeamDragSnapshot>();
    pair->reference=clone_editor_object(*document.find_object(seam->reference));pair->moving=clone_editor_object(*document.find_object(seam->moving));
    for(auto next:ids)if(next!=pair->reference.id&&next!=pair->moving.id)pair->additional.push_back(clone_editor_object(*document.find_object(next)));
    if(!pair_state_matches(document,*pair,true)){fail(error,"Every linked patch must be visible, unlocked, owned and valid");return {};}
    auto strip=[](EditorObject& object){for(auto& image:object.polygon->images){image.width=image.height=1;image.rgba8.assign(4,255);image.rgba8.shrink_to_fit();}};
    strip(pair->reference);strip(pair->moving);for(auto& next:pair->additional)strip(next);
    return pair;
}
std::unique_ptr<PreparedNurbsSeamEdit> PreparedNurbsSeamEdit::prepare(std::shared_ptr<const NurbsSeamDragSnapshot> snapshot,EditorObjectId id,const NurbsSource& source,std::string* error,std::stop_token stop){
    if(stop.stop_requested()||!snapshot)return {};
    if(!snapshot->find(id)){fail(error,"Edited patch is outside the seam snapshot");return {};}
    const auto& selected=*snapshot->find(id);
    auto before=editor_nurbs_source(selected,error);auto edges=network_edges(snapshot->objects(),error);
    if(!before||!edges||!validate_nurbs(source,error))return {};
    for(const auto* object:snapshot->objects())if(!validate_editor_nurbs(*object,error,stop))return {};
    if(before->u!=source.u||before->v!=source.v||before->columns!=source.columns||before->rows!=source.rows){fail(error,"Unlink before changing linked parameterization");return {};}
    std::set<std::size_t> changed;for(std::size_t i=0;i<source.controls.size();++i)if(source.controls[i]!=before->controls[i])changed.insert(i);
    if(nurbs_seam_controls_driven(selected,changed)){fail(error,"This boundary/tangent row is driven by a reference; select free controls or edit the reference");return {};}
    auto edit=std::make_unique<PreparedNurbsSeamEdit>();edit->before_=std::move(snapshot);
    edit->after_.reference=clone_editor_object(edit->before_->reference);edit->after_.moving=clone_editor_object(edit->before_->moving);
    for(const auto& object:edit->before_->additional)edit->after_.additional.push_back(clone_editor_object(object));
    if(source==*before)return edit; // Exact identity: do not renormalize weights or resample.
    std::vector<EditorObject*> nodes{&edit->after_.reference,&edit->after_.moving};for(auto& object:edit->after_.additional)nodes.push_back(&object);
    for(auto* object:nodes)if(object->id==id)set_source(*object,source);
    if(!prepare_network(nodes,std::move(*edges),error,stop)||stop.stop_requested())return {};
    return edit;
}
bool PreparedNurbsSeamEdit::matches(const EditorDocument& document,bool after,bool editable) const{return before_&&pair_state_matches(document,after?after_:*before_,editable);}
bool PreparedNurbsSeamEdit::publish(EditorDocument& document,bool after,std::string* error){return publish_pair(document,after?after_:*before_,error);}
bool PreparedNurbsSeamEdit::publish_preview(EditorDocument& document,const PreparedNurbsSeamEdit* previous,std::string* error){
    if(!(previous?previous->matches(document,true):matches(document,false)))return fail(error,"Linked source, placement or display changed during drag");
    return publish(document,true,error);
}
bool PreparedNurbsSeamEdit::restore_preview(EditorDocument& document,std::string* error){
    // A changed lock/placement cancels the drag but need not prevent restoring its geometry.
    // External source, metadata or cache changes are never overwritten.
    // A partner removed mid-drag must not strand the survivor in its unapplied preview.
    if(!before_||!pair_state_matches(document,after_,false,true))return fail(error,"Linked source or display changed; external edits preserved");
    if(!publish_pair(document,*before_,error,true))return false;
    // A network member that vanished mid-drag leaves survivors with records naming it.
    // Detach the whole surviving network (geometry retained) so the scene stays valid.
    std::vector<EditorObject*> survivors;bool missing=false;
    for(const auto* saved:before_->objects()){auto* current=document.find_object(saved->id);if(current)survivors.push_back(current);else missing=true;}
    if(!missing)return true;
    std::vector<CookedPolygonAsset> meshes;
    for(auto* object:survivors){auto source=editor_nurbs_source(*object,error);if(!source)return false;auto mesh=tessellate_nurbs(*source,object->id,error);if(!mesh)return false;
        appearance(*mesh,*object->polygon);auto valid=validate_polygon_asset(*mesh);if(!valid)return fail(error,valid.message);meshes.push_back(std::move(*mesh));}
    for(std::size_t i=0;i<survivors.size();++i){clear_seam(*survivors[i]);survivors[i]->polygon=std::move(meshes[i]);}
    document.mark_dirty();return true;
}
CommandResult PreparedNurbsSeamEdit::execute(EditorDocument& document){
    std::string error;if(!matches(document,false)||!publish(document,true,&error))return CommandResult::fail(error.empty()?"Linked source, placement or display changed; history was not applied":error);
    document.mark_dirty();return CommandResult::ok();
}
CommandResult PreparedNurbsSeamEdit::undo(EditorDocument& document){
    std::string error;if(!matches(document,true)||!publish(document,false,&error))return CommandResult::fail(error.empty()?"Linked source, placement or display changed; history was not applied":error);
    document.mark_dirty();return CommandResult::ok();
}

bool remap_nurbs_seam_ids(EditorObject& object,EditorObjectId originalId,const std::map<EditorObjectId,EditorObjectId>& ids,std::string* error){
    if(!has_nurbs_seam(object))return true;
    EditorObject old(originalId);old.components.push_back(*find_component_by_type(std::span<const Component>(object.components),kNurbsComponent));auto seams=editor_nurbs_seams(old,error);if(!seams)return false;
    for(auto& seam:*seams){auto a=ids.find(seam.reference),b=ids.find(seam.moving);if(a==ids.end()||b==ids.end())return fail(error,"Copy the complete NURBS network so every seam endpoint can be remapped");seam.reference=a->second;seam.moving=b->second;}
    set_seams(object,*seams);if(object.polygon){object.polygon->objectId=object.id;object.polygon->contentHash=polygon_asset_content_hash(*object.polygon);}return true;
}
bool canonicalize_nurbs_network_corners(EditorDocument& document,const std::set<EditorObjectId>& selected,std::string* error){
    std::set<EditorObjectId> visited;
    for(const auto& [id,object]:document.objects())if(has_nurbs_seam(object)&&selected.contains(id)&&!visited.contains(id)){
        auto ids=nurbs_seam_network_ids(document,id,error);if(ids.empty())return false;std::vector<const EditorObject*> nodes;std::map<EditorObjectId,NurbsSource> sources;
        for(auto next:ids){visited.insert(next);nodes.push_back(document.find_object(next));auto source=editor_nurbs_source(*nodes.back(),error);if(!source)return false;sources[next]=std::move(*source);}
        auto edges=network_edges(nodes,error);if(!edges)return false;auto junctions=junction_positions(sources,*edges);if(!junctions)return fail(error,"Remapped junction evaluation failed");
        for(auto next:ids){auto* target=document.find_object(next);auto seams=editor_nurbs_seams(*target,error);if(!seams)return false;
            for(auto& seam:*seams){seam.anchors.front()=junctions->at({seam.reference,corner(seam.options.referenceBoundary,false)});seam.anchors.back()=junctions->at({seam.reference,corner(seam.options.referenceBoundary,true)});}
            set_seams(*target,*seams);
        }
        for(auto next:ids){auto* target=document.find_object(next);auto mesh=tessellate_editor_nurbs_display(*target,sources.at(next),error);if(!mesh)return false;appearance(*mesh,*target->polygon);target->polygon=std::move(mesh);}
    }
    return validate_editor_nurbs_seams(document,error);
}

}
