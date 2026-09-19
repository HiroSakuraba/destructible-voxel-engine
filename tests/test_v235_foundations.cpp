#include "dve/v235_foundations.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures{};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<dve::NavigationTriangle> plane() {
    dve::NavigationTriangle a;
    a.vertices={dve::Float3{0,0,0},dve::Float3{0,0,4},dve::Float3{4,0,0}};
    a.area=1U;a.flags=0xFFFFU;a.sourceId=1U;
    dve::NavigationTriangle b;
    b.vertices={dve::Float3{4,0,0},dve::Float3{0,0,4},dve::Float3{4,0,4}};
    b.area=1U;b.flags=0xFFFFU;b.sourceId=2U;
    return {a,b};
}

std::vector<dve::NavigationTriangle> tiled_corridor(float middleHeight = 0.0F) {
    std::vector<dve::NavigationTriangle> triangles;
    for (int tile=0; tile<3; ++tile) {
        const float x0=static_cast<float>(tile*2),x1=x0+2.0F;
        const float height=tile==1?middleHeight:0.0F;
        dve::NavigationTriangle a;
        a.vertices={dve::Float3{x0,height,0},dve::Float3{x0,height,2},dve::Float3{x1,height,0}};
        a.area=1U;a.flags=0xFFFFU;a.sourceId=static_cast<std::uint64_t>(tile*2+1);
        dve::NavigationTriangle b;
        b.vertices={dve::Float3{x1,height,0},dve::Float3{x0,height,2},dve::Float3{x1,height,2}};
        b.area=1U;b.flags=0xFFFFU;b.sourceId=static_cast<std::uint64_t>(tile*2+2);
        triangles.push_back(a);triangles.push_back(b);
    }
    return triangles;
}

void test_navigation() {
    dve::DynamicNavigationWorld world;
    std::string error;
    check(world.set_source_geometry(plane(), {}, &error), "dynamic navigation initial build");
    check(world.revision() == 1U, "navigation revision starts at one");
    dve::NavigationAgent agent;
    check(agent.set_target({3.5F,0,3.5F}, &error), "agent target accepted");
    dve::Float3 position{0.2F,0,0.2F}, velocity{};
    for (int step=0; step<600 && agent.status()!=dve::NavigationAgentStatus::Arrived; ++step) {
        auto output=agent.tick(world.mesh(),world.revision(),position,velocity,1.0F/60.0F);
        velocity=output.desiredVelocity;
        position=dve::add(position,dve::multiply(velocity,1.0F/60.0F));
    }
    check(agent.status()==dve::NavigationAgentStatus::Arrived, "agent follows funnel path to target");
    world.mark_voxel_edit({0,0,0},{7,3,7},0.1F);
    check(!world.dirty_tiles().empty(), "voxel edit dirties navigation tiles");
    check(world.rebuild_dirty(&error), "dirty navigation rebuild succeeds");
    check(world.revision()==2U, "navigation rebuild advances revision");
    const auto path=dve::find_navigation_path(world.mesh(),{0.2F,0,0.2F},{3.5F,0,3.5F});
    check(static_cast<bool>(path), "path continuity survives transactional rebuild");
}

void test_incremental_navigation_tiles() {
    dve::NavigationBuildSettings settings;
    settings.tileSizeMeters=2.0F;settings.agentRadiusMeters=0.1F;settings.maximumPolygons=6U;
    dve::DynamicNavigationWorld world(settings);std::string error;
    const auto original=tiled_corridor();
    check(world.set_source_geometry(original,{},&error),"incremental navigation initial build");
    const auto initialPath=dve::find_navigation_path(world.mesh(),{0.2F,0,1},{5.8F,0,1});
    check(initialPath&&!initialPath.partial,"initial tiled corridor is connected");

    const dve::NavigationBounds middle{{2.01F,-1.0F,0.01F},{3.99F,1.0F,1.99F}};
    check(world.replace_region(middle,{},&error),"destroy middle navigation region");
    check(world.rebuild_dirty(&error),"incremental destruction rebuild");
    const auto broken=dve::find_navigation_path(world.mesh(),{0.2F,0,1},{5.8F,0,1});
    check(!broken||broken.partial,"destroyed tile prevents a complete path");
    check(world.telemetry().lastRebuiltTileCount==1U,"only one dirty tile rebuilt");
    check(world.telemetry().lastReusedPolygonCount==4U,"unchanged tile polygons reused");
    check(world.telemetry().lastRebuiltPolygonCount==0U,"destroyed tile publishes no polygons");

    const auto raised=tiled_corridor(0.2F);
    const std::span<const dve::NavigationTriangle> replacement(raised.data()+2,2U);
    check(world.replace_region(middle,replacement,&error),"restore middle navigation region");
    check(world.rebuild_dirty(&error),"incremental restoration rebuild");
    const auto restored=dve::find_navigation_path(world.mesh(),{0.2F,0,1},{5.8F,0,1});
    check(restored&&!restored.partial,"border stitching restores cross-tile path continuity");
    check(world.telemetry().lastRebuiltTileCount==1U&&
          world.telemetry().lastReusedPolygonCount==4U&&
          world.telemetry().lastRebuiltPolygonCount==2U,
          "incremental rebuild telemetry separates reused and rebuilt polygons");
    check(world.telemetry().lastCandidateTriangleCount<original.size(),
          "incremental rebuild classifies a bounded source subset");
    for (std::uint64_t sourceId : {1U,2U,5U,6U}) {
        const auto before=std::find_if(original.begin(),original.end(),[&](const auto& triangle){
            return triangle.sourceId==sourceId;
        });
        const auto after=std::find_if(world.mesh().polygons.begin(),world.mesh().polygons.end(),
            [&](const auto& polygon){return polygon.sourceId==sourceId;});
        const bool sameGeometry=before!=original.end()&&after!=world.mesh().polygons.end()&&
            std::equal(before->vertices.begin(),before->vertices.end(),after->vertices.begin(),
                [](dve::Float3 a,dve::Float3 b){
                    return a.x==b.x&&a.y==b.y&&a.z==b.z;
                });
        check(sameGeometry,
              "unchanged navigation polygon geometry survives tile replacement");
    }
    const auto stableHash=world.mesh().contentHash;
    world.mark_dirty(middle);
    check(world.rebuild_dirty(&error),"repeat dirty-tile rebuild");
    check(world.mesh().contentHash==stableHash,
          "unchanged incremental tile rebuild is deterministic");

    std::vector<dve::NavigationTriangle> excessive(7U,replacement.front());
    for(std::size_t index=0;index<excessive.size();++index)excessive[index].sourceId=100U+index;
    const auto stableRevision=world.revision();
    check(world.replace_region(middle,excessive,&error),"stage excessive tile replacement");
    check(!world.rebuild_dirty(&error),"polygon-limit failure rejects tile rebuild");
    check(world.revision()==stableRevision&&world.mesh().contentHash==stableHash&&
          world.dirty_tiles().size()==1U,
          "failed tile rebuild preserves live mesh and retry state");
    check(world.replace_region(middle,replacement,&error)&&world.rebuild_dirty(&error),
          "valid replacement recovers a rejected tile transaction");
}

void test_package_and_dependencies(const std::filesystem::path& root) {
    std::filesystem::create_directories(root/"content");
    std::filesystem::create_directories(root/"editor");
    {std::ofstream(root/"content/a.txt")<<"alpha";std::ofstream(root/"content/b.txt")<<"beta";std::ofstream(root/"editor/hidden.txt")<<"hidden";}
    std::vector<std::filesystem::path> inputs{"content/b.txt","editor/hidden.txt","content/a.txt"};
    dve::DvePakManifest manifest;std::string error;
    check(dve::build_dvepak(root,inputs,root/"game.dvepak",{},&manifest,&error),"package build");
    check(manifest.entries.size()==2U,"editor-only content stripped");
    check(manifest.entries[0].path=="content/a.txt","package directory is deterministic");
    dve::DvePakMount mount;check(mount.mount(root/"game.dvepak",&error),"package mount");
    const auto bytes=mount.read("content/a.txt",&error);check(bytes&&bytes->size()==5U,"mounted package read and integrity check");

    dve::AssetDependencyGraph graph;
    check(graph.upsert({"texture",1,{}},&error),"dependency insert root");
    check(graph.upsert({"material",2,{"texture"}},&error),"dependency insert dependent");
    check(graph.upsert({"scene",3,{"material"}},&error),"dependency insert transitive");
    const std::vector<std::string> changed{"texture"};const auto order=graph.deterministic_reimport_order(changed,&error);
    check(order==std::vector<std::string>({"texture","material","scene"}),"dependency reimport order");
    check(graph.rename("texture","albedo",&error),"dependency rename fixup");
    check(graph.validate().empty(),"rename leaves no missing references");
    const std::vector<std::string> roots{"scene"};
    check(graph.dependency_closure(roots,&error)==
        std::vector<std::string>({"albedo","material","scene"}),
        "dependency collection is dependency-first and deterministic");
    dve::SourceMonitor monitor;std::vector<std::filesystem::path> watched{root/"content/a.txt"};
    check(monitor.poll(watched).size()==1U&&monitor.poll(watched).empty(),"source monitor reports changes once");
}

void test_profiler_input_and_save(const std::filesystem::path& root) {
    auto& profiler=dve::Profiler::instance();profiler.begin_frame(7U);
    {dve::ProfileScope scope("unit");int value=0;for(int i=0;i<100;++i)value+=i;check(value==4950,"profiled work executed");}
    profiler.set_counter("agents",3.0);profiler.add_memory("nav",128);const auto frame=profiler.end_frame();
    check(frame.events.size()==1U&&frame.counters.at("agents")==3.0,"profiler captures scopes and counters");
    check(profiler.frame_json(frame).find("\"unit\"")!=std::string::npos,"profiler emits JSON");

    dve::InputActionSystem input;dve::InputContext gameplay;gameplay.name="gameplay";gameplay.priority=10;
    gameplay.bindings.push_back({"jump","Space",{},dve::InputTrigger::Press});
    gameplay.bindings.push_back({"dash","D",{},dve::InputTrigger::DoubleTap,0.35F,0.30F});
    dve::InputBinding horizontal;
    horizontal.action="move_horizontal";horizontal.primary="";horizontal.actuationThreshold=0.25F;
    horizontal.composite={{"Left",-1.0F},{"Right",1.0F}};
    gameplay.bindings.push_back(horizontal);
    gameplay.bindings.push_back({"look","LookAxis",{},dve::InputTrigger::Press,0.35F,0.25F,1.0F,0.5F});
    std::string error;check(input.set_context(gameplay,&error),"input context registration");
    input.set_control("Space",1);input.begin_frame(0.016F);check(input.action("jump")&&input.action("jump")->pressed,"input press action");
    input.set_control("D",1);input.begin_frame(0.016F);input.set_control("D",0);input.begin_frame(0.05F);input.set_control("D",1);input.begin_frame(0.05F);
    check(input.action("dash")&&input.action("dash")->pressed,"double-tap recognition");
    input.set_control("Left",1);input.begin_frame(0.016F);
    check(input.action("move_horizontal")&&std::abs(input.action("move_horizontal")->value+1.0F)<1e-6F&&
        input.action("move_horizontal")->pressed,"weighted composite produces a signed action");
    input.set_control("Right",1);input.begin_frame(0.016F);
    check(!input.action("move_horizontal"),"opposed composite controls cancel deterministically");
    input.set_control("Left",0);input.begin_frame(0.016F);
    check(input.action("move_horizontal")&&std::abs(input.action("move_horizontal")->value-1.0F)<1e-6F&&
        input.action("move_horizontal")->pressed,"composite re-actuates after crossing zero");
    input.set_control("LookAxis",0.25F);input.begin_frame(0.016F);
    check(input.action("look")&&std::abs(input.action("look")->value-0.25F)<1e-6F&&
        !input.action("look")->pressed,"analog values remain visible below their trigger threshold");

    dve::InputBinding sameJump{"", "Space"};
    check(input.rebind("gameplay","jump",sameJump,false,&error),"rebinding ignores the binding being replaced");
    dve::InputBinding conflictingJump{"", "D"};
    check(!input.rebind("gameplay","jump",conflictingJump,false,&error)&&
        error.find("gameplay:dash")!=std::string::npos,"rebinding reports the deterministic conflict owner");
    const auto conflicts=input.conflicts(dve::InputBinding{"candidate","Left"});
    check(std::find(conflicts.begin(),conflicts.end(),"gameplay:move_horizontal")!=conflicts.end(),
        "single controls conflict with composite parts");

    dve::InputContext menu;menu.name="menu";menu.priority=20;menu.consume=true;
    menu.bindings.push_back({"menu_right","Right"});check(input.set_context(menu,&error),"higher priority input context registration");
    input.set_control("Right",0);input.begin_frame(0.016F);input.set_control("Right",1);input.begin_frame(0.016F);
    check(input.action("menu_right")&&input.action("menu_right")->pressed&&!input.action("move_horizontal"),
        "higher priority context consumes a composite part");

    check(input.save_bindings(root/"bindings.tsv",&error),"binding persistence write");
    dve::InputActionSystem loaded;check(loaded.load_bindings(root/"bindings.tsv",&error),"versioned binding persistence read");
    loaded.set_control("Left",1);loaded.begin_frame(0.016F);
    check(loaded.action("move_horizontal")&&std::abs(loaded.action("move_horizontal")->value+1.0F)<1e-6F,
        "composite binding persistence round trip");
    {std::ofstream corrupt(root/"bad-bindings.tsv");corrupt<<"DVE_INPUT_BINDINGS\t99\n";}
    check(!loaded.load_bindings(root/"bad-bindings.tsv",&error),"unsupported binding persistence version is rejected");
    loaded.set_control("Left",0);loaded.begin_frame(0.016F);loaded.set_control("Left",1);loaded.begin_frame(0.016F);
    check(loaded.action("move_horizontal")!=nullptr,"failed binding load is transactional");
    {std::ofstream legacy(root/"legacy-bindings.tsv");legacy<<"legacy\t1\t1\t1\tconfirm\tEnter\t0\t0.35\t0.25\t1\t\n";}
    dve::InputActionSystem legacy;check(legacy.load_bindings(root/"legacy-bindings.tsv",&error),"legacy binding persistence remains readable");
    legacy.set_control("Enter",1);legacy.begin_frame(0.016F);
    check(legacy.action("confirm")&&legacy.action("confirm")->pressed,"legacy binding executes after migration load");

    dve::SaveGameStore store(2U);check(store.register_migration(1U,[](dve::SaveGameDocument& doc,std::string*){doc.sections["migrated"]={std::byte{1}};++doc.schemaVersion;return true;},&error),"save migration registration");
    dve::SaveGameDocument first;first.sequence=1;first.sections["world"]={std::byte{1},std::byte{2}};
    check(store.write_atomic(root/"slot.dvesave",first,&error),"atomic save write one");
    dve::SaveGameDocument second;second.sequence=2;second.sections["world"]={std::byte{3}};
    check(store.write_atomic(root/"slot.dvesave",second,&error),"atomic save write two with backup");
    {std::ofstream corrupt(root/"slot.dvesave",std::ios::binary|std::ios::trunc);corrupt<<"bad";}
    const auto recovered=store.read_recover(root/"slot.dvesave",&error);check(recovered&&recovered->sequence==1U,"corrupt save recovers previous atomic backup");
}

void test_animation_physics_ai() {
    dve::SkeletonAsset source;source.name="source";
    source.bones={{"hips",-1,{{0,1,0},{}}},{"head",0,{{0,1,0},{}}},{"lf",0,{{-0.2F,-1,0},{}}},{"rf",0,{{0.2F,-1,0},{}}}};
    dve::SkeletonAsset target=source;target.name="target";
    dve::HumanoidRigMap map;map.bones={{dve::HumanoidBone::Hips,0},{dve::HumanoidBone::Head,1},{dve::HumanoidBone::LeftFoot,2},{dve::HumanoidBone::RightFoot,3}};
    check(static_cast<bool>(dve::validate_humanoid_rig(source,map)),"humanoid map validation");
    auto sourcePose=dve::make_bind_pose(source);sourcePose[0].position={1,2,3};std::string error;
    auto retargeted=dve::retarget_humanoid_pose(source,sourcePose,map,target,map,&error);check(retargeted.size()==4U&&retargeted[0].position.x==1.0F,"CPU humanoid retarget");
    std::vector<dve::SkinVertex> base(1);base[0].normal={0,1,0};dve::MorphTarget smile{"smile",{{0,{1,0,0},{0,0,0}}}};std::vector<dve::SkinVertex> morphed;
    check(dve::apply_morph_targets(base,std::span<const dve::MorphTarget>(&smile,1),{{"smile",0.5F}},morphed,&error)&&std::abs(morphed[0].position.x-0.5F)<1e-6F,"morph target application");

    dve::ReferenceRigidBodyWorld physics;dve::RigidBodyCreateDesc body;body.transform=dve::make_rigid_transform({},{});body.massKilograms=1.0;body.inertiaKilogramMetersSquared={1,1,1,0,0,0};body.boxes.push_back({{}, {0.5F,0.5F,0.5F}});
    const auto handle=physics.create_body(body);check(handle!=dve::kInvalidRigidBodyHandle,"reference body creation");
    check(physics.set_contact_material(handle,2),"body physical material metadata");
    const auto rayHits=physics.ray_cast_all({-2,0,0},{1,0,0},5.0F);
    check(rayHits.size()==1U&&rayHits[0].body==handle&&rayHits[0].material==2U&&
        std::abs(rayHits[0].distance-1.5F)<1e-6F,"solver-neutral ray query metadata");
    const auto overlapHits=physics.overlap_aabb({{-0.25F,-0.25F,-0.25F},{0.25F,0.25F,0.25F}});
    check(overlapHits.size()==1U,"solver-neutral overlap query");
    const auto sphereHits=physics.cast_sphere_all({-2,0,0},0.25F,{1,0,0},5.0F);
    check(sphereHits.size()==1U&&std::abs(sphereHits[0].distance-1.25F)<1e-6F,
        "solver-neutral sphere cast");
    dve::RigidBodyQueryFilter ignored;ignored.ignoredBodies.push_back(handle);
    check(physics.ray_cast_all({-2,0,0},{1,0,0},5.0F,ignored).empty(),
        "physics queries honor ignored bodies");
    check(physics.apply_impulse_at_point(handle,{1,0,0},{0,1,0}),"off-center impulse accepted");const auto state=physics.state(handle);check(state&&std::abs(state->angularVelocity.z)>0.1F,"off-center impulse creates angular response");
    dve::PhysicsMaterialLibrary materials;check(materials.set(2,{"stone",0.8F,0.1F,2400},&error),"physical material registration");
    std::vector<dve::PhysicsQueryHit> hits{{2,{}, {},0.8F,8,2,0},{1,{}, {},0.2F,2,2,0}};const auto sorted=dve::filter_and_sort_physics_hits(hits);check(sorted.size()==2U&&sorted[0].body==1U,"stable neutral physics query sorting");

    dve::Blackboard blackboard;blackboard.set("has_target",true);int actions{};dve::BehaviorTree tree;
    const auto condition=tree.add({dve::BehaviorTree::Kind::Condition,{},[](const dve::Blackboard& board){const auto* value=board.get("has_target");return value&&std::get<bool>(*value);},{}});
    const auto action=tree.add({dve::BehaviorTree::Kind::Action,{}, {},[&](dve::Blackboard&,float){++actions;return dve::BehaviorStatus::Success;}});
    const auto root=tree.add({dve::BehaviorTree::Kind::Sequence,{condition,action},{},{}});tree.set_root(root);
    check(tree.tick(blackboard,0.016F)==dve::BehaviorStatus::Success&&actions==1,"behavior tree sequence");
    std::vector<dve::PerceptionCandidate> candidates{{2,{3,0,0}},{1,{1,0,0}},{3,{-1,0,0}}};const auto perceived=dve::query_perception({}, {1,0,0},5,0.6F,candidates);
    check(perceived.size()==2U&&perceived[0].id==1U,"perception field and deterministic score order");
}

void test_network_editor_plugins() {
    dve::NetworkTransformInterpolator interpolation;check(interpolation.push({10,{0,0,0},{1,0,0}}),"network sample one");check(interpolation.push({20,{10,0,0},{1,0,0}}),"network sample two");const auto middle=interpolation.sample(15.0);check(middle&&std::abs(middle->position.x-5.0F)<1e-6F,"network interpolation");
    dve::ReplicationRelevancyGrid grid(16);grid.upsert(2,{100,0,0});grid.upsert(1,{2,0,0});check(grid.query({},5)==std::vector<dve::NetworkObjectId>({1}),"replication relevancy query");
    dve::RollbackBuffer<int,int> rollback(3);rollback.push({1,10,100});rollback.push({2,20,200});rollback.push({3,30,300});rollback.push({4,40,400});check(!rollback.find(1)&&rollback.inputs_after(2)==std::vector<int>({300,400}),"bounded rollback buffer");

    int value{};dve::EditorOperationHistory history;std::string error;
    check(history.execute({"increment",[&](std::string*){++value;return true;},[&](std::string*){--value;return true;}},&error),"editor operation execute");check(history.undo(&error)&&value==0,"editor undo");check(history.redo(&error)&&value==1,"editor redo");
    dve::PluginRegistry plugins;check(plugins.register_extension({"sample","assets","Sample Importer","importer"},&error),"plugin extension registration");check(plugins.find("importer","assets")!=nullptr,"plugin lookup");check(plugins.unregister_owner("sample")==1U,"plugin owner unload");
}

} // namespace

int main() {
    const auto root=std::filesystem::temp_directory_path()/"dve_v235_foundation_tests";
    std::error_code ec;std::filesystem::remove_all(root,ec);std::filesystem::create_directories(root,ec);
    test_navigation();test_incremental_navigation_tiles();test_package_and_dependencies(root);test_profiler_input_and_save(root);test_animation_physics_ai();test_network_editor_plugins();
    std::filesystem::remove_all(root,ec);
    if(failures){std::cerr<<failures<<" v2.35 foundation checks failed\n";return 1;}
    std::cout<<"v2.35 foundations: all deterministic checks passed\n";return 0;
}
