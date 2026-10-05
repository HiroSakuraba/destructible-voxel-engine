#include "dve/dashr_seam_map.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
void require(bool c,const char* m){if(!c)throw std::runtime_error(m);}

CookedPolygonAsset seam_asset() {
    CookedPolygonAsset a;
    VoxelMaterialDefinition m;m.name="seam";a.materials.push_back(m);a.materialBindings.push_back({});
    // Two triangles share the geometric edge (1,0,0)-(0,1,0), but their UV
    // copies live on separate islands at x=0.4 and x=0.6.
    a.vertices={
        {{0,0,0},{0,0,1},{1,0,0,1},{0.1F,0.1F},{1,1,1,1},{0,0}},
        {{1,0,0},{0,0,1},{1,0,0,1},{0.4F,0.1F},{1,1,1,1},{0,0}},
        {{0,1,0},{0,0,1},{1,0,0,1},{0.4F,0.4F},{1,1,1,1},{0,0}},
        {{1,0,0},{0,0,1},{1,0,0,1},{0.6F,0.1F},{1,1,1,1},{0,0}},
        {{1,1,0},{0,0,1},{1,0,0,1},{0.9F,0.1F},{1,1,1,1},{0,0}},
        {{0,1,0},{0,0,1},{1,0,0,1},{0.6F,0.4F},{1,1,1,1},{0,0}},
    };
    a.indices={0,1,2,3,4,5};
    a.submeshes={{"two-islands",0,6,0}};
    a.bounds={{0,0,0},{1,1,0}};
    a.contentHash=polygon_asset_content_hash(a);
    require(static_cast<bool>(validate_polygon_asset(a)),"seam fixture invalid");
    return a;
}

void test_seam_pair_and_destination_inset() {
    DashrSeamCookSettings settings;
    settings.resolution=128U;
    settings.radiusPixels=2U;
    settings.destinationInsetPixels=4U;
    std::string error;
    const auto map=cook_dashr_seam_map(seam_asset(),settings,&error);
    require(map.has_value(),error.c_str());
    require(map->stats.seamPairs==1U,"expected exactly one duplicated geometric seam");
    require(map->stats.markedTexels>0U,"seam cooker did not mark a teleport band");

    bool sawAToB=false,sawBToA=false;
    for(const Float4 texel:map->texels) if(texel.z>0.0F) {
        if(texel.x>0.6F) sawAToB=true;
        if(texel.x<0.4F) sawBToA=true;
        require(texel.w>0.5F,"marked seam texel lacks validity");
    }
    require(sawAToB&&sawBToA,"seam teleport was not generated in both directions");
}

void test_open_boundary_and_identical_uv_are_not_seams() {
    auto a=seam_asset();
    // Collapse the second island onto the first. The duplicated geometric edge
    // now has the same UV endpoints and should no longer be a teleport seam.
    a.vertices[3].texcoord=a.vertices[1].texcoord;
    a.vertices[5].texcoord=a.vertices[2].texcoord;
    a.contentHash=polygon_asset_content_hash(a);
    DashrSeamCookSettings settings;settings.resolution=64U;
    std::string error;
    const auto map=cook_dashr_seam_map(a,settings,&error);
    require(map.has_value(),error.c_str());
    require(map->stats.seamPairs==0U,"identical UV copies were misclassified as a seam");
    require(map->stats.markedTexels==0U,"non-seam texture contains teleport pixels");
}

void test_non_manifold_group_is_skipped() {
    auto a=seam_asset();
    // Add a third triangle using another duplicate of the same geometric edge.
    const std::uint32_t base=static_cast<std::uint32_t>(a.vertices.size());
    a.vertices.push_back({{1,0,0},{0,0,1},{1,0,0,1},{0.2F,0.6F},{1,1,1,1},{0,0}});
    a.vertices.push_back({{0,1,0},{0,0,1},{1,0,0,1},{0.2F,0.9F},{1,1,1,1},{0,0}});
    a.vertices.push_back({{0,0,1},{0,0,1},{1,0,0,1},{0.1F,0.8F},{1,1,1,1},{0,0}});
    a.indices.insert(a.indices.end(),{base,base+1U,base+2U});
    a.submeshes[0].indexCount=9U;
    a.bounds.maximum.z=1.0F;
    a.contentHash=polygon_asset_content_hash(a);
    DashrSeamCookSettings settings;settings.resolution=64U;
    std::string error;
    const auto map=cook_dashr_seam_map(a,settings,&error);
    require(map.has_value(),error.c_str());
    require(map->stats.nonManifoldGroupsSkipped>=1U,
            "non-manifold coincident edge group was not skipped");
}

}

int main(){try{
    test_seam_pair_and_destination_inset();
    test_open_boundary_and_identical_uv_are_not_seams();
    test_non_manifold_group_is_skipped();
    std::cout<<"dve_dashr_seam_map_tests: PASS\n";return 0;
}catch(const std::exception&e){std::cerr<<"dve_dashr_seam_map_tests: FAIL: "<<e.what()<<'\n';return 1;}}
