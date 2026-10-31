#include "dve/spherical_harmonic_exponentials.hpp"
#include "dve/environment_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " line " + std::to_string(__LINE__)); } while(false)
dve::Float3 normal_at(int i) {
    const float y=1.0F-2.0F*(float(i)+0.5F)/768.0F;
    const float radius=std::sqrt(std::max(0.0F,1.0F-y*y));
    const float a=float(i)*2.3999632297286533F;
    return {radius*std::cos(a),y,radius*std::sin(a)};
}
float local_dot(dve::Float3 a,dve::Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
dve::Float3 cross(dve::Float3 a,dve::Float3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
dve::Float3 norm(dve::Float3 v){
    const float l=std::sqrt(local_dot(v,v));
    return {v.x/l,v.y/l,v.z/l};
}
void test_synthetic_fit_and_half_packing() {
    dve::SheProbeCoefficients source{};
    // Constant term + view-dependent directional terms in both factors.
    source.rgb[0][0]= 1.5F; source.rgb[1][0]=-0.8F; source.rgb[2][0]=0.3F;
    source.rgb[0][3]= 0.18F; source.rgb[1][4]=0.12F; source.rgb[2][7]=-0.1F;
    source.rgb[0][25]=0.08F; source.rgb[1][26]=-0.16F; source.rgb[2][29]=0.14F;
    CHECK(source.validate());
    std::vector<dve::SheFitSample> samples;
    samples.reserve(768);
    for(int i=0;i<768;++i){
        auto n=normal_at(i);
        auto axis=std::abs(n.y)<0.9F?dve::Float3{0,1,0}:dve::Float3{1,0,0};
        auto t=norm(cross(axis,n)),b=cross(n,t);
        const float ndv=0.15F+0.85F*std::fmod(float(i)*0.75487766625F,1.0F);
        const float az=6.283185307179586F*std::fmod(float(i)*0.56984029099F,1.0F);
        const float s=std::sqrt(1.0F-ndv*ndv);
        dve::Float3 v=norm({n.x*ndv+s*(t.x*std::cos(az)+b.x*std::sin(az)),
                            n.y*ndv+s*(t.y*std::cos(az)+b.y*std::sin(az)),
                            n.z*ndv+s*(t.z*std::cos(az)+b.z*std::sin(az))});
        const float alpha=0.25F+0.75F*std::fmod(float(i)*0.61803398875F,1.0F);
        auto value=dve::evaluate_she_base_reflectance(source,n,v,alpha);
        CHECK(value);
        samples.push_back({n,v,alpha,*value});
    }
    auto fitted=dve::fit_she_probe(samples,1.0e-6F);
    CHECK(fitted);
    CHECK(fitted->fittedSamples==samples.size());
    CHECK(fitted->logRmse<0.025F);
    const auto packed=dve::pack_she_probe_half(fitted->coefficients);
    const auto unpacked=dve::unpack_she_probe_half(packed);
    CHECK(unpacked.validate());
    for(std::size_t i=99;i<dve::kShePackedHalfCount;++i)CHECK(packed.half[i]==0U);
    for(int i=0;i<30;++i){
        const auto& sample=samples[static_cast<std::size_t>(i)*17U];
        const auto out=dve::evaluate_she_base_reflectance(
            unpacked,sample.normal,sample.viewDirection,sample.alpha);
        CHECK(out);
        CHECK(std::abs(out->x-sample.baseReflectance.x)<0.08F);
        CHECK(std::abs(out->y-sample.baseReflectance.y)<0.08F);
        CHECK(std::abs(out->z-sample.baseReflectance.z)<0.08F);
    }
    CHECK(!dve::evaluate_she_base_reflectance(
        unpacked,{0,1,0},{0,1,0},0.24F));
    CHECK(!dve::evaluate_she_base_reflectance(
        unpacked,{0,0,0},{0,1,0},0.5F));
    auto invalid=unpacked;
    invalid.rgb[0][0]=std::numeric_limits<float>::infinity();
    CHECK(!invalid.validate());
}

void test_constant_environment_bake() {
    dve::EnvironmentCube environment;
    environment.resolution=4U;
    environment.texels.assign(6U*4U*4U,{1.0F,0.5F,0.25F});
    CHECK(environment.validate());
    // Independent direct GGX oracle: finite, bounded, and not selected by
    // a SHE coefficient toggle or the split-sum BRDF prefilter.
    const auto highSample=dve::reference_ggx_environment_integral(
        environment,{0,1,0},{0,1,0},0.64F,2048U);
    CHECK(highSample && std::isfinite(highSample->x));
    CHECK(highSample->x>0.0F && highSample->y>0.0F);
    CHECK(!dve::reference_ggx_environment_integral(
        environment,{0,0,0},{0,1,0},0.64F,64U));
    CHECK(!dve::reference_ggx_environment_integral(
        environment,{0,1,0},{0,1,0},0.64F,8U));
    dve::SheBakeSettings settings;
    settings.trainingSamples=256U;
    settings.ggxSamplesPerTrainingPoint=24U;
    const auto fitted=dve::bake_she_probe(environment,settings);
    CHECK(fitted);
    CHECK(fitted->coefficients.validate());
    CHECK(fitted->logRmse>=0.0F&&std::isfinite(fitted->logRmse));
    const auto base=dve::evaluate_she_base_reflectance(
        fitted->coefficients,{0,1,0},{0,1,0},0.5F);
    CHECK(base&&base->x>0.0F&&base->y>0.0F&&base->z>0.0F);
    CHECK(std::abs(base->x/base->y-2.0F)<0.15F);
    CHECK(std::abs(base->y/base->z-2.0F)<0.15F);

    const auto ibl=dve::bake_image_based_lighting(
        environment,dve::IblBakeSettings{4U,8U,4U,16U,8U});
    dve::IblMaterialInput material;
    material.normal={0,1,0};
    material.viewDirection={0,1,0};
    material.roughness=0.8F;
    material.metallic=1.0F;
    const auto baseline=dve::evaluate_image_based_lighting(ibl,material);
    material.sheProbe=&fitted->coefficients;
    const auto withShe=dve::evaluate_image_based_lighting(ibl,material);
    CHECK(std::isfinite(withShe.x)&&std::isfinite(withShe.y)&&std::isfinite(withShe.z));
    CHECK(withShe.x>0.0F);
    // Prove that the CPU evaluator actually selects SHE, rather than silently
    // always returning the split-sum result. DC Y00 ~= 0.282.
    dve::SheProbeCoefficients strong{};
    strong.rgb[0][0]=20.0F;
    material.sheProbe=&strong;
    const auto forced=dve::evaluate_image_based_lighting(ibl,material);
    CHECK(forced.x>baseline.x+10.0F);
    // Sharp surfaces are guaranteed to keep their previous split-sum values.
    material.roughness=0.2F;
    const auto fallback=dve::evaluate_image_based_lighting(ibl,material);
    material.sheProbe=nullptr;
    const auto split=dve::evaluate_image_based_lighting(ibl,material);
    CHECK(fallback.x==split.x&&fallback.y==split.y&&fallback.z==split.z);
}
}
int main(){
    try{
        test_synthetic_fit_and_half_packing();
        test_constant_environment_bake();
        std::cout<<"dve_spherical_harmonic_exponentials_tests: PASS\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"dve_spherical_harmonic_exponentials_tests: FAIL: "<<e.what()<<'\n';
        return 1;
    }
}
