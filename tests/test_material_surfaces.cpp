#include "dve/editor_native.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_material_preview.hpp"
#include "dve/dvox.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/render/scene_lighting.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
using namespace dve;using namespace dve::editor;using namespace dve::render;
namespace {
void check(bool ok,const std::string& what){if(!ok)throw std::runtime_error(what);}
void near(float a,float b,float tolerance,const std::string& what){if(!std::isfinite(a)||!std::isfinite(b)||std::abs(a-b)>tolerance)throw std::runtime_error(what+": "+std::to_string(a)+" vs "+std::to_string(b));}
std::filesystem::path dir;
void gradients_and_filtering(){
    for(unsigned pattern=1;pattern<=5;++pattern){MaterialSurfaceDetail d;d.pattern=SurfacePattern(pattern);d.seed=7;d.periodMeters=.03F;d.rotationRadians={.2F,.3F,-.1F};
        const Float3 p{.014F,.023F,.037F};const auto analytic=sample_surface_pattern(d,p,.0001F);
        const float epsilon=.000002F;
        for(unsigned axis=0;axis<3;++axis){auto a=p,b=p;float *ap=axis==0?&a.x:axis==1?&a.y:&a.z,*bp=axis==0?&b.x:axis==1?&b.y:&b.z;*ap+=epsilon;*bp-=epsilon;
            const float derivative=(sample_surface_pattern(d,a,.0001F).value-sample_surface_pattern(d,b,.0001F).value)/(2*epsilon);
            const float expected=axis==0?analytic.gradientPerMeter.x:axis==1?analytic.gradientPerMeter.y:analytic.gradientPerMeter.z;
            near(derivative,expected,.3F+.015F*std::abs(expected),"analytic pattern derivative");}
        const auto far=sample_surface_pattern(d,p,2);near(far.value,0,1e-5F,"minified pattern converges to neutral mean");near(length(far.gradientPerMeter),0,.001F,"minified bump disappears");
        const auto stable=sample_surface_pattern(d,p,.0001F);near(stable.value,analytic.value,0,"reproducible seed");
        ++d.seed;check(std::abs(sample_surface_pattern(d,p,.0001F).value-analytic.value)>1e-5F,"seed changes pattern");
        check(std::isfinite(sample_surface_pattern(d,{1e30F,0,0}).value),"huge coordinates stay finite");
    }
}
void metric_mapping(){
    const auto patch=material_preview_fixture({},MaterialPreviewShape::Patch);
    near(patch.bounds.minimum.x,-.5F,1e-6F,"patch stays centered in studio");
    near(patch.bounds.maximum.x,.5F,1e-6F,"patch fits studio framing");
    auto catalog=MaterialPresetCatalog::starter();auto base=catalog.find("dve.wood.raw")->defaults;
    const auto hash=material_surface_detail_hash(base.surfaceDetail);auto changed=base;changed.surfaceDetail.seed++;check(material_surface_detail_hash(changed.surfaceDetail)!=hash,"seed participates in hash");
    MaterialSurfaceContext c;c.objectPositionMeters={.017F,.023F,.037F};c.worldPositionMeters=c.objectPositionMeters;c.footprintMeters=.0001F;
    auto a=base;Float3 n{0,0,1};apply_material_surface_detail(a,n,c);check(std::abs(a.baseColor.x-base.baseColor.x)>1e-5F&&std::abs(a.roughness-base.roughness)>1e-5F,"color and roughness maps both affect shading");
    auto b=base;Float3 nb{0,0,1};c.worldPositionMeters.x+=12;apply_material_surface_detail(b,nb,c);near(a.baseColor.x,b.baseColor.x,0,"object mapping follows translation");
    b=base;b.surfaceDetail.space=SurfaceDetailSpace::World;nb={0,0,1};apply_material_surface_detail(b,nb,c);check(std::abs(a.baseColor.x-b.baseColor.x)>1e-5F,"world mapping stays anchored");
    // Rigidly rotating an object rotates its bump, preserving the scalar pattern.
    c.pose.rotation=quaternion_from_axis_angle({0,1,0},.7F);c.worldPositionMeters=transform_point(c.pose,c.objectPositionMeters);
    b=base;nb=transform_vector(c.pose,{0,0,1});apply_material_surface_detail(b,nb,c);near(a.baseColor.x,b.baseColor.x,1e-6F,"rotating object preserves grain color");near(length(subtract(transform_vector(c.pose,n),nb)),0,1e-5F,"bump rotates with object");
    auto coarse=base;Float3 coarseN{0,0,1};c.footprintMeters=1;apply_material_surface_detail(coarse,coarseN,c);near(coarse.baseColor.x,base.baseColor.x,1e-5F,"distant color filters to base");
    auto coat=catalog.find("dve.wood.varnished")->defaults;const float coatRoughness=coat.clearCoatRoughness;apply_material_surface_detail(coat,coarseN,c);near(coat.clearCoatRoughness,coatRoughness,0,"base grain does not modify coat roughness");
    auto invalid=base;const auto old=invalid.surfaceDetail;std::string error;check(!set_material_preset_value(invalid,MaterialPresetProperty::PatternScale,.00001F,&error)&&material_surface_detail_hash(invalid.surfaceDetail)==material_surface_detail_hash(old),"invalid coupled edit leaves material unchanged");
    check(!set_material_preset_value(invalid,MaterialPresetProperty::Pattern,1.5F,&error),"fractional enum refused");
    auto original=*catalog.find("dve.wood.raw");auto instance=instantiate_material_preset(original,{});
    check(set_material_preset_value(instance,MaterialPresetProperty::BumpHeight,.000001F,&error),error);
    check(set_material_preset_value(instance,MaterialPresetProperty::PatternScale,.00001F,&error),error);
    original.revision++;check(update_material_preset(instance,original,true,&error),error);
    near(instance.surfaceDetail.periodMeters,.00001F,0,"coupled scale override survives update");
    near(instance.surfaceDetail.bumpHeightMeters,.000001F,0,"coupled bump override survives update");
}
void persistence(){
    auto catalog=MaterialPresetCatalog::starter();std::string error;
    for(const auto& a:catalog.entries())if(a.defaults.surfaceDetail.pattern!=SurfacePattern::NoPattern){
        auto m=instantiate_material_preset(a,{});const auto hash=material_surface_detail_hash(m.surfaceDetail);
        check(write_material_preset(dir/"detail.dvepreset",a,&error),error);const auto preset=read_material_preset(dir/"detail.dvepreset",&error);check(preset&&material_surface_detail_hash(preset->defaults.surfaceDetail)==hash,"preset retains all procedural settings");
        auto mesh=make_editor_polygon_cube(1);mesh.materials[0]=m;mesh.contentHash=polygon_asset_content_hash(mesh);check(write_dmesh(dir/"detail.dmesh",mesh,&error),error);auto read=read_dmesh(dir/"detail.dmesh");check(bool(read),read.error);check(material_surface_detail_hash(read.asset.materials[0].surfaceDetail)==hash,"DMESH retains detail");
        CookedVoxelAsset voxel(1);voxel.materials={VoxelMaterialDefinition{},m};voxel.object.set_voxel({0,0,0},1);check(write_dvox(dir/"detail.dvox",voxel,{},&error),error);auto v=read_dvox(dir/"detail.dvox");check(v.success,v.error);check(material_surface_detail_hash(v.asset.materials[1].surfaceDetail)==hash,"DVOX retains detail");
        {auto& uv=voxel;uv.materials[1].surfaceDetail.space=SurfaceDetailSpace::UV0;std::string uvError;check(!write_dvox(dir/"uv.dvox",uv,{},&uvError)&&!std::filesystem::exists(dir/"uv.dvox"),"DVOX refuses UV-space voxel detail");}
        EditorDocument document("Surface detail");EditorObject object(1,"Sample");object.polygon=std::move(mesh);document.add_object(std::move(object));const auto saved=document.save_transactional(dir/"detail.dvescene");check(saved.success,saved.error);const auto scene=EditorDocument::load(dir/"detail.dvescene",&error);check(scene&&material_surface_detail_hash(scene->find_object(1)->polygon->materials[0].surfaceDetail)==hash,"scene retains detail");
    }
}
CookedPolygonAsset textured_plane(){
    auto a=make_editor_polygon_plane(1,2);for(auto& v:a.vertices){const auto y=v.position.y;v.position.y=v.position.z;v.position.z=y;v.normal={0,0,1};v.tangent={1,0,0,1};v.color={1,1,1,1};}
    a.materials[0].baseColor={.5F,.5F,.5F,1};a.materials[0].roughness=.8F;a.materials[0].emissive={1,1,1};
    a.images={{"color","raw",1,1,{255,0,0,255}},{"data","raw",1,1,{255,64,128,255}},{"normal","raw",1,1,{200,128,230,255}},{"emission","raw",1,1,{0,255,0,255}}};
    for(unsigned i=0;i<4;++i)a.textures.push_back({"map",i,{}});
    a.materialBindings[0].baseColor.texture=0;a.materialBindings[0].metallicRoughness.texture=1;a.materialBindings[0].normal.texture=2;a.materialBindings[0].emissive.texture=3;
    refresh_editor_polygon(a);return a;
}
std::vector<PolygonTextureMipChain> mips(const CookedPolygonAsset& a){std::vector<PolygonTextureMipChain> r;for(const auto& image:a.images){auto chain=generate_texture_mips(image);chain.normalLevels=generate_texture_mips(image,true).levels;chain.srgbLevels=generate_texture_mips(image,false,true).levels;r.push_back(std::move(chain));}return r;}
void ray_hit_maps(){
    auto a=textured_plane();PolygonBvh bvh;check(bvh.build(a),"textured fixture BVH");auto hit=bvh.raycast({0,0,2},{0,0,-1},5);check(bool(hit),"fixture hit");
    auto sample=sample_polygon_hit_material(a,*hit,{}, {0,0,1},mips(a),1,.001F,2);
    near(sample.material.baseColor.x,.5F,1e-5F,"ray color map");near(sample.material.baseColor.y,0,1e-5F,"ray color channel");near(sample.material.roughness,.8F*64/255,1e-5F,"roughness data stays linear");near(sample.material.emissive.y,1,1e-5F,"emissive map");near(sample.material.emissive.x,0,1e-5F,"emissive channel");const auto t=a.vertices[0].tangent;check(dot(sample.normal,{t.x,t.y,t.z})>.2F,"normal map tilts along authored tangent");
    // Camera continuation must see these maps, rather than just the constant emitter factor.
    RenderEnvironment e;e.sunIntensity=0;e.skyColor={};e.groundColor={};e.pathTraceBounces=4;e.pathTraceSamples=64;e.lights.clear();
    std::array<PolygonRenderInstance,1> objects{{{1,&a}}};SceneLighting lighting({},objects,e,1);
    auto radiance=lighting.camera_transport({0,0,2},{0,0,-1},5,17).sum();near(radiance.x,0,1e-5F,"camera continuation samples emissive red channel");check(radiance.y>.9F,"camera continuation samples emissive green channel");
    // On object-triplanar maps UVs must not choose the normal-map mip.
    a.materialBindings[0].mapping.mappingMode=MaterialMappingMode::ObjectTriplanar;
    auto& image=a.images[2];image.width=32;image.height=32;image.rgba8.resize(32*32*4);
    for(unsigned i=0;i<32*32;++i){image.rgba8[i*4]=(i%32)%2?240:16;image.rgba8[i*4+1]=128;image.rgba8[i*4+2]=180;image.rgba8[i*4+3]=255;}
    a.contentHash=polygon_asset_content_hash(a);auto before=sample_polygon_hit_material(a,*hit,{}, {0,0,1},mips(a),1,.2F,2);
    for(auto& v:a.vertices){v.texcoord.x*=100000;v.texcoord.y*=100000;}a.contentHash=polygon_asset_content_hash(a);
    auto after=sample_polygon_hit_material(a,*hit,{}, {0,0,1},mips(a),1,.2F,2);near(length(subtract(before.normal,after.normal)),0,1e-6F,"triplanar normal filtering is independent of unused UVs");
}
void reflected_and_refracted_maps(){
    auto background=textured_plane();
    background.materialBindings[0].normal.texture.reset();
    background.materials[0].baseColor={0,0,0,1};
    for(auto& v:background.vertices)v.position.z=-2;
    refresh_editor_polygon(background);
    auto glass=make_editor_polygon_cube(2,1);
    for(auto& v:glass.vertices)v.position=subtract(v.position,{.5F,.5F,.5F});
    auto& m=glass.materials[0];m.transmission=1;m.roughness=0;m.indexOfRefraction=1.5F;
    refresh_editor_polygon(glass);
    RenderEnvironment e;e.sunIntensity=0;e.skyColor={};e.groundColor={};e.pathTraceBounces=8;e.pathTraceSamples=128;e.globalIlluminationMaxDistanceMeters=20;e.lights.clear();
    std::array<PolygonRenderInstance,2> objects{{{1,&glass},{2,&background}}};
    SceneLighting refracted({},objects,e,1);
    const auto through=refracted.camera_transport({0,0,2},{0,0,-1},20,23).sum();
    check(through.y>.5F&&through.x<1e-5F,"refracted continuation samples green emissive texture");
    auto mirror=textured_plane();mirror.images.clear();mirror.textures.clear();mirror.materialBindings[0]={};
    mirror.materials[0]={};mirror.materials[0].metallic=1;mirror.materials[0].roughness=0;mirror.materials[0].baseColor={1,1,1,1};refresh_editor_polygon(mirror);
    for(auto& v:background.vertices)v.position.z=3;refresh_editor_polygon(background);
    objects[0].asset=&mirror;
    SceneLighting reflected({},objects,e,1);
    const auto bounce=reflected.camera_transport({0,0,2},{0,0,-1},20,23).sum();
    check(bounce.y>.5F&&bounce.x<1e-5F,"reflected continuation samples green emissive texture");
}
void voxel_uv_assets(){
    CookedVoxelAsset asset(99);asset.materials.resize(2);asset.object.set_voxel({0,0,0},1);
    auto& detail=asset.materials[1].surfaceDetail;detail.pattern=SurfacePattern::Wood;detail.space=SurfaceDetailSpace::UV0;
    std::string error;check(!write_dvox(dir/"bad-uv.dvox",asset,{},&error),"cooker rejects UV recipe on voxel assets");
    detail.space=SurfaceDetailSpace::UV1;check(!write_dvox(dir/"bad-uv.dvox",asset,{},&error),"cooker rejects secondary UV recipe on voxel assets");
    detail.space=SurfaceDetailSpace::World;check(write_dvox(dir/"world.dvox",asset,{},&error),error);
}
void assignment(){
    EditorDocument d("UV refusal");EditorObject object(1,"Voxel");object.voxels->set_voxel({0,0,0},1);d.add_object(std::move(object));NativeEditorController n{EditorWorkspace(std::move(d))};n.workspace().select_object(1);n.open_material_panel();n.material_panel_select("dve.wood.raw");
    n.material_panel().draft.surfaceDetail.space=SurfaceDetailSpace::UV0;const auto count=n.workspace().commands().size();check(!n.apply_material_panel()&&n.workspace().commands().size()==count,"UV detail on voxels refuses atomically");
    n.material_panel().draft.surfaceDetail.space=SurfaceDetailSpace::Object;check(n.apply_material_panel(),n.material_panel().message);const auto& o=*n.workspace().document().find_object(1);const auto slot=o.voxels->material_at({0,0,0});check(o.voxelMaterials.at(slot).surfaceDetail.pattern==SurfacePattern::Wood,"voxel assignment retains recipe");n.close_material_panel();
}
}
int main(){try{dir=std::filesystem::temp_directory_path()/"dve-material-surfaces";std::filesystem::remove_all(dir);std::filesystem::create_directories(dir);gradients_and_filtering();metric_mapping();persistence();ray_hit_maps();reflected_and_refracted_maps();voxel_uv_assets();assignment();std::filesystem::remove_all(dir);std::cout<<"Surface maps, filtering, persistence and ray-hit checks passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
