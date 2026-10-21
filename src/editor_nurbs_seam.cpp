#include "dve/editor_nurbs_seam.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
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
            if(seam.options.c1&&std::abs(da[k]*scaleA-db[k]*scaleB)>1e-7*(1+std::abs(da[k]*scaleA)))return fail(error,"Linked seam C1 continuity is stale");}
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
class SeamCommand final:public IEditorCommand {
public:
    enum class Mode{Link,Refresh,Unlink,Edit};
    SeamCommand(Mode mode,EditorObjectId id,EditorObjectId moving=0,NurbsAttachmentOptions options={},std::optional<NurbsSource> before={},std::optional<NurbsSource> after={}):mode_(mode),id_(id),moving_(moving),options_(options),expected_(std::move(before)),value_(std::move(after)){}
    std::string_view label() const noexcept override{return mode_==Mode::Unlink?"Unlink NURBS seam":"Update linked NURBS seam";}
    CommandResult execute(EditorDocument& document) override{
        if(before_.empty()){
            std::string error;auto* selected=document.find_object(id_);if(!selected)return CommandResult::fail("NURBS seam target is missing");
            EditorObjectId reference=id_,moving=moving_;
            if(mode_!=Mode::Link){auto seam=editor_nurbs_seam(*selected,&error);if(!seam)return CommandResult::fail(error.empty()?"Select a linked NURBS patch":error);reference=seam->reference;moving=seam->moving;options_=seam->options;}
            auto* a=document.find_object(reference);auto* b=document.find_object(moving);
            if(!a||!b||a==b)return CommandResult::fail("NURBS seam needs two existing distinct patches");
            for(auto* object:{a,b})if(object->flags.locked||!object->flags.visible||object->prefabLink||!validate_editor_nurbs(*object,&error))return CommandResult::fail(error.empty()?"Both linked patches must be visible, unlocked and owned":error);
            if(mode_==Mode::Link&&(has_nurbs_seam(*a)||has_nurbs_seam(*b)))return CommandResult::fail("This batch supports one seam per patch; unlink the existing seam first");
            if(mode_!=Mode::Link&&!validate_editor_nurbs_seams(document,&error))return CommandResult::fail(error);
            std::vector<EditorObject> after;after.push_back(clone_editor_object(*a));after.push_back(clone_editor_object(*b));
            if(mode_==Mode::Edit){auto current=editor_nurbs_source(*selected,&error);if(!current||!expected_||*current!=*expected_||!value_)return CommandResult::fail("Linked NURBS source changed; reopen the edit");
                if(current->u!=value_->u||current->v!=value_->v||current->columns!=value_->columns||current->rows!=value_->rows)return CommandResult::fail("Unlink the seam before changing degree, knots or parameterization");
                if(id_==moving){const bool v=v_edge(options_.movingBoundary),minimum=min_edge(options_.movingBoundary);const auto count=v?current->rows:current->columns;
                    for(std::size_t i=0;i<current->controls.size();++i){const auto cross=v?unsigned(i/current->columns):unsigned(i%current->columns);const auto distance=minimum?cross:count-1-cross;
                        if(distance<=(options_.c1?1U:0U)&&current->controls[i]!=value_->controls[i])return CommandResult::fail("This boundary/tangent row is driven by the reference; edit the reference or unlink first");}
                }
                set_source(after[reference==id_?0:1],*value_);
            }
            if(mode_==Mode::Unlink){for(auto& object:after){clear_seam(object);auto source=editor_nurbs_source(object,&error);if(!source||!replace_editor_nurbs_source(object,*source,&error))return CommandResult::fail(error);}}
            else if(!prepare_pair(after[0],after[1],options_,&error))return CommandResult::fail(error);
            before_.push_back(clone_editor_object(*a));before_.push_back(clone_editor_object(*b));after_=std::move(after);
        }
        return apply(document,before_,after_);
    }
    CommandResult undo(EditorDocument& document) override{return apply(document,after_,before_);}
private:
    CommandResult apply(EditorDocument& document,const std::vector<EditorObject>& expected,const std::vector<EditorObject>& values){
        if(expected.size()!=2||values.size()!=2)return CommandResult::fail("NURBS seam history is incomplete");
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
std::optional<NurbsSeam> editor_nurbs_seam(const EditorObject& object,std::string* error){
    auto invalid=[&]()->std::optional<NurbsSeam>{fail(error,"Invalid or unsupported NURBS seam metadata");return {};};const auto* text=encoded(object);if(!text||text->size()>1024*1024)return invalid();
    std::istringstream in(*text);in.imbue(std::locale::classic());std::string magic;unsigned version,a,b,reverse,c1;NurbsSeam seam;
    if(!(in>>magic>>version>>seam.reference>>seam.moving>>a>>b>>reverse>>c1>>seam.referenceStamp>>seam.movingStamp)||magic!="DVE_NURBS_SEAM"||version!=1||a>3||b>3||reverse>1||c1>1||!seam.reference||!seam.moving||seam.reference==seam.moving||(object.id!=seam.reference&&object.id!=seam.moving))return invalid();
    seam.options={static_cast<NurbsBoundary>(a),static_cast<NurbsBoundary>(b),reverse!=0,c1!=0,true};
    for(auto* values:{&seam.grid.u,&seam.grid.v,&seam.fractions}){unsigned count;if(!(in>>count)||count<2||count>4096)return invalid();values->resize(count);for(std::size_t i=0;i<count;++i)if(!(in>>(*values)[i])||!std::isfinite((*values)[i])||(i&&!((*values)[i]>(*values)[i-1])))return invalid();}
    if(seam.fractions.front()!=0||seam.fractions.back()!=1)return invalid();seam.anchors.resize(seam.fractions.size());for(auto& p:seam.anchors)if(!(in>>p.x>>p.y>>p.z)||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||std::abs(p.x)>1e8||std::abs(p.y)>1e8||std::abs(p.z)>1e8)return invalid();in>>std::ws;if(!in.eof())return invalid();return seam;
}
std::optional<CookedPolygonAsset> tessellate_editor_nurbs_display(const EditorObject& object,const NurbsSource& source,std::string* error,std::stop_token stop){
    if(!has_nurbs_seam(object))return tessellate_nurbs(source,object.id,error,stop);
    auto seam=editor_nurbs_seam(object,error);if(!seam)return {};const bool reference=object.id==seam->reference;const auto stamp=reference?seam->referenceStamp:seam->movingStamp;if(stamp!=nurbs_source_stamp(source)){fail(error,"Linked NURBS source is stale; use a coordinated seam edit");return {};}
    auto mesh=tessellate_nurbs_grid(source,object.id,seam->grid,error,stop);if(!mesh)return {};
    const auto edge=reference?seam->options.referenceBoundary:seam->options.movingBoundary;auto params=parameters(source,edge,seam->fractions,!reference&&seam->options.reverseBoundary);const auto& values=v_edge(edge)?seam->grid.u:seam->grid.v;
    if(params.size()!=values.size()){fail(error,"Shared seam schedule is incomplete");return {};}
    for(double p:params)if(std::find(values.begin(),values.end(),p)==values.end()){fail(error,"Shared seam schedule lost a parameter");return {};}
    for(std::size_t i=0;i<params.size();++i){if(stop.stop_requested())return {};auto p=boundary(source,edge,params[i]);if(!p)return {};auto a=seam->anchors[i];if(reference&&(float(p->position[0])!=a.x||float(p->position[1])!=a.y||float(p->position[2])!=a.z)){fail(error,"Reference seam anchors are stale");return {};}for(unsigned k=0;k<3;++k){double x=p->position[k],y=k==0?a.x:k==1?a.y:a.z;if(std::abs(x-y)>8*std::numeric_limits<float>::epsilon()*(1+std::abs(x))){fail(error,"Shared seam anchor diverged from its NURBS boundary");return {};}}}
    const auto columns=seam->grid.u.size()-1;
    for(std::size_t j=0;j+1<seam->grid.v.size();++j)for(std::size_t i=0;i+1<seam->grid.u.size();++i)for(unsigned k=0;k<4;++k){const auto u=i+(k==1||k==2),v=j+(k>=2);const bool on=v_edge(edge)?(min_edge(edge)?v==0:v+1==seam->grid.v.size()):(min_edge(edge)?u==0:u+1==seam->grid.u.size());if(!on)continue;
        const double t=v_edge(edge)?seam->grid.u[u]:seam->grid.v[v];auto it=std::find(params.begin(),params.end(),t);if(it==params.end()){fail(error,"Shared seam vertex is not scheduled");return {};}mesh->vertices[(j*columns+i)*4+k].position=seam->anchors[std::size_t(it-params.begin())];}
    for(std::size_t i=0;i<mesh->indices.size();i+=3){if(stop.stop_requested())return {};auto a=mesh->vertices[mesh->indices[i]].position,b=mesh->vertices[mesh->indices[i+1]].position,c=mesh->vertices[mesh->indices[i+2]].position;const double x=(double(b.y)-a.y)*(double(c.z)-a.z)-(double(b.z)-a.z)*(double(c.y)-a.y),y=(double(b.z)-a.z)*(double(c.x)-a.x)-(double(b.x)-a.x)*(double(c.z)-a.z),z=(double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x);if(!(std::hypot(x,y,z)>0)){fail(error,"Shared seam triangles collapse at float precision");return {};}}
    mesh->bounds.minimum=mesh->bounds.maximum=mesh->vertices.front().position;for(const auto& v:mesh->vertices){auto p=v.position;mesh->bounds.minimum={(std::min)(mesh->bounds.minimum.x,p.x),(std::min)(mesh->bounds.minimum.y,p.y),(std::min)(mesh->bounds.minimum.z,p.z)};mesh->bounds.maximum={(std::max)(mesh->bounds.maximum.x,p.x),(std::max)(mesh->bounds.maximum.y,p.y),(std::max)(mesh->bounds.maximum.z,p.z)};}mesh->contentHash=polygon_asset_content_hash(*mesh);return mesh;
}
bool validate_editor_nurbs_seams(const EditorDocument& document,std::string* error){
    for(const auto& [id,object]:document.objects())if(has_nurbs_seam(object)){
        auto seam=editor_nurbs_seam(object,error);if(!seam)return false;const auto* a=document.find_object(seam->reference);const auto* b=document.find_object(seam->moving);
        if(!a||!b||!same_frame(*a,*b)||a->prefabLink||b->prefabLink)return fail(error,"NURBS seam partner/frame is missing or changed; unlink before independent placement or prefab operations");
        auto other=editor_nurbs_seam(id==a->id?*b:*a,error);if(!other)return false;
        if(other->reference!=seam->reference||other->moving!=seam->moving||other->options.referenceBoundary!=seam->options.referenceBoundary||other->options.movingBoundary!=seam->options.movingBoundary||other->options.reverseBoundary!=seam->options.reverseBoundary||other->options.c1!=seam->options.c1||other->referenceStamp!=seam->referenceStamp||other->movingStamp!=seam->movingStamp||other->fractions!=seam->fractions||other->anchors.size()!=seam->anchors.size())return fail(error,"NURBS seam relationship is not reciprocal");
        for(std::size_t i=0;i<seam->anchors.size();++i){auto x=seam->anchors[i],y=other->anchors[i];if(x.x!=y.x||x.y!=y.y||x.z!=y.z)return fail(error,"NURBS seam anchors disagree");}
        auto sa=editor_nurbs_source(*a,error),sb=editor_nurbs_source(*b,error);if(!sa||!sb||nurbs_source_stamp(*sa)!=seam->referenceStamp||nurbs_source_stamp(*sb)!=seam->movingStamp)return fail(error,"Linked NURBS source stamps are stale");
        if(id==seam->reference&&!continuity(*sa,*sb,*seam,error))return false;
    }
    return true;
}
std::unique_ptr<IEditorCommand> link_nurbs_seam_command(EditorObjectId a,EditorObjectId b,NurbsAttachmentOptions options){return std::make_unique<SeamCommand>(SeamCommand::Mode::Link,a,b,options);}
std::unique_ptr<IEditorCommand> unlink_nurbs_seam_command(EditorObjectId id){return std::make_unique<SeamCommand>(SeamCommand::Mode::Unlink,id);}
std::unique_ptr<IEditorCommand> refresh_nurbs_seam_command(EditorObjectId id){return std::make_unique<SeamCommand>(SeamCommand::Mode::Refresh,id);}
std::unique_ptr<IEditorCommand> edit_nurbs_seam_command(EditorObjectId id,const NurbsSource& before,const NurbsSource& after){return std::make_unique<SeamCommand>(SeamCommand::Mode::Edit,id,0,NurbsAttachmentOptions{},before,after);}
namespace {
bool pair_state_matches(const EditorDocument& document,const NurbsSeamDragSnapshot& pair,bool editable,bool allowMissing=false){
    for(const auto* saved:{&pair.reference,&pair.moving}){
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
    for(const auto* saved:{&pair.reference,&pair.moving}){
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
    auto seam=editor_nurbs_seam(object);auto source=editor_nurbs_source(object);
    if(!seam||!source)return true;
    return std::any_of(indices.begin(),indices.end(),[&](auto i){return driven_control(*source,*seam,object.id,i);});
}
std::shared_ptr<const NurbsSeamDragSnapshot> capture_nurbs_seam_drag(const EditorDocument& document,EditorObjectId id,std::string* error){
    const auto* object=document.find_object(id);if(!object)return {};
    auto seam=editor_nurbs_seam(*object,error);if(!seam||!validate_editor_nurbs_seams(document,error))return {};
    auto* a=document.find_object(seam->reference);auto* b=document.find_object(seam->moving);
    if(!a||!b)return {};
    auto pair=std::make_shared<NurbsSeamDragSnapshot>();
    pair->reference=clone_editor_object(*a);pair->moving=clone_editor_object(*b);
    if(!pair_state_matches(document,*pair,true)){fail(error,"Both linked patches must be visible, unlocked, owned and valid");return {};}
    // History keeps before/after pairs; publication always takes appearance from the
    // live objects, so retain only 1x1 image placeholders (indices stay valid).
    for(auto* object:{&pair->reference,&pair->moving})for(auto& image:object->polygon->images){image.width=image.height=1;image.rgba8.assign(4,255);image.rgba8.shrink_to_fit();}
    return pair;
}
std::unique_ptr<PreparedNurbsSeamEdit> PreparedNurbsSeamEdit::prepare(std::shared_ptr<const NurbsSeamDragSnapshot> snapshot,EditorObjectId id,const NurbsSource& source,std::string* error,std::stop_token stop){
    if(stop.stop_requested()||!snapshot)return {};
    if(id!=snapshot->reference.id&&id!=snapshot->moving.id){fail(error,"Edited patch is outside the seam snapshot");return {};}
    const auto& selected=id==snapshot->reference.id?snapshot->reference:snapshot->moving;
    auto before=editor_nurbs_source(selected,error);auto seam=editor_nurbs_seam(snapshot->reference,error);
    if(!before||!seam||!validate_nurbs(source,error)||
       !validate_editor_nurbs(snapshot->reference,error,stop)||!validate_editor_nurbs(snapshot->moving,error,stop))return {};
    if(before->u!=source.u||before->v!=source.v||before->columns!=source.columns||before->rows!=source.rows){fail(error,"Unlink before changing linked parameterization");return {};}
    for(std::size_t i=0;i<source.controls.size();++i)if(source.controls[i]!=before->controls[i]&&driven_control(*before,*seam,id,i)){fail(error,"This boundary/tangent row is driven by the reference; select free controls or edit the reference");return {};}
    auto edit=std::make_unique<PreparedNurbsSeamEdit>();edit->before_=std::move(snapshot);
    edit->after_.reference=clone_editor_object(edit->before_->reference);edit->after_.moving=clone_editor_object(edit->before_->moving);
    if(source==*before)return edit; // Exact identity: do not renormalize weights or resample.
    set_source(id==edit->after_.reference.id?edit->after_.reference:edit->after_.moving,source);
    if(!prepare_pair(edit->after_.reference,edit->after_.moving,seam->options,error,stop)||stop.stop_requested())return {};
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
    return publish_pair(document,*before_,error,true);
}
CommandResult PreparedNurbsSeamEdit::execute(EditorDocument& document){
    std::string error;if(!matches(document,false)||!publish(document,true,&error))return CommandResult::fail(error.empty()?"Linked source, placement or display changed; history was not applied":error);
    document.mark_dirty();return CommandResult::ok();
}
CommandResult PreparedNurbsSeamEdit::undo(EditorDocument& document){
    std::string error;if(!matches(document,true)||!publish(document,false,&error))return CommandResult::fail(error.empty()?"Linked source, placement or display changed; history was not applied":error);
    document.mark_dirty();return CommandResult::ok();
}

}
