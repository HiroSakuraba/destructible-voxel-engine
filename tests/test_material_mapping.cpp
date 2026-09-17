#include "dve/material_mapping.hpp"
#include "dve/gpu_material_mapping.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string("CHECK failed: ") + #condition + " at " + __FILE__ + ":" + std::to_string(__LINE__)); } while (false)

bool close(float a, float b, float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

void test_validation_and_uv_transform() {
    dve::MaterialMappingSettings settings;
    std::string error;
    CHECK(dve::validate_material_mapping_settings(settings, &error));
    settings.baseTransform.scale = {2.0F, 3.0F};
    settings.baseTransform.offset = {0.25F, -0.5F};
    settings.baseTransform.rotationRadians = 1.57079632679F;
    const dve::Float2 transformed = dve::transform_texture_coordinates({1.0F, 0.0F}, settings.baseTransform);
    CHECK(close(transformed.x, 0.25F));
    CHECK(close(transformed.y, 1.5F));

    settings.heightMode = dve::HeightMappingMode::ParallaxOcclusion;
    settings.mappingMode = dve::MaterialMappingMode::WorldTriplanar;
    CHECK(!dve::validate_material_mapping_settings(settings, &error));
    CHECK(error.find("UV0 or UV1") != std::string::npos);
}

void test_triplanar_weights() {
    const auto x = dve::make_triplanar_coordinates({1.0F, 2.0F, 3.0F}, {1.0F, 0.0F, 0.0F}, 2.0F, 4.0F);
    CHECK(x.weights.x > 0.999F);
    CHECK(close(x.xProjection.x, 6.0F));
    CHECK(close(x.xProjection.y, 4.0F));

    const auto diagonal = dve::make_triplanar_coordinates({}, {1.0F, 1.0F, 1.0F}, 1.0F, 1.0F);
    CHECK(close(diagonal.weights.x + diagonal.weights.y + diagonal.weights.z, 1.0F));
    CHECK(close(diagonal.weights.x, 1.0F / 3.0F, 1.0e-3F));
}

void test_parallax_modes() {
    dve::MaterialMappingSettings settings;
    settings.heightScale = 0.08F;
    settings.maximumParallaxDistanceMeters = 50.0F;
    const dve::HeightSampleFunction ramp = [](dve::Float2 uv) {
        return std::clamp(0.5F + 0.25F * uv.x, 0.0F, 1.0F);
    };

    settings.heightMode = dve::HeightMappingMode::OffsetParallax;
    auto offset = dve::apply_parallax_mapping({0.5F, 0.5F}, {0.4F, 0.0F, 1.0F}, 1.0F, settings, ramp);
    CHECK(offset.samples == 1U);
    CHECK(offset.uv.x < 0.5F);

    settings.heightMode = dve::HeightMappingMode::SteepParallax;
    auto steep = dve::apply_parallax_mapping({0.5F, 0.5F}, {0.4F, 0.0F, 1.0F}, 1.0F, settings, ramp);
    CHECK(steep.samples >= settings.minimumHeightSteps);
    CHECK(steep.uv.x < 0.5F);

    settings.heightMode = dve::HeightMappingMode::ParallaxOcclusion;
    settings.refinementSteps = 5U;
    auto pom = dve::apply_parallax_mapping({0.5F, 0.5F}, {0.4F, 0.0F, 1.0F}, 1.0F, settings, ramp);
    CHECK(pom.samples >= steep.samples);
    CHECK(std::isfinite(pom.uv.x) && std::isfinite(pom.uv.y));
}


void test_gpu_mapping_record() {
    dve::PolygonMaterialBinding material;
    material.mapping.mappingMode = dve::MaterialMappingMode::ObjectTriplanar;
    material.mapping.triplanarScale = 2.5F;
    material.mapping.detailColorStrength = 0.75F;
    material.baseColor.texture = 3U;
    material.normal.texture = 7U;
    material.detailNormal.texture = 11U;
    material.detailNormal.texcoord = 1U;
    material.normalScale = 0.8F;
    material.detailNormalScale = 0.6F;
    material.doubleSided = true;
    material.alphaCutoff = 0.37F;
    material.baseColor.colorSpace = dve::PolygonTextureColorSpace::Srgb;
    material.emissive.colorSpace = dve::PolygonTextureColorSpace::Linear;
    material.detailBaseColor.colorSpace = dve::PolygonTextureColorSpace::Srgb;
    const auto gpu = dve::pack_gpu_polygon_material_mapping(material);
    CHECK(gpu.mappingMode == 3U);
    CHECK(gpu.baseColorTexture == 3U);
    CHECK(gpu.normalTexture == 7U);
    CHECK(gpu.detailNormalTexture == 11U);
    CHECK(gpu.heightTexture == dve::kNoGpuTextureIndex);
    CHECK(gpu.detailTexcoord == 1U);
    CHECK(close(gpu.triplanarScale, 2.5F));
    CHECK(close(gpu.detailColorStrength, 0.75F));
    CHECK(close(gpu.normalScale, 0.8F));
    CHECK(close(gpu.detailNormalScale, 0.6F));
    CHECK(close(gpu.alphaCutoff, 0.37F));
    CHECK((gpu.mappingFlags & dve::kGpuMaterialMappingDoubleSided) != 0U);
    CHECK((gpu.mappingFlags & dve::kGpuMaterialMappingBaseColorSrgb) != 0U);
    CHECK((gpu.mappingFlags & dve::kGpuMaterialMappingEmissiveSrgb) == 0U);
    CHECK((gpu.mappingFlags & dve::kGpuMaterialMappingDetailBaseColorSrgb) != 0U);
}

void test_detail_blending() {
    dve::MaterialMappingSettings settings;
    settings.detailFadeStartMeters = 2.0F;
    settings.detailFadeEndMeters = 10.0F;
    CHECK(close(dve::material_detail_fade(1.0F, settings), 1.0F));
    CHECK(close(dve::material_detail_fade(12.0F, settings), 0.0F));
    CHECK(dve::material_detail_fade(6.0F, settings) > 0.0F);

    const auto color = dve::blend_detail_color({0.5F, 0.5F, 0.5F, 1.0F}, {1.0F, 0.5F, 0.0F, 1.0F}, 1.0F);
    CHECK(color.x > 0.5F && close(color.y, 0.5F) && color.z < 0.5F);
    CHECK(dve::blend_detail_roughness(0.5F, 1.0F, 1.0F) > 0.9F);
    const auto normal = dve::blend_detail_normal({0.0F, 0.0F, 1.0F}, {0.4F, 0.0F, 0.9165F}, 1.0F);
    CHECK(normal.x > 0.2F && normal.z > 0.8F);
}
}

int main() {
    try {
        test_validation_and_uv_transform();
        test_triplanar_weights();
        test_parallax_modes();
        test_detail_blending();
        test_gpu_mapping_record();
        std::cout << "dve_material_mapping_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_material_mapping_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
