#include "dve/material_displacement.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dve;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b, float tolerance) {
    return std::abs(a-b)<=tolerance;
}

CookedPolygonAsset make_asset() {
    CookedPolygonAsset asset;
    asset.objectId=9U;
    VoxelMaterialDefinition material;
    material.name="Noise";
    asset.materials.push_back(material);
    asset.materialBindings.emplace_back();
    asset.vertices={
        {{0,0,0},{0,0,1},{1,0,0,1},{0,0},{1,1,1,1},{0,0}},
        {{1,0,0},{0,0,1},{1,0,0,1},{1,0},{1,1,1,1},{0,0}},
        {{0,1,0},{0,0,1},{1,0,0,1},{0,1},{1,1,1,1},{0,0}}};
    asset.indices={0U,1U,2U};
    asset.submeshes.push_back({"triangle",0U,3U,0U});
    asset.bounds={{0,0,0},{1,1,0}};
    asset.contentHash=polygon_asset_content_hash(asset);
    require(static_cast<bool>(validate_polygon_asset(asset)),"noise displacement fixture is invalid");
    return asset;
}

void test_analytic_gradient() {
    constexpr Float3 point{0.237F,-1.419F,2.736F};
    constexpr float step=0.001F;
    const NoiseSample3 sample=value_noise_3d(point,42U);
    const NoiseSample3 repeated=value_noise_3d(point,42U);
    const NoiseSample3 differentSeed=value_noise_3d(point,43U);
    require(sample.value==repeated.value&&sample.gradient.x==repeated.gradient.x&&
            sample.gradient.y==repeated.gradient.y&&sample.gradient.z==repeated.gradient.z,
            "noise output is not deterministic for a fixed seed");
    require(sample.value!=differentSeed.value,"changing the noise seed did not change the field");
    require(sample.value>=-1.0F&&sample.value<=1.0F,"value noise exceeded its normalized range");
    const float dx=(value_noise_3d({point.x+step,point.y,point.z},42U).value-
                    value_noise_3d({point.x-step,point.y,point.z},42U).value)/(2.0F*step);
    const float dy=(value_noise_3d({point.x,point.y+step,point.z},42U).value-
                    value_noise_3d({point.x,point.y-step,point.z},42U).value)/(2.0F*step);
    const float dz=(value_noise_3d({point.x,point.y,point.z+step},42U).value-
                    value_noise_3d({point.x,point.y,point.z-step},42U).value)/(2.0F*step);
    require(close(sample.gradient.x,dx,2.0e-3F),"analytic x derivative disagrees with finite difference");
    require(close(sample.gradient.y,dy,2.0e-3F),"analytic y derivative disagrees with finite difference");
    require(close(sample.gradient.z,dz,2.0e-3F),"analytic z derivative disagrees with finite difference");

    const NoiseSample3 left=value_noise_3d({1.0F-1.0e-3F,0.37F,-0.61F},8U);
    const NoiseSample3 right=value_noise_3d({1.0F+1.0e-3F,0.37F,-0.61F},8U);
    require(std::abs(left.value-right.value)<2.0e-5F,"value noise has a crease at a lattice boundary");
    require(std::abs(left.gradient.x-right.gradient.x)<2.0e-4F,
            "value noise derivative is discontinuous at a lattice boundary");
}

void test_fractal_chain_rule() {
    FractalNoiseSettings settings;
    settings.frequency=1.7F;
    settings.octaves=4U;
    settings.lacunarity=2.0F;
    settings.persistence=0.45F;
    settings.seed=11U;
    require(validate_fractal_noise_settings(settings),"valid fractal-noise settings were rejected");
    constexpr Float3 point{0.23F,0.41F,-0.77F};
    constexpr float step=0.0005F;
    const NoiseSample3 sample=fractal_noise_3d(point,settings);
    const float dx=(fractal_noise_3d({point.x+step,point.y,point.z},settings).value-
                    fractal_noise_3d({point.x-step,point.y,point.z},settings).value)/(2.0F*step);
    const float dy=(fractal_noise_3d({point.x,point.y+step,point.z},settings).value-
                    fractal_noise_3d({point.x,point.y-step,point.z},settings).value)/(2.0F*step);
    const float dz=(fractal_noise_3d({point.x,point.y,point.z+step},settings).value-
                    fractal_noise_3d({point.x,point.y,point.z-step},settings).value)/(2.0F*step);
    require(close(sample.gradient.x,dx,1.5e-2F),"fractal x gradient omitted octave frequency chain rule");
    require(close(sample.gradient.y,dy,1.5e-2F),"fractal y gradient omitted octave frequency chain rule");
    require(close(sample.gradient.z,dz,1.5e-2F),"fractal z gradient omitted octave frequency chain rule");
    settings.octaves=0U;
    require(!validate_fractal_noise_settings(settings),"zero-octave noise settings were accepted");
    settings.octaves=12U;
    settings.frequency=100.0F;
    settings.lacunarity=4.0F;
    require(!validate_fractal_noise_settings(settings),"unbounded highest-octave frequency was accepted");
}

void test_noise_displacement_and_bump_normal() {
    CookedPolygonAsset source=make_asset();
    MaterialVertexDisplacementSettings setting;
    setting.policy=MaterialDisplacementPolicy::VisualOnly;
    setting.proceduralNoiseEnabled=true;
    setting.proceduralNoiseAmplitudeMeters=0.2F;
    setting.proceduralNoise.frequency=1.7F;
    setting.proceduralNoise.octaves=3U;
    setting.proceduralNoise.seed=123U;
    setting.offlineSubdivisionLevels=1U;
    std::string error;
    const auto result=build_displaced_polygon_asset(
        source,std::span<const MaterialVertexDisplacementSettings>(&setting,1U),
        0.0F,100U,&error);
    require(result.has_value(),error.c_str());
    require(result->visualAsset.vertices.size()>source.vertices.size(),"procedural displacement skipped subdivision");
    require(result->stats.displacedVisualVertices>0U,"procedural displacement did not affect vertices");
    bool moved=false,normalBent=false;
    for (std::size_t i=0;i<result->visualAsset.vertices.size();++i) {
        const PolygonVertex& vertex=result->visualAsset.vertices[i];
        if (i>=source.vertices.size()&&std::abs(vertex.position.z)>1.0e-5F) moved=true;
        if (std::abs(vertex.normal.x)>1.0e-4F||std::abs(vertex.normal.y)>1.0e-4F) normalBent=true;
        const float tangentDot=vertex.normal.x*vertex.tangent.x+
            vertex.normal.y*vertex.tangent.y+vertex.normal.z*vertex.tangent.z;
        require(std::abs(tangentDot)<1.0e-3F,"noise-updated tangent is not orthogonal to the normal");
    }
    require(moved,"procedural noise failed to displace the subdivided surface");
    require(normalBent,"analytic noise gradient did not perturb the surface normal");
}

} // namespace

int main() {
    try {
        test_analytic_gradient();
        test_fractal_chain_rule();
        test_noise_displacement_and_bump_normal();
        std::cout<<"procedural noise tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr<<"procedural noise tests: FAIL: "<<exception.what()<<'\n';
        return 1;
    }
}
