#include "dve/editor_nurbs_surface.hpp"
#include "dve/editor_nurbs_seam.hpp"
#include "dve/nurbs_modeling.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
namespace dve::editor {
namespace {
bool fail(std::string* e,const char* text){if(e)*e=text;return false;}
void appearance(CookedPolygonAsset& a,const CookedPolygonAsset& b){a.materials=b.materials;a.materialBindings=b.materialBindings;a.images=b.images;a.samplers=b.samplers;a.textures=b.textures;a.contentHash=polygon_asset_content_hash(a);}
void store(EditorObject& object,const NurbsSurfaceFeatures& features){auto* c=find_component_by_type(std::span<Component>(object.components),kNurbsComponent);if(features.curves.empty()&&!features.trim)c->properties.erase(std::string(kNurbsSurfaceFeatures));else c->properties[std::string(kNurbsSurfaceFeatures)]=encode_nurbs_surface_features(features);}
bool same_metadata(const EditorObject& a,const EditorObject& b){const auto* x=find_component_by_type(a.components,kNurbsComponent);const auto* y=find_component_by_type(b.components,kNurbsComponent);if(!x||!y)return false;
    for(auto key:{std::string_view("source"),kNurbsSeamProperty,kNurbsSurfaceFeatures}){auto i=x->properties.find(key),j=y->properties.find(key);if((i==x->properties.end())!=(j==y->properties.end()))return false;if(i!=x->properties.end()){const auto* u=std::get_if<std::string>(&i->second);const auto* v=std::get_if<std::string>(&j->second);if(!u||!v||*u!=*v)return false;}}return true;}
Float3 scaled(Float3 p,Float3 s){return {p.x*s.x,p.y*s.y,p.z*s.z};}
}
bool validate_nurbs_surface_features(const NurbsSurfaceFeatures& f,std::string* error){
    if(f.curves.size()>32)return fail(error,"At most 32 persistent surface curves are supported");std::set<std::uint64_t> ids;
    for(const auto& c:f.curves){if(!c.id||!ids.insert(c.id).second||c.samples<3||c.samples>257||!c.uv.is_curve()||!validate_nurbs(c.uv,error))return fail(error,"Invalid persistent UV curve identity, sampling or source");
        for(auto p:c.uv.controls)if(p.x<0||p.x>1||p.y<0||p.y>1||p.z!=0)return fail(error,"UV curve controls must lie in normalized UV with z=0");}
    return !f.trim||validate_nurbs_trim(*f.trim,error);
}
std::string encode_nurbs_surface_features(const NurbsSurfaceFeatures& f){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<"DVE_NURBS_SURFACE_FEATURES 1 "<<f.curves.size()<<' ';
    for(const auto& c:f.curves)out<<c.id<<' '<<c.projectionSource<<' '<<c.samples<<' '<<std::quoted(encode_nurbs(c.uv))<<' ';
    out<<bool(f.trim)<<' ';if(f.trim){const auto& t=*f.trim;out<<t.inverted<<' '<<t.loops.size()<<' ';for(const auto& l:t.loops){out<<l.id<<' '<<l.points.size()<<' ';for(auto p:l.points)out<<p[0]<<' '<<p[1]<<' ';}out<<t.seeds.size()<<' ';for(auto p:t.seeds)out<<p[0]<<' '<<p[1]<<' ';}return out.str();}
std::optional<NurbsSurfaceFeatures> decode_nurbs_surface_features(std::string_view text,std::string* error){
    const auto refuse=[&]()->std::optional<NurbsSurfaceFeatures>{fail(error,"Invalid or unsupported persistent surface feature metadata");return {};};if(text.size()>1024*1024)return refuse();
    std::istringstream in{std::string(text)};in.imbue(std::locale::classic());std::string magic;unsigned version,count,present;
    if(!(in>>magic>>version>>count)||magic!="DVE_NURBS_SURFACE_FEATURES"||version!=1||count>32)return refuse();NurbsSurfaceFeatures f;
    for(unsigned i=0;i<count;++i){NurbsSurfaceCurve c;std::string uv;if(!(in>>c.id>>c.projectionSource>>c.samples>>std::quoted(uv)))return refuse();auto source=decode_nurbs(uv,error);if(!source)return {};c.uv=std::move(*source);f.curves.push_back(std::move(c));}
    if(!(in>>present)||present>1)return refuse();if(present){f.trim.emplace();unsigned inverted,loops;if(!(in>>inverted>>loops)||inverted>1||loops==0||loops>32)return refuse();f.trim->inverted=inverted!=0;
        std::size_t total=0;for(unsigned i=0;i<loops;++i){NurbsTrimLoop l;unsigned size;if(!(in>>l.id>>size)||size<3||(total+=size)>2048)return refuse();l.points.resize(size);for(auto& p:l.points)if(!(in>>p[0]>>p[1]))return refuse();f.trim->loops.push_back(std::move(l));}
        unsigned seeds;if(!(in>>seeds)||seeds>32)return refuse();f.trim->seeds.resize(seeds);for(auto& p:f.trim->seeds)if(!(in>>p[0]>>p[1]))return refuse();}
    in>>std::ws;if(!in.eof()||!validate_nurbs_surface_features(f,error))return refuse();return f;
}
std::optional<NurbsSurfaceFeatures> editor_nurbs_surface_features(const EditorObject& o,std::string* error){const auto* c=find_component_by_type(o.components,kNurbsComponent);if(!c){fail(error,"Missing NURBS source");return {};}
    const auto it=c->properties.find(kNurbsSurfaceFeatures);if(it==c->properties.end())return NurbsSurfaceFeatures{};const auto* text=std::get_if<std::string>(&it->second);if(!text){fail(error,"Invalid surface feature property type");return {};}auto f=decode_nurbs_surface_features(*text,error);auto s=editor_nurbs_source(o,error);if(!s||s->is_curve()||(f&&std::any_of(f->curves.begin(),f->curves.end(),[&](const auto& curve){return curve.projectionSource==o.id;}))){fail(error,"Surface curves require a surface and cannot project from themselves");return {};}return f;
}
std::optional<std::vector<NurbsUV>> sample_nurbs_uv_curve(const NurbsSurfaceCurve& c,bool closed,std::string* error){NurbsSurfaceFeatures f;f.curves.push_back(c);if(!validate_nurbs_surface_features(f,error))return {};std::vector<NurbsUV> points;
    for(unsigned i=0;i<c.samples;++i){auto p=evaluate_nurbs(c.uv,std::lerp(c.uv.u.knots.front(),c.uv.u.knots.back(),double(i)/(c.samples-1)),0,error);if(!p)return {};points.push_back({p->position[0],p->position[1]});}
    if(closed){if(std::hypot(points.front()[0]-points.back()[0],points.front()[1]-points.back()[1])>1e-9){fail(error,"Trim curve must be closed in UV");return {};}points.pop_back();}
    return points;
}
std::optional<NurbsSurfaceCurve> project_editor_nurbs_surface_curve(const EditorDocument& d,EditorObjectId target,EditorObjectId curve,std::uint64_t curveId,unsigned samples,std::string* error){
    const auto* a=d.find_object(target);const auto* b=d.find_object(curve);if(!a||!b||target==curve||samples<3||samples>257){fail(error,"Projection requires distinct NURBS curve/surface and 3..257 samples");return {};}
    auto surface=editor_nurbs_source(*a,error),source=editor_nurbs_source(*b,error);if(!surface||!source||surface->is_curve()||!source->is_curve()){fail(error,"Projection target must be a surface and its source a curve");return {};}
    if(a->voxelScale.x<=0||a->voxelScale.y<=0||a->voxelScale.z<=0){fail(error,"Projection target scale must be positive");return {};}
    const auto from=d.world_transform(curve).value_or(b->transform),to=d.world_transform(target).value_or(a->transform);
    NurbsSurfaceCurve result;result.id=curveId;result.projectionSource=curve;result.samples=samples;result.uv.columns=samples;result.uv.u.degree=1;result.uv.u.knots={0,0};
    for(unsigned i=1;i+1<samples;++i)result.uv.u.knots.push_back(double(i)/(samples-1));result.uv.u.knots.insert(result.uv.u.knots.end(),{1,1});
    for(unsigned i=0;i<samples;++i){auto sample=evaluate_nurbs(*source,std::lerp(source->u.knots.front(),source->u.knots.back(),double(i)/(samples-1)),0,error);if(!sample)return {};
        auto world=transform_point(from,scaled({float(sample->position[0]),float(sample->position[1]),float(sample->position[2])},b->voxelScale));auto local=inverse_transform_point(to,world);local={local.x/a->voxelScale.x,local.y/a->voxelScale.y,local.z/a->voxelScale.z};
        auto p=project_nurbs_point(*surface,{double(local.x),double(local.y),double(local.z)},1e-7,error);if(!p)return {};
        result.uv.controls.push_back({(p->u-surface->u.knots.front())/(surface->u.knots.back()-surface->u.knots.front()),(p->v-surface->v.knots.front())/(surface->v.knots.back()-surface->v.knots.front()),0,1});}
    NurbsSurfaceFeatures f;f.curves.push_back(result);if(!validate_nurbs_surface_features(f,error))return {};return result;
}
std::optional<std::vector<Float3>> evaluate_editor_nurbs_surface_curve(const EditorDocument& d,EditorObjectId id,std::uint64_t curve,std::string* error){const auto* o=d.find_object(id);if(!o){fail(error,"Surface is missing");return {};}auto f=editor_nurbs_surface_features(*o,error);auto source=editor_nurbs_source(*o,error);if(!f||!source)return {};auto it=std::find_if(f->curves.begin(),f->curves.end(),[&](const auto& c){return c.id==curve;});if(it==f->curves.end()){fail(error,"Surface curve is missing");return {};}
    auto uv=sample_nurbs_uv_curve(*it,false,error);if(!uv)return {};std::vector<Float3> result;const auto pose=d.world_transform(id).value_or(o->transform);
    for(auto q:*uv){auto p=evaluate_nurbs(*source,std::lerp(source->u.knots.front(),source->u.knots.back(),q[0]),std::lerp(source->v.knots.front(),source->v.knots.back(),q[1]),error);if(!p)return {};result.push_back(transform_point(pose,scaled({float(p->position[0]),float(p->position[1]),float(p->position[2])},o->voxelScale)));}return result;
}
std::optional<NurbsDisplayGrid> editor_nurbs_surface_grid(const EditorObject& o,const NurbsSource& s,std::string* error,std::stop_token stop){auto grid=nurbs_display_grid(s,nullptr,error,stop);if(!grid)return {};auto f=editor_nurbs_surface_features(o,error);if(!f)return {};return f->trim?nurbs_trim_grid(s,*grid,*f->trim,error):grid;}
namespace {
class FeaturesCommand final:public IEditorCommand {
public:
    enum class Mode{Set,Project,Refresh,Trim,TrimCurve,Region};
    FeaturesCommand(EditorObjectId id,Mode mode):id_(id),mode_(mode){}
    std::optional<NurbsSurfaceFeatures> expected,value;std::optional<NurbsTrimRegion> trim;std::uint64_t curve{};unsigned samples{65};NurbsUV seed{};bool add{};
    std::string_view label()const noexcept override{return "Edit NURBS surface curves and trim regions";}
    CommandResult execute(EditorDocument& doc)override{
        if(before_.empty()){
            auto* o=doc.find_object(id_);std::string error;if(!o||!o->polygon||o->flags.locked||!o->flags.visible||o->prefabLink)return CommandResult::fail("Select a visible unlocked owned NURBS surface");
            auto source=editor_nurbs_source(*o,&error);auto old=editor_nurbs_surface_features(*o,&error);if(!source||source->is_curve()||!old||!validate_editor_nurbs(*o,&error)||!validate_editor_nurbs_seams(doc,&error))return CommandResult::fail(error);
            if(expected&&*expected!=*old)return CommandResult::fail("Surface curves or trimming changed; reopen the edit");auto next=value.value_or(*old);
            if(mode_==Mode::Project){std::uint64_t nextId=1;for(const auto& c:next.curves)nextId=std::max(nextId,c.id+1);if(next.trim)for(const auto& l:next.trim->loops)nextId=std::max(nextId,l.id+1);auto projected=project_editor_nurbs_surface_curve(doc,id_,curve,nextId,samples,&error);if(!projected)return CommandResult::fail(error);next.curves.push_back(std::move(*projected));}
            if(mode_==Mode::Refresh){auto it=std::find_if(next.curves.begin(),next.curves.end(),[&](const auto& c){return c.id==curve;});if(it==next.curves.end()||!it->projectionSource)return CommandResult::fail("Curve has no projection relationship");auto projected=project_editor_nurbs_surface_curve(doc,id_,it->projectionSource,it->id,it->samples,&error);if(!projected)return CommandResult::fail(error);*it=std::move(*projected);}
            if(mode_==Mode::Trim)next.trim=trim;
            if(mode_==Mode::TrimCurve){auto it=std::find_if(next.curves.begin(),next.curves.end(),[&](const auto& c){return c.id==curve;});if(it==next.curves.end())return CommandResult::fail("Surface curve is missing");auto points=sample_nurbs_uv_curve(*it,true,&error);if(!points)return CommandResult::fail(error);if(!next.trim)next.trim.emplace();auto existing=std::find_if(next.trim->loops.begin(),next.trim->loops.end(),[&](const auto& l){return l.id==curve;});if(existing==next.trim->loops.end())next.trim->loops.push_back({curve,*points});else existing->points=*points;next.trim->seeds.clear();}
            if(mode_==Mode::Region){if(!next.trim)return CommandResult::fail("Surface has no trim loops");if(!add)next.trim->seeds.clear();next.trim->seeds.push_back(seed);next.trim->inverted=false;}
            // Editing a curve used by a trim updates its boundary in the same transaction.
            if(next.trim&&(mode_==Mode::Set||mode_==Mode::Refresh))for(auto& loop:next.trim->loops){auto it=std::find_if(next.curves.begin(),next.curves.end(),[&](const auto& c){return c.id==loop.id;});if(it!=next.curves.end()){auto points=sample_nurbs_uv_curve(*it,true,&error);if(!points)return CommandResult::fail(error);loop.points=*points;}}
            if(!validate_nurbs_surface_features(next,&error))return CommandResult::fail(error);
            auto ids=nurbs_seam_network_ids(doc,id_,&error);if(ids.empty())return CommandResult::fail(error);
            for(auto id:ids){const auto* object=doc.find_object(id);if(!object||!object->flags.visible||object->flags.locked||object->prefabLink)return CommandResult::fail("Every linked surface must be visible, unlocked and owned");}
            auto staged=clone_editor_document(doc);store(*staged.find_object(id_),next);if(!rebuild_editor_nurbs_network_display(staged,id_,&error)||!staged.validate(&error))return CommandResult::fail(error);
            for(auto id:ids){before_.push_back(clone_editor_object(*doc.find_object(id)));after_.push_back(clone_editor_object(*staged.find_object(id)));}
        }
        return apply(doc,before_,after_);
    }
    CommandResult undo(EditorDocument& doc)override{return apply(doc,after_,before_);}
private:
    CommandResult apply(EditorDocument& doc,const std::vector<EditorObject>& old,const std::vector<EditorObject>& values){
        for(const auto& saved:old){const auto* o=doc.find_object(saved.id);if(!o||!o->polygon||o->flags.locked||!o->flags.visible||o->prefabLink||!nurbs_seam_same_frame(*o,saved)||editor_nurbs_source(*o)!=editor_nurbs_source(saved)||editor_nurbs_surface_features(*o)!=editor_nurbs_surface_features(saved)||!same_metadata(*o,saved)||!nurbs_display_geometry_matches(*o->polygon,*saved.polygon))return CommandResult::fail("Surface history is stale; no changes applied");}
        std::vector<CookedPolygonAsset> meshes;std::vector<Component> components;
        for(const auto& saved:values){auto* o=doc.find_object(saved.id);auto mesh=*saved.polygon;appearance(mesh,*o->polygon);auto valid=validate_polygon_asset(mesh);if(!valid)return CommandResult::fail(valid.message);meshes.push_back(std::move(mesh));auto component=*find_component_by_type(std::span<const Component>(o->components),kNurbsComponent);const auto& props=find_component_by_type(saved.components,kNurbsComponent)->properties;
            for(auto key:{std::string_view("source"),kNurbsSeamProperty,kNurbsSurfaceFeatures}){auto it=props.find(key);if(it==props.end())component.properties.erase(std::string(key));else component.properties[std::string(key)]=it->second;}components.push_back(std::move(component));}
        for(std::size_t i=0;i<values.size();++i){auto* o=doc.find_object(values[i].id);o->polygon=std::move(meshes[i]);*find_component_by_type(std::span<Component>(o->components),kNurbsComponent)=std::move(components[i]);}doc.mark_dirty();return CommandResult::ok();
    }
    EditorObjectId id_;Mode mode_;std::vector<EditorObject> before_,after_;
};
}
std::unique_ptr<IEditorCommand> set_nurbs_surface_features_command(EditorObjectId id,const NurbsSurfaceFeatures& a,const NurbsSurfaceFeatures& b){auto c=std::make_unique<FeaturesCommand>(id,FeaturesCommand::Mode::Set);c->expected=a;c->value=b;return c;}
std::unique_ptr<IEditorCommand> project_nurbs_surface_curve_command(EditorObjectId id,EditorObjectId curve,unsigned samples){auto c=std::make_unique<FeaturesCommand>(id,FeaturesCommand::Mode::Project);c->curve=curve;c->samples=samples;return c;}
std::unique_ptr<IEditorCommand> refresh_nurbs_surface_curve_command(EditorObjectId id,std::uint64_t curve){auto c=std::make_unique<FeaturesCommand>(id,FeaturesCommand::Mode::Refresh);c->curve=curve;return c;}
std::unique_ptr<IEditorCommand> trim_nurbs_surface_command(EditorObjectId id,std::optional<NurbsTrimRegion> trim){auto c=std::make_unique<FeaturesCommand>(id,FeaturesCommand::Mode::Trim);c->trim=std::move(trim);return c;}
std::unique_ptr<IEditorCommand> trim_nurbs_surface_curve_command(EditorObjectId id,std::uint64_t curve){auto c=std::make_unique<FeaturesCommand>(id,FeaturesCommand::Mode::TrimCurve);c->curve=curve;return c;}
std::unique_ptr<IEditorCommand> select_nurbs_trim_region_command(EditorObjectId id,NurbsUV seed,bool add){auto c=std::make_unique<FeaturesCommand>(id,FeaturesCommand::Mode::Region);c->seed=seed;c->add=add;return c;}
}
