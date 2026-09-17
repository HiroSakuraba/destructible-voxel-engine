#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "dve/navigation_geometry.hpp"
#include "dve/navigation_mesh.hpp"

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

void add_quad(std::vector<dve::NavigationTriangle>& out,float x0,float z0,float x1,float z1,float y=0.0F){
    const dve::Float3 a{x0,y,z0},b{x1,y,z0},c{x1,y,z1},d{x0,y,z1};
    out.push_back({{a,c,b},1U,0xFFFFU,out.size()});
    out.push_back({{a,d,c},1U,0xFFFFU,out.size()});
}

void test_build_and_path(){
    std::vector<dve::NavigationTriangle> triangles;
    for(int z=0;z<3;++z)for(int x=0;x<3;++x)add_quad(triangles,static_cast<float>(x),static_cast<float>(z),static_cast<float>(x+1),static_cast<float>(z+1));
    dve::NavigationBuildSettings settings;settings.agentRadiusMeters=0.05F;settings.tileSizeMeters=2.0F;
    const auto built=dve::build_navigation_mesh(triangles,settings);
    require(built.success(),"navigation build failed");
    require(built.mesh.polygons.size()==18U,"unexpected navigation polygon count");
    const auto path=dve::find_navigation_path(built.mesh,{0.1F,0.0F,0.1F},{2.9F,0.0F,2.9F});
    require(static_cast<bool>(path),"navigation path failed");
    require(!path.partial,"navigation path unexpectedly partial");
    require(path.points.size()>=2U,"navigation path has too few points");
    require(path.corridor.size()>=3U,"navigation corridor too short");
    require(std::abs(path.points.front().position.x-0.1F)<0.01F,"path start was not projected correctly");
    require(std::abs(path.points.back().position.z-2.9F)<0.01F,"path end was not projected correctly");

    dve::NavigationObstacle wall;wall.bounds={{0.95F,-1.0F,-0.1F},{2.05F,1.0F,3.1F}};
    const auto blocked=dve::find_navigation_path(built.mesh,{0.1F,0.0F,1.5F},{2.9F,0.0F,1.5F},{},std::span<const dve::NavigationObstacle>(&wall,1U));
    require(!blocked,"blocking obstacle should separate the plane");
}

void test_slope_and_portal_width(){
    std::vector<dve::NavigationTriangle> triangles;
    add_quad(triangles,0.0F,0.0F,1.0F,1.0F);
    dve::NavigationTriangle steep;
    steep.vertices = {{{1.0F,0.0F,0.0F},{2.0F,2.0F,1.0F},{2.0F,2.0F,0.0F}}};
    steep.area = 1U; steep.flags = 0xFFFFU; steep.sourceId = 9U;
    triangles.push_back(steep);
    dve::NavigationBuildSettings settings;settings.maximumSlopeDegrees=30.0F;settings.agentRadiusMeters=0.71F;
    const auto built=dve::build_navigation_mesh(triangles,settings);
    require(built.success(),"slope build failed");
    require(built.mesh.polygons.size()==2U,"steep triangle was not removed");
    require(built.mesh.polygons[0].portals.empty()&&built.mesh.polygons[1].portals.empty(),"agent-radius erosion should close the diagonal portal");
}

void test_step_height_and_clearance(){
    std::vector<dve::NavigationTriangle> steps;
    add_quad(steps,0.0F,0.0F,1.0F,1.0F,0.0F);
    add_quad(steps,1.0F,0.0F,2.0F,1.0F,0.30F);
    dve::NavigationBuildSettings passSettings;passSettings.agentRadiusMeters=0.05F;passSettings.maximumStepHeightMeters=0.35F;
    const auto pass=dve::build_navigation_mesh(steps,passSettings);require(pass.success(),"step build failed");
    require(static_cast<bool>(dve::find_navigation_path(pass.mesh,{0.1F,0.0F,0.5F},{1.9F,0.3F,0.5F})),"walkable step was not connected");
    auto failSettings=passSettings;failSettings.maximumStepHeightMeters=0.20F;
    const auto failBuild=dve::build_navigation_mesh(steps,failSettings);require(failBuild.success(),"high-step build failed");
    const auto tooHigh=dve::find_navigation_path(failBuild.mesh,{0.1F,0.0F,0.5F},{1.9F,0.3F,0.5F});
    require(tooHigh.partial,"step above maximumStepHeightMeters should return only a partial path");

    std::vector<dve::NavigationTriangle> lowCeiling;add_quad(lowCeiling,0.0F,0.0F,1.0F,1.0F,0.0F);add_quad(lowCeiling,0.0F,0.0F,1.0F,1.0F,1.0F);
    dve::NavigationBuildSettings clearance;clearance.agentRadiusMeters=0.05F;clearance.agentHeightMeters=1.8F;
    const auto clearanceBuild=dve::build_navigation_mesh(lowCeiling,clearance);require(clearanceBuild.success(),"clearance build failed");
    require(clearanceBuild.mesh.polygons.size()==2U,"low ceiling did not remove the lower walkable layer");
}

void test_voxel_geometry_extraction(){
    dve::VoxelObject object(17U);
    for(int z=0;z<2;++z)for(int x=0;x<2;++x)object.set_voxel({x,0,z},1U);
    const auto triangles=dve::navigation_triangles_from_voxel_object(object,1.0F);
    require(triangles.size()==8U,"voxel top-face extraction count mismatch");
    dve::NavigationBuildSettings settings;settings.agentRadiusMeters=0.05F;
    const auto built=dve::build_navigation_mesh(triangles,settings);require(built.success(),"voxel navigation build failed");
    require(static_cast<bool>(dve::find_navigation_path(built.mesh,{0.1F,1.0F,0.1F},{1.9F,1.0F,1.9F})),"voxel navigation path failed");
}

void test_off_mesh_link_and_roundtrip(){
    std::vector<dve::NavigationTriangle> triangles;add_quad(triangles,0.0F,0.0F,1.0F,1.0F);add_quad(triangles,3.0F,0.0F,4.0F,1.0F);
    dve::NavigationOffMeshLink link;link.start={0.8F,0.0F,0.5F};link.end={3.2F,0.0F,0.5F};link.radiusMeters=0.6F;link.userId=77U;
    dve::NavigationBuildSettings settings;settings.agentRadiusMeters=0.05F;
    const auto built=dve::build_navigation_mesh(triangles,settings,std::span<const dve::NavigationOffMeshLink>(&link,1U));
    require(built.success(),"off-mesh build failed");
    const auto path=dve::find_navigation_path(built.mesh,{0.1F,0.0F,0.5F},{3.9F,0.0F,0.5F});
    require(static_cast<bool>(path),"off-mesh path failed");
    bool sawStart=false,sawEnd=false;for(const auto& p:path.points){sawStart|=p.kind==dve::NavigationPathPointKind::OffMeshStart&&p.userId==77U;sawEnd|=p.kind==dve::NavigationPathPointKind::OffMeshEnd&&p.userId==77U;}
    require(sawStart&&sawEnd,"off-mesh path markers missing");

    const auto pathFile=std::filesystem::temp_directory_path()/"dve_navigation_mesh_test.dnav";std::string error;
    require(dve::write_dnav(pathFile,built.mesh,&error),"navigation write failed");
    const auto loaded=dve::read_dnav(pathFile,&error);require(loaded.has_value(),"navigation read failed");
    require(loaded->contentHash==built.mesh.contentHash,"navigation roundtrip hash mismatch");
    const auto roundtripPath=dve::find_navigation_path(*loaded,{0.1F,0.0F,0.5F},{3.9F,0.0F,0.5F});
    require(static_cast<bool>(roundtripPath),"roundtrip navigation path failed");
    std::error_code ec;std::filesystem::remove(pathFile,ec);
}
}

int main(){try{test_build_and_path();test_slope_and_portal_width();test_step_height_and_clearance();test_voxel_geometry_extraction();test_off_mesh_link_and_roundtrip();std::cout<<"navigation mesh tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<"navigation mesh test failure: "<<e.what()<<"\n";return 1;}}
