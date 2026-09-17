#ifndef DVE_MATERIAL_RECORD_HLSLI
#define DVE_MATERIAL_RECORD_HLSLI

struct MaterialRecord {
    float baseColorR;
    float baseColorG;
    float baseColorB;
    float baseColorA;
    float emissiveR;
    float emissiveG;
    float emissiveB;
    float metallic;
    float roughness;
    float specular;
    uint shadingModel;
    uint blendMode;
    float subsurfaceScatterDistanceMeters;
    float subsurfaceColorR;
    float subsurfaceColorG;
    float subsurfaceColorB;
    float clearCoat;
    float clearCoatRoughness;
    float foliageColorR;
    float foliageColorG;
    float foliageColorB;
    float foliageTransmittance;
    float foliageWrap;
};

static const uint kShadingModelStandardPBR = 0u;
static const uint kShadingModelUnlit = 1u;
static const uint kShadingModelEmissive = 2u;
static const uint kShadingModelSubsurface = 3u;
static const uint kShadingModelTwoSidedFoliage = 4u;
static const uint kShadingModelClearCoat = 5u;

static const uint kBlendModeOpaque = 0u;
static const uint kBlendModeMasked = 1u;
static const uint kBlendModeTranslucent = 2u;

float3 MaterialBaseColorRGB(MaterialRecord material) {
    return float3(material.baseColorR, material.baseColorG, material.baseColorB);
}
float3 MaterialEmissiveRGB(MaterialRecord material) {
    return float3(material.emissiveR, material.emissiveG, material.emissiveB);
}
float3 MaterialSubsurfaceColorRGB(MaterialRecord material) {
    return float3(material.subsurfaceColorR, material.subsurfaceColorG, material.subsurfaceColorB);
}
float3 MaterialFoliageColorRGB(MaterialRecord material) {
    return float3(material.foliageColorR, material.foliageColorG, material.foliageColorB);
}

#endif // DVE_MATERIAL_RECORD_HLSLI
