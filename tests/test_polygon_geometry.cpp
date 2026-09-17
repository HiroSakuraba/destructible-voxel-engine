#include "dve/geometry_build.hpp"
#include "dve/game_world.hpp"
#include "dve/gameplay_runtime.hpp"
#include "dve/hybrid_geometry_world.hpp"
#include "dve/polygon_collision.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/render/mesh_rhi_mirror.hpp"
#include "dve/rhi/null_device.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(c) do { if(!(c)) throw std::runtime_error(std::string("CHECK failed: ")+#c+" at "+__FILE__+":"+std::to_string(__LINE__)); } while(false)

using namespace dve;

ImportedScene make_scene() {
    ImportedScene scene;
    scene.name = "PolygonTest";
    const auto add_image = [&](std::string name, std::array<std::uint8_t,4> rgba) {
        ImportedImage image;
        image.name = name; image.mimeType = "image/raw"; image.width = 1; image.height = 1;
        image.rgba8.assign(rgba.begin(), rgba.end());
        scene.images.push_back(image);
        scene.textures.push_back({name, static_cast<std::uint32_t>(scene.images.size()-1U), 0U});
    };
    scene.samplers.push_back({});
    add_image("base", {255,255,255,255});
    add_image("metalrough", {0,96,220,255});
    add_image("normal", {128,180,240,255});
    add_image("emissive", {40,100,220,255});
    add_image("opacity", {255,255,255,192});
    ImportedMaterial material;
    material.name = "Leaf";
    material.baseColorFactor = {0.2F,0.7F,0.1F,1.0F};
    material.alphaMode = ImportedAlphaMode::Mask;
    material.alphaCutoff = 0.45F;
    material.doubleSided = true;
    material.baseColorTexture = 0U;
    material.baseColorTexcoord = 0U;
    material.metallicRoughnessTexture = 1U;
    material.metallicRoughnessTexcoord = 1U;
    material.normalTexture = 2U;
    material.normalTexcoord = 0U;
    material.normalScale = 0.75F;
    material.emissiveTexture = 3U;
    material.emissiveTexcoord = 1U;
    material.opacityTexture = 4U;
    material.opacityTexcoord = 1U;
    scene.materials.push_back(material);
    ImportedMesh mesh;
    mesh.name = "Quad";
    mesh.vertices = {
        {{-1,0,-1},{},{0,0},{1,1,1,1},{0.1F,0.2F}},{{1,0,-1},{},{1,0},{1,1,1,1},{0.9F,0.2F}},
        {{1,0,1},{},{1,1},{1,1,1,1},{0.9F,0.8F}},{{-1,0,1},{},{0,1},{1,1,1,1},{0.1F,0.8F}}};
    mesh.triangles = {{{0,1,2},0,0},{{0,2,3},0,0}};
    scene.meshes.push_back(mesh);
    ImportedNode node; node.name="QuadNode"; node.mesh=0; node.worldTransform=Matrix4::identity();
    scene.nodes.push_back(node); scene.roots.push_back(0);
    return scene;
}

std::filesystem::path temp_path() {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()/("dve_polygon_"+std::to_string(stamp)+".dmesh");
}

void test_cook_roundtrip_and_validation() {
    CookedPolygonAsset asset = cook_polygon_scene(make_scene(), {.objectId=77});
    CHECK(validate_polygon_asset(asset));
    CHECK(asset.vertices.size()==4U);
    CHECK(asset.indices.size()==6U);
    CHECK(asset.submeshes.size()==1U);
    CHECK(asset.materials.size()==1U);
    CHECK(asset.materialBindings[0].doubleSided);
    CHECK(asset.materialBindings[0].baseColor.texture==0U);
    CHECK(asset.materialBindings[0].baseColor.colorSpace==PolygonTextureColorSpace::Srgb);
    CHECK(asset.materialBindings[0].metallicRoughness.texture==1U);
    CHECK(asset.materialBindings[0].metallicRoughness.texcoord==1U);
    CHECK(asset.materialBindings[0].metallicRoughness.colorSpace==PolygonTextureColorSpace::Linear);
    CHECK(asset.materialBindings[0].normal.texture==2U);
    CHECK(std::abs(asset.materialBindings[0].normalScale-0.75F)<1.0e-6F);
    CHECK(asset.materialBindings[0].emissive.texture==3U);
    CHECK(asset.materialBindings[0].emissive.texcoord==1U);
    CHECK(asset.materialBindings[0].emissive.colorSpace==PolygonTextureColorSpace::Srgb);
    CHECK(asset.materialBindings[0].opacity.texture==4U);
    CHECK(asset.materialBindings[0].opacity.texcoord==1U);
    CHECK(asset.images.size()==5U);
    CHECK(std::abs(asset.vertices[0].texcoord1.x-0.1F)<1.0e-6F);
    CHECK(std::abs(asset.vertices[0].normal.y) > 0.99F);
    CHECK(std::abs(asset.vertices[0].tangent.w) == 1.0F);
    VoxelMaterialDefinition rust;rust.name="Rust Layer";rust.baseColor={0.6F,0.12F,0.03F,1.0F};
    asset.materials.push_back(rust);asset.materialBindings.push_back({});
    asset.materials[0].layers.push_back({1U,0.65F,MaterialLayerBlendMode::Lerp,true});
    PolygonMaterialLayerBinding layerBinding;layerBinding.semantic=MaterialLayerSemantic::Rust;
    layerBinding.mask.texture=4U;layerBinding.mask.texcoord=1U;layerBinding.height.texture=1U;
    layerBinding.heightBlendStrength=0.75F;layerBinding.heightBlendBias=-0.1F;
    layerBinding.heightBlendTransition=0.25F;layerBinding.opacityPolicy=MaterialLayerOpacityPolicy::PreserveBase;
    asset.materialBindings[0].layers.push_back(layerBinding);
    asset.contentHash=polygon_asset_content_hash(asset);CHECK(validate_polygon_asset(asset));
    const auto path=temp_path();std::string error;
    CHECK(write_dmesh(path,asset,&error));
    const PolygonAssetReadResult read=read_dmesh(path);
    CHECK(read);
    CHECK(read.asset.contentHash==asset.contentHash);
    CHECK(read.asset.indices==asset.indices);
    CHECK(read.asset.images[0].rgba8==asset.images[0].rgba8);
    CHECK(read.asset.materialBindings[0].normal.texture==2U);
    CHECK(read.asset.materialBindings[0].emissive.texcoord==1U);
    CHECK(read.asset.materialBindings[0].opacity.texture==4U);
    CHECK(read.asset.materials[0].layers.size()==1U);
    CHECK(read.asset.materialBindings[0].layers.size()==1U);
    CHECK(read.asset.materialBindings[0].layers[0].semantic==MaterialLayerSemantic::Rust);
    CHECK(read.asset.materialBindings[0].layers[0].mask.texcoord==1U);
    CHECK(std::abs(read.asset.materialBindings[0].layers[0].heightBlendStrength-0.75F)<1.0e-6F);
    CHECK(std::abs(read.asset.vertices[0].texcoord1.x-asset.vertices[0].texcoord1.x)<1.0e-6F);
    CHECK(std::abs(read.asset.vertices[0].texcoord1.y-asset.vertices[0].texcoord1.y)<1.0e-6F);
    std::filesystem::remove(path);
}

void test_corruption_rejected() {
    CookedPolygonAsset asset=cook_polygon_scene(make_scene());const auto path=temp_path();std::string error;CHECK(write_dmesh(path,asset,&error));
    std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);CHECK(file.good());file.seekp(20);char c='X';file.write(&c,1);file.close();
    const auto read=read_dmesh(path);CHECK(!read);std::filesystem::remove(path);
}

void test_runtime_world_collision_and_rhi() {
    CookedPolygonAsset asset=cook_polygon_scene(make_scene(),{.objectId=9});
    ReferenceRuntimeMeshWorld meshWorld;
    const auto mh=meshWorld.create_object({9,make_rigid_transform({1,2,3},{}),&asset});CHECK(mh!=kInvalidRuntimeMeshHandle);CHECK(meshWorld.readback_hash(mh)==asset.contentHash);CHECK(meshWorld.counts().vertices==4U);
    CHECK(meshWorld.update_object(mh,{9,make_rigid_transform({4,5,6},{}),&asset}));CHECK(meshWorld.world_transform(mh)->position.x==4.0F);
    const auto staticDesc=make_polygon_static_body_desc(asset,{});CHECK(staticDesc);CHECK(staticDesc->boxes.size()==1U);
    const auto dynamicDesc=make_polygon_dynamic_body_desc(asset,{}, {.densityKilogramsPerCubicMeter=500.0});
    CHECK(dynamicDesc);
    CHECK(dynamicDesc->massKilograms > 0.0);
    rhi::NullDevice device;render::MeshRhiMirror mirror(device);std::string error;CHECK(mirror.upload(asset,&error));CHECK(mirror.readback_matches(asset,&error));CHECK(mirror.stats().publications==1U);
}


void test_game_world_polygon_spawn_and_queries() {
    CookedPolygonAsset asset = cook_polygon_scene(make_scene(), {.objectId=33});
    const auto path = temp_path();
    std::string error;
    CHECK(write_dmesh(path, asset, &error));
    auto physics = std::make_unique<ReferenceRigidBodyWorld>();
    GameWorld world(std::move(physics));
    const GameObjectId id = world.spawn_asset(path, "PolygonFloor", make_rigid_transform({0,0,0}, {}), false, true, &error);
    if (geometry_kind_supported(GeometryKind::Polygon)) {
        CHECK(id != kInvalidGameObjectId);
        CHECK(world.geometry_kind(id) == GameGeometryKind::Polygon);
        CHECK(!world.voxel_count(id));
        const auto hit = world.raycast({0,1,0}, {0,-1,0}, 4.0F);
        CHECK(hit);
        CHECK(hit->objectId == id);
        CHECK(std::abs(hit->distance - 1.0F) < 1.0e-4F);
        CHECK(!world.damage_sphere(id, {0,0,0}, 1.0F));
        const auto overlaps = world.sphere_overlap({0,0,0}, 0.25F);
        CHECK(std::find(overlaps.begin(), overlaps.end(), id) != overlaps.end());
    } else {
        CHECK(id == kInvalidGameObjectId);
    }
    std::filesystem::remove(path);
}

CookedPolygonAsset make_horizontal_floor_asset() {
    ImportedScene scene;
    scene.name = "ExactCapsuleFloor";
    ImportedMaterial material;
    material.name = "Floor";
    scene.materials.push_back(material);
    ImportedMesh mesh;
    mesh.name = "Floor";
    mesh.vertices = {
        {{-2,-2,0},{0,0,1},{0,0},{1,1,1,1},{}},
        {{ 2,-2,0},{0,0,1},{1,0},{1,1,1,1},{}},
        {{ 2, 2,0},{0,0,1},{1,1},{1,1,1,1},{}},
        {{-2, 2,0},{0,0,1},{0,1},{1,1,1,1},{}}};
    mesh.triangles = {{{0,1,2},0,0},{{0,2,3},0,0}};
    scene.meshes.push_back(mesh);
    ImportedNode node;
    node.name = "Floor";
    node.mesh = 0;
    node.worldTransform = Matrix4::identity();
    scene.nodes.push_back(node);
    scene.roots.push_back(0);
    return cook_polygon_scene(scene, {.objectId=101});
}

void test_exact_polygon_capsule_queries() {
    const CookedPolygonAsset asset = make_horizontal_floor_asset();
    PolygonBvh bvh;
    std::string error;
    CHECK(bvh.build(asset, &error));

    Capsule separated{{0,0,0.30F},{0,0,1.30F},0.25F};
    CHECK(!bvh.capsule_overlap(separated));
    Capsule touching{{0,0,0.25F},{0,0,1.25F},0.25F};
    const auto touchingContact = bvh.capsule_overlap(touching);
    CHECK(touchingContact);
    CHECK(touchingContact->distance <= 0.25001F);
    CHECK(touchingContact->normal.z > 0.99F);

    Capsule start{{0,0,1.0F},{0,0,2.0F},0.25F};
    const auto sweep = bvh.sweep_capsule(start, {0,0,-2.0F});
    CHECK(sweep);
    CHECK(std::abs(sweep->time - 0.375F) < 2.0e-3F);
    CHECK(sweep->normal.z > 0.99F);
    CHECK(sweep->materialIndex == 0U);

    // The capsule AABB overlaps the floor bounds, but the exact triangles do not.
    Capsule outside{{2.30F,2.30F,0.0F},{2.30F,2.30F,1.0F},0.25F};
    CHECK(!bvh.capsule_overlap(outside));

    const auto path = temp_path();
    CHECK(write_dmesh(path, asset, &error));
    auto physics = std::make_unique<ReferenceRigidBodyWorld>();
    GameWorld world(std::move(physics));
    const GameObjectId floor = world.spawn_polygon_asset(path, "Exact Floor", {}, false, true, &error);
    if (geometry_kind_supported(GeometryKind::Polygon)) {
        CHECK(floor != kInvalidGameObjectId);
        const auto worldSweep = world.capsule_sweep(start, {0,0,-2.0F});
        CHECK(worldSweep);
        CHECK(worldSweep->objectId == floor);
        CHECK(std::abs(worldSweep->time - 0.375F) < 2.0e-3F);
        CHECK(worldSweep->worldNormal.z > 0.99F);

        Capsule penetrating{{0,0,0.10F},{0,0,1.10F},0.25F};
        CHECK(world.capsule_overlaps(penetrating));
        const auto resolution = world.depenetrate_capsule(penetrating, kInvalidGameObjectId, 4U, 0.001F);
        CHECK(resolution.iterations > 0U);
        CHECK(resolution.correction.z > 0.14F);
        CHECK(!world.capsule_overlaps(penetrating));
    }
    std::filesystem::remove(path);
}


void test_gameplay_controller_on_exact_polygon_floor() {
    if (!geometry_kind_supported(GeometryKind::Polygon)) return;
    const CookedPolygonAsset asset = make_horizontal_floor_asset();
    const auto path = temp_path();
    std::string error;
    CHECK(write_dmesh(path, asset, &error));

    auto physics = std::make_unique<ReferenceRigidBodyWorld>();
    physics->set_gravity({0,0,0});
    GameWorld world(std::move(physics));
    const GameObjectId floor = world.spawn_polygon_asset(
        path, "Playable Polygon Floor", {}, false, true, &error);
    CHECK(floor != kInvalidGameObjectId);

    GameObjectDesc pawnDesc;
    pawnDesc.name = "Polygon Pawn";
    pawnDesc.tags = {"player"};
    pawnDesc.transform = make_rigid_transform({0,0,0.25F}, {});
    const GameObjectId pawn = world.create_object(std::move(pawnDesc), &error);
    CHECK(pawn != kInvalidGameObjectId);
    CHECK(world.gameplay().add_character(pawn, {}, &error));
    const GamePlayerId player = world.gameplay().create_player("Polygon Player", true);
    CHECK(world.gameplay().possess(player, pawn, &error));

    for (int i = 0; i < 30; ++i) world.tick(1.0F / 60.0F);
    const CharacterControllerState* settled = world.gameplay().character(pawn);
    CHECK(settled != nullptr);
    CHECK(settled->grounded);
    CHECK(settled->groundNormal.z > 0.99F);
    CHECK(settled->supportObject == floor);

    CHECK(world.gameplay().set_player_input(player, {{1,0,0}, false, false}));
    for (int i = 0; i < 20; ++i) world.tick(1.0F / 60.0F);
    CHECK(world.position(pawn)->x > 0.15F);
    CHECK(world.gameplay().character(pawn)->grounded);

    std::filesystem::remove(path);
}

void test_hybrid_profile_and_publication() {
    VoxelObject object(1);object.set_voxel({0,0,0},1);PackedBrickmapScene packed;packed.rebuild(object);
    CookedPolygonAsset asset=cook_polygon_scene(make_scene(),{.objectId=2});
    ReferenceRuntimeBrickmapWorld voxels;ReferenceRuntimeMeshWorld meshes;HybridGeometryWorld world(&voxels,&meshes);std::string error;
    const auto vh=world.create_voxel_object({1,{},&packed},&error);const auto ph=world.create_polygon_object({2,{},&asset},&error);
    CHECK((vh!=kInvalidRuntimeGeometryHandle)==geometry_kind_supported(GeometryKind::Voxel));
    CHECK((ph!=kInvalidRuntimeGeometryHandle)==geometry_kind_supported(GeometryKind::Polygon));
    const auto stats=world.stats();
    CHECK(stats.voxelObjects==(geometry_kind_supported(GeometryKind::Voxel)?1U:0U));
    CHECK(stats.polygonObjects==(geometry_kind_supported(GeometryKind::Polygon)?1U:0U));
    if(vh!=kInvalidRuntimeGeometryHandle){CHECK(world.update_transform(vh,make_rigid_transform({1,0,0},{}),&error));CHECK(world.destroy_object(vh));}
    if(ph!=kInvalidRuntimeGeometryHandle){CHECK(world.update_transform(ph,make_rigid_transform({0,1,0},{}),&error));CHECK(world.destroy_object(ph));}
}
}

int main(){try{test_cook_roundtrip_and_validation();test_corruption_rejected();test_runtime_world_collision_and_rhi();test_game_world_polygon_spawn_and_queries();test_exact_polygon_capsule_queries();test_gameplay_controller_on_exact_polygon_floor();test_hybrid_profile_and_publication();std::cout<<"polygon geometry tests: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
