#include "dve/dashr_surface.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool close(float a, float b, float eps = 1.0e-4F) {
    return std::abs(a-b) <= eps;
}

DashrSurfaceSample plane_sample(Float2 uv, float objectUOffset = 0.0F) {
    auto sample = make_dashr_surface_sample(
        uv, {uv.x + objectUOffset, uv.y, 0.0F},
        {1.0F,0.0F,0.0F}, {0.0F,1.0F,0.0F}, {0.0F,0.0F,1.0F});
    require(sample.has_value(), "plane sample construction failed");
    return *sample;
}

void test_frame_and_distortion() {
    auto sample = make_dashr_surface_sample(
        {0.25F,0.75F}, {2.0F,3.0F,4.0F},
        {2.0F,0.0F,0.0F}, {0.0F,4.0F,0.0F}, {0.0F,0.0F,0.5F});
    require(sample.has_value(), "basis inversion failed");
    DashrSurfaceSettings settings;
    float stepFactor{};
    const auto surface = dashr_object_to_surface(
        *sample, {2.2F,2.6F,4.1F}, settings, &stepFactor);
    require(close(surface.x,0.35F), "surface U mapping wrong");
    require(close(surface.y,0.65F), "surface V mapping wrong");
    require(close(surface.z,0.7F), "surface height mapping wrong");
    require(close(stepFactor,1.0F), "identity distortion shortened step");

    const float h=0.01F;
    auto center=plane_sample({0.5F,0.5F});
    auto pu=plane_sample({0.5F+h,0.5F});
    auto nu=plane_sample({0.5F-h,0.5F});
    auto pv=plane_sample({0.5F,0.5F+h});
    auto nv=plane_sample({0.5F,0.5F-h});
    const auto ratio=measure_dashr_distortion(center,pu,nu,pv,nv,h);
    require(close(ratio.x,1.0F,1.0e-3F) && close(ratio.y,1.0F,1.0e-3F),
            "identity distortion should be one");
}

void test_distortion_damping() {
    auto sample=plane_sample({0.5F,0.5F});
    sample.distortionRatio={2.0F,-0.1F};
    DashrSurfaceSettings settings;
    settings.stretchThreshold=1.5F;
    settings.compressionThreshold=0.05F;
    settings.minimumStepFactor=0.01F;
    float factor{};
    const auto mapped=dashr_object_to_surface(sample,{0.7F,0.7F,0.0F},settings,&factor);
    require(close(mapped.x,0.6F,1.0e-4F), "stretch damping did not reduce U motion");
    require(close(mapped.y,0.7F,1.0e-4F), "compression damping should preserve local coordinate");
    require(close(factor,0.01F,1.0e-5F), "compressed region did not request minimum step");
}

void test_flat_trace_hit_and_escape() {
    DashrSurfaceSettings settings;
    settings.heightScale=0.2F;
    settings.heightReferencePlane=0.5F;
    settings.envelopePadding=0.25F;
    settings.stepSize=0.01F;
    settings.stepScale=8.0F;
    settings.maximumSteps=128U;
    settings.refinementSteps=6U;

    const DashrSurfaceSampleFunction field=[](Float2 uv)->std::optional<DashrSurfaceSample>{
        return plane_sample(uv);
    };
    const DashrHeightSampleFunction flat=[](Float2){return 0.5F;};

    const auto hit=trace_dashr_heightfield(
        {0.5F,0.5F,0.12F},{0.0F,0.0F,-1.0F},{0.5F,0.5F},
        settings,field,flat);
    require(hit.hit(), "flat surface trace did not hit");
    require(hit.objectDistance>0.10F && hit.objectDistance<0.14F,
            "flat surface hit distance wrong");
    require(std::abs(hit.objectPosition.z)<0.005F, "refined hit is not near plane");
    require(hit.refinementSteps>0U, "hit did not refine");

    const auto escaped=trace_dashr_heightfield(
        {0.95F,0.5F,0.12F},{1.0F,0.0F,0.0F},{0.95F,0.5F},
        settings,field,flat);
    require(escaped.status==DashrTraceStatus::Escaped,
            "sideways trace should escape UV shell");
}

void test_seam_teleport() {
    DashrSurfaceSettings settings;
    settings.heightScale=0.2F;
    settings.envelopePadding=0.25F;
    settings.stepSize=0.01F;
    settings.maximumSteps=128U;

    const DashrSurfaceSampleFunction field=[](Float2 uv)->std::optional<DashrSurfaceSample>{
        return plane_sample(uv, uv.x < 0.5F ? 0.5F : 0.0F);
    };
    const DashrHeightSampleFunction flat=[](Float2){return 0.5F;};
    const DashrTeleportSampleFunction teleport=[](Float2 uv)
        ->std::optional<DashrTeleportSample> {
        if (uv.x > 0.75F) return DashrTeleportSample{{uv.x-0.5F,uv.y},1.0F,true};
        return DashrTeleportSample{uv,-1.0F,true};
    };

    const auto result=trace_dashr_heightfield(
        {0.8F,0.5F,0.10F},{0.0F,0.0F,-1.0F},{0.8F,0.5F},
        settings,field,flat,teleport);
    require(result.hit(), "teleported trace did not hit");
    require(result.teleports==1U, "seam teleport count wrong");
    require(result.surfacePosition.x<0.5F, "trace did not remain on destination UV island");
}

void test_validation_and_degenerate_basis() {
    DashrSurfaceSettings settings;
    std::string error;
    require(validate_dashr_surface_settings(settings,&error), "default settings invalid");
    settings.maximumSteps=0U;
    require(!validate_dashr_surface_settings(settings,&error), "zero step budget accepted");

    const auto degenerate=make_dashr_surface_sample(
        {0.5F,0.5F},{},{1,0,0},{2,0,0},{0,0,1});
    require(!degenerate.has_value(), "singular tangent frame accepted");
}

} // namespace

int main() {
    try {
        test_frame_and_distortion();
        test_distortion_damping();
        test_flat_trace_hit_and_escape();
        test_seam_teleport();
        test_validation_and_degenerate_basis();
        std::cout << "dve_dashr_surface_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_dashr_surface_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
