#include "dve/editor_voxel_slice.hpp"
#include "dve/editor_viewport.hpp"
#include "dve/editor_prefab.hpp"
#ifdef DVE_TEST_CPU_PLAYER
#include "dve/player/player_renderer.hpp"
#endif
#include "dve/game_save.hpp"
#include "dve/game_world.hpp"
#include "dve/print_export.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/resumable_destruction.hpp"
#include "dve/runtime_scene.hpp"
#include "dve/voxel_axis_physics.hpp"
#include "dve/voxel_boolean.hpp"
#include <iostream>
#include <stdexcept>
using namespace dve;
namespace {
void check(bool v,const char* m) { if (!v) throw std::runtime_error(m); }
void near(double a,double b,const char* m) {check(std::abs(a-b)<0.001,m);}
GameObjectDesc cube(Float3 axes,bool dynamic,int count=1) {
    GameObjectDesc desc; desc.voxelSizeMeters=1; desc.voxelScale=axes; desc.dynamic=dynamic;
    desc.voxels=std::make_unique<VoxelObject>(1);
    for(int z=0;z<count;++z) for(int y=0;y<count;++y) for(int x=0;x<count;++x)
        (void)desc.voxels->set_voxel({x,y,z},1);
    return desc;
}
void physics_test() {
    VoxelObject v(1); (void)v.set_voxel({0,0,0},1);
    const auto package=build_fragment_solver_package(v,{},MaterialMassTable{});
    auto body=make_rigid_body_desc(package,{1000,1}); check(bool(body),"base descriptor");
    check(scale_voxel_body(*body,{},package.mass.centerOfMass,{2,3,4}),"scaled body valid");
    near(body->massKilograms,24000,"volume mass");
    near(body->inertiaKilogramMetersSquared.xx,50000,"X inertia");
    near(body->inertiaKilogramMetersSquared.yy,40000,"Y inertia");
    near(body->inertiaKilogramMetersSquared.zz,26000,"Z inertia");
    near(body->transform.position.z,2,"scaled center of mass");
    near(body->boxes[0].halfExtents.x,1,"scaled collision extent");
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    std::string error; const auto id=world.create_object(cube({2,0.5F,3},true),&error);
    check(id!=kInvalidGameObjectId,error.c_str());
    const auto hit=world.raycast({-4,0.25F,1.5F},{1,0,0},10);
    check(hit && hit->objectId==id,"dynamic ray"); near(hit->distance,4,"ray meters");
    check(!world.raycast({-4,0.75F,1.5F},{1,0,0},10),"ray misses compressed axis");
    check(world.sphere_overlap({2.1F,0.25F,1.5F},0.2F).size()==1,"scaled overlap");
    const Capsule cap{{-1,0.25F,1.5F},{-1,0.25F,1.5F},0.1F};
    const auto sweep=world.capsule_sweep(cap,{4,0,0});
    check(bool(sweep),"scaled capsule sweep"); near(sweep->time,0.225,"capsule time");
    near(sweep->worldPosition.x,-0.1,"capsule position is meters");
    Capsule embedded{{1.95F,0.25F,1.5F},{1.95F,0.25F,1.5F},0.1F};
    check(world.capsule_overlaps(embedded),"capsule overlap");
    const auto correction=world.depenetrate_capsule(embedded);
    check(length(correction.correction)>0,"capsule depenetration");
    GameSaveCodec codec; GameSaveData data; data.world=world.capture_save_state();
    const auto encoded=codec.encode(data,nullptr,&error); check(bool(encoded),error.c_str());
    auto loaded=codec.decode(*encoded,nullptr,&error); check(bool(loaded),error.c_str());
    near(loaded->world.objects[0].voxelScale.y,0.5,"save metric");
    GameWorld restored(std::make_unique<ReferenceRigidBodyWorld>());
    check(restored.restore_save_state(loaded->world,nullptr,&error),error.c_str());
    check(restored.state_hash()==world.state_hash(),"scaled runtime state round trip");
    auto legacy=codec.to_document(data,nullptr,&error); legacy->schemaVersion=3;
    legacy->sections.erase("dve.voxel_axes");
    SaveGameReadReport report;
    auto old=codec.decode(encode_save_game_document(*legacy),&report,&error);
    check(old && report.migrationsApplied==1 && unit_voxel_scale(old->world.objects[0].voxelScale),"legacy migration defaults unit axes");
    auto corrupt=codec.to_document(data,nullptr,&error);
    corrupt->sections["dve.voxel_axes"][12]=std::byte{0};
    corrupt->sections["dve.voxel_axes"][13]=std::byte{0};
    corrupt->sections["dve.voxel_axes"][14]=std::byte{0};
    corrupt->sections["dve.voxel_axes"][15]=std::byte{0};
    check(!codec.from_document(*corrupt,&error),"zero axis rejected");
}
void geometry_test() {
    VoxelObject v(1); (void)v.set_voxel({0,0,0},1);
    const auto pose=make_rigid_transform({4,5,6},quaternion_from_axis_angle({0,1,0},0.4F));
    const Float3 spacing{2,0.5F,3};
    render::VoxelReferenceInstance instance{1,&v,pose,{},true,spacing};
    render::spwi::VoxelSceneTracer tracer({&instance,1});
    const auto origin=voxel_to_world(pose,spacing,{-2,0.5F,0.5F});
    const auto hit=tracer.trace(origin,rotate(pose.rotation,{1,0,0}),10);
    const auto zero=raycast_voxels_scaled(v,pose,spacing,voxel_to_world(pose,spacing,{0.5F,0.5F,0.5F}),{1,0,0},0);
    check(zero && zero->objectHit.distance==0,"zero-distance origin cell probe");
    check(bool(hit),"reference scaled trace"); near(hit->objectHit.distance,4,"reference world depth");
    check(!tracer.trace(origin,rotate(pose.rotation,{1,0,0}),3.9F),"reference max distance");
    CookedVoxelAsset scaledAsset(1);scaledAsset.voxelScale=spacing;
    scaledAsset.materials.resize(2);(void)scaledAsset.object.set_voxel({0,0,0},1);
    RuntimeSceneObjectMetadata metadata;metadata.id=1;
    const auto validated=validate_dvoxscene_asset(metadata,scaledAsset,{},{});
    check(!validated,validated.message.c_str());
    VoxelObject operand(2); (void)operand.set_voxel({0,0,0},2);
    VoxelBooleanVolume primary{&v,{},1,{},"primary",{2,1,1}};
    VoxelBooleanVolume other{&operand,make_rigid_transform({2,0,0},{}),1,{},"operand",{2,1,1}};
    const auto boolean=compute_voxel_boolean(primary,{&other,1});
    check(boolean.computed && !boolean.has_errors() && boolean.stats.alignedOperands==1 && boolean.stats.addedVoxels==1,"scaled aligned Boolean");
    PrintExportOptions print; print.voxelSizeMeters=1; print.voxelScale=spacing;
    const auto soup=extract_print_surface(v,print); float maxX=0,maxY=0,maxZ=0;
    for(std::size_t i=0;i<soup.positions.size();i+=3) {
        maxX=std::max(maxX,float(soup.positions[i]));maxY=std::max(maxY,float(soup.positions[i+1]));maxZ=std::max(maxZ,float(soup.positions[i+2]));
    }
    near(maxX,2,"print width");near(maxY,0.5,"print height");near(maxZ,3,"print depth");
}
void prefab_test() {
    using namespace dve::editor;
    EditorPrefabAsset prefab("scaled template");EditorObject object(1,"part");
    (void)object.voxels->set_voxel({0,0,0},1);prefab.templateDocument.add_object(std::move(object));
    prefab.rootTemplateObjectIds={1};prefab.contentHash=editor_prefab_content_hash(prefab);
    EditorDocument doc("instances");const auto instance=instantiate_editor_prefab(doc,prefab);
    check(instance.success,instance.error.c_str());const auto id=instance.objectIds[0];
    const auto before=prefab.contentHash;prefab.templateDocument.find_object(1)->voxelScale={2,0.5F,3};
    prefab.contentHash=editor_prefab_content_hash(prefab);check(before!=prefab.contentHash,"axis-only prefab hash");
    check(preview_editor_prefab_instance_update(doc,instance.instanceId,prefab).stale,"axis-only update stale");
    const auto updated=apply_editor_prefab_instance_update(doc,instance.instanceId,prefab);
    check(updated.success,updated.error.c_str());near(doc.find_object(id)->voxelScale.z,3,"prefab propagates axes");
    std::string error;check(apply_editor_prefab_override(doc,id,"voxel.scale",Float3{4,1,1},&error),error.c_str());
    prefab.templateDocument.find_object(1)->voxelScale={5,2,1};prefab.contentHash=editor_prefab_content_hash(prefab);
    check(apply_editor_prefab_instance_update(doc,instance.instanceId,prefab).success,"update with override");
    near(doc.find_object(id)->voxelScale.x,4,"prefab explicit axis override retained");
}
void editor_geometry_test() {
    using namespace dve::editor;
    EditorDocument doc("scaled geometry"); EditorObject object(1,"part");
    object.voxelSizeMeters=1; object.voxelScale={2,0.5F,3};
    (void)object.voxels->set_voxel({0,0,0},1); (void)object.voxels->set_voxel({1,0,0},1);
    doc.add_object(std::move(object));
    const auto bounds=object_world_bounds(*doc.find_object(1));
    near(bounds.maximum.x,4,"editor bounds width");near(bounds.maximum.y,0.5,"editor bounds height");
    const auto picked=pick_editor_document(doc,{{-4,0.25F,1.5F},{1,0,0}});
    check(bool(picked),"editor scaled pick");near(picked->worldDistance,4,"editor pick world distance");
    check(!pick_editor_document(doc,{{-4,0.75F,1.5F},{1,0,0}}),"editor scaled miss");
    EditorVoxelSliceSession slice;
    check(slice.begin(doc,1,{1.5F,0,0},{1,0,0}),"scaled slice begin");
    check(slice.cells().size()==2 && !slice.cells()[0].front && slice.cells()[1].front,"slice classifies scaled centers");
    doc.find_object(1)->voxelScale.x=3;
    check(!slice.target_unchanged(doc),"metric change invalidates slice snapshot");
}
#ifdef DVE_TEST_CPU_PLAYER
void player_test() {
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());
    check(world.create_object(cube({2,0.5F,3},false))!=kInvalidGameObjectId,"player scaled object");
    auto small=cube({1,1,1},false);small.voxelSizeMeters=0.5F;small.transform.position={3,0,0};
    check(world.create_object(std::move(small))!=kInvalidGameObjectId,"player smaller object");
    auto renderer=player::make_cpu_player_renderer(nullptr,{player::CpuRenderQuality::Fast,1});
    std::string error;check(renderer->resize(32,32,&error),error.c_str());
    const auto objects=world.render_objects(); player::PlayerRenderView view{objects};
    view.camera.position={2,2,8};view.camera.target={1.5F,0.25F,1.5F};
    check(renderer->render(view,&error),error.c_str());
    const auto stats=renderer->last_stats();
    check(stats.voxelInstances==2 && stats.skippedObjects==0 && stats.hitRays>0,"player renders mixed metrics");
    check(renderer->readback() && renderer->readback()->valid(),"player framebuffer");
}
#endif
void damage_test() {
    const Float3 axes{2,0.5F,3};
    GameWorld immediate(std::make_unique<ReferenceRigidBodyWorld>());
    const auto id=immediate.create_object(cube(axes,false,3));
    check(immediate.damage_sphere(id,{1,0.25F,1.5F},0.6F)==2,"world sphere removes only nearby physical centers");
    for(const auto& item:immediate.render_objects()) check(item.voxelScale.x==2 && item.voxelScale.z==3,"damage fragments inherit axes");
    GameWorld queued(std::make_unique<ReferenceRigidBodyWorld>());
    const auto queuedId=queued.create_object(cube(axes,false,3));
    queued.set_destruction_units_per_tick(100000);
    check(bool(queued.queue_damage_sphere(queuedId,{1,0.25F,1.5F},0.6F)),"queue scaled damage");
    for(int i=0;i<10 && queued.pending_destruction_count();++i) queued.tick(1.0F/60);
    check(queued.pending_destruction_count()==0 && queued.rejected_destruction_requests()==0,"queued damage published");
    std::uint64_t total=0; for(const auto& item:queued.render_objects()) { total+=item.voxels?item.voxels->occupied_voxel_count():0;check(item.voxelScale.y==0.5F,"queued fragments inherit axes"); }
    check(total==25,"queued metric matches immediate sphere");
}
}
int main(){try{physics_test();geometry_test();prefab_test();editor_geometry_test();damage_test();
#ifdef DVE_TEST_CPU_PLAYER
player_test();
#endif
std::cout<<"Axis runtime passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
