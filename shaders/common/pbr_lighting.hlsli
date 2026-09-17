#ifndef DVE_PBR_LIGHTING_HLSLI
#define DVE_PBR_LIGHTING_HLSLI

#include "material_record.hlsli"

// Standard Cook-Torrance microfacet BRDF: GGX/Trowbridge-Reitz normal distribution,
// height-correlated Smith visibility (the "V" form that already includes the 1/(4*NdotL*NdotV)
// denominator, avoiding a separate divide-by-zero-prone term), and Fresnel-Schlick. This is
// the same formulation used in most modern real-time PBR renderers (Unreal, Frostbite,
// Filament); nothing here is voxel-specific, which is the point: exactly one shading
// evaluation, parameterized per-material, not a shader graph per material (see
// master_material.hpp for why that split makes sense for this engine).

static const float kPi = 3.14159265358979323846F;

float DistributionGGX(float NdotH, float roughness) {
    float a = max(roughness * roughness, 1.0e-4F);
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0F) + 1.0F;
    denom = kPi * denom * denom;
    return a2 / max(denom, 1.0e-9F);
}

// Height-correlated Smith visibility term (V = G / (4 * NdotL * NdotV)), Filament's derivation.
float VisibilitySmithGGXCorrelated(float NdotV, float NdotL, float roughness) {
    float a2 = max(roughness * roughness, 1.0e-4F);
    a2 = a2 * a2;
    float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0F - a2) + a2);
    float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0F - a2) + a2);
    return 0.5F / max(lambdaV + lambdaL, 1.0e-9F);
}

float3 FresnelSchlick(float cosTheta, float3 f0) {
    float m = saturate(1.0F - cosTheta);
    float m2 = m * m;
    return f0 + (1.0F.xxx - f0) * (m2 * m2 * m);
}

// Direct lighting from a single light with the given (already normalized) direction *to* the
// light and radiance. lightRadiance folds in intensity and any distance/shadow attenuation the
// caller has already computed; this function is purely the BRDF evaluation and NdotL term.
float3 ShadeDirectLight(
    float3 normal, float3 viewDirection, float3 lightDirection, float3 lightRadiance,
    float3 baseColor, float metallic, float roughness, float specular) {
    float3 halfVector = normalize(viewDirection + lightDirection);
    float NdotL = saturate(dot(normal, lightDirection));
    float NdotV = saturate(dot(normal, viewDirection)) + 1.0e-4F; // avoid a hard zero at grazing angles
    float NdotH = saturate(dot(normal, halfVector));
    float VdotH = saturate(dot(viewDirection, halfVector));
    if (NdotL <= 0.0F) return float3(0.0F, 0.0F, 0.0F);

    // Dielectric F0 comes from the material's Specular parameter (0.5 default -> ~4%
    // reflectance, matching Unreal's own convention); metals reflect their base color instead,
    // the standard "metallic workflow" split.
    float3 dielectricF0 = (0.08F * specular).xxx;
    float3 f0 = lerp(dielectricF0, baseColor, metallic);
    float3 diffuseColor = baseColor * (1.0F - metallic);

    float D = DistributionGGX(NdotH, roughness);
    float V = VisibilitySmithGGXCorrelated(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, f0);

    float3 specularTerm = D * V * F; // V already folds in the 4*NdotL*NdotV denominator
    float3 diffuseTerm = diffuseColor * (1.0F / kPi) * (1.0F.xxx - F);
    return (diffuseTerm + specularTerm) * lightRadiance * NdotL;
}


// A compact two-lobe clear-coat model. The coat is a dielectric GGX lobe with fixed F0=0.04;
// its Fresnel term attenuates the base lobe before the coat contribution is added, avoiding the
// obvious double-energy error of simply summing two full BRDFs.
float3 ShadeClearCoatDirect(
    float3 normal, float3 viewDirection, float3 lightDirection, float3 lightRadiance,
    float3 baseColor, float metallic, float roughness, float specular,
    float clearCoat, float clearCoatRoughness) {
    float coatAmount = saturate(clearCoat);
    float3 base = ShadeDirectLight(normal, viewDirection, lightDirection, lightRadiance,
                                  baseColor, metallic, roughness, specular);
    if (coatAmount <= 0.0F) return base;

    float3 halfVector = normalize(viewDirection + lightDirection);
    float NdotL = saturate(dot(normal, lightDirection));
    float NdotV = saturate(dot(normal, viewDirection)) + 1.0e-4F;
    float NdotH = saturate(dot(normal, halfVector));
    float VdotH = saturate(dot(viewDirection, halfVector));
    if (NdotL <= 0.0F) return base;

    float coatRoughness = clamp(clearCoatRoughness, 0.02F, 1.0F);
    float D = DistributionGGX(NdotH, coatRoughness);
    float V = VisibilitySmithGGXCorrelated(NdotV, NdotL, coatRoughness);
    float3 F = FresnelSchlick(VdotH, 0.04F.xxx);
    float3 coat = D * V * F * lightRadiance * NdotL;
    float3 baseTransmission = 1.0F.xxx - coatAmount * F;
    // The base lobe crosses the coat interface on the way in and again on the way out.
    return base * baseTransmission * baseTransmission + coat * coatAmount;
}

// Thin two-sided foliage: the view-facing side receives the ordinary PBR lobe, while light
// arriving from the geometric back side contributes colored transmission. `wrap` softens the
// terminator without pretending to be a full leaf-volume transport solve.
float3 ShadeTwoSidedFoliageDirect(
    float3 geometricNormal, float3 viewDirection, float3 lightDirection, float3 lightRadiance,
    float3 baseColor, float metallic, float roughness, float specular,
    float3 foliageColor, float transmittance, float wrap) {
    float facing = dot(geometricNormal, viewDirection) >= 0.0F ? 1.0F : -1.0F;
    float3 shadingNormal = geometricNormal * facing;
    float3 front = ShadeDirectLight(shadingNormal, viewDirection, lightDirection, lightRadiance,
                                   baseColor, metallic, roughness, specular);
    float wrappedBack = saturate((dot(-shadingNormal, lightDirection) + saturate(wrap)) /
                                 (1.0F + saturate(wrap)));
    float3 transmitted = max(foliageColor, 0.0F.xxx) * (1.0F / kPi) * lightRadiance *
                         wrappedBack * saturate(transmittance);
    return front + transmitted;
}

// Cheap, IBL-free ambient term: a two-color hemisphere lerp by the normal's up-component
// (sky/ground), the same trick used by countless real-time renderers when a full irradiance
// probe isn't available (it isn't here: no baked lighting pipeline exists). `ao` (0=fully
// occluded, 1=fully open) modulates it, sourced from the ambient-occlusion pass.
float3 ShadeAmbient(float3 normal, float3 skyColor, float3 groundColor, float ao) {
    float upness = 0.5F + 0.5F * normal.y;
    return lerp(groundColor, skyColor, upness) * ao;
}

#endif // DVE_PBR_LIGHTING_HLSLI
