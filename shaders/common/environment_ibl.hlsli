#ifndef DVE_ENVIRONMENT_IBL_HLSLI
#define DVE_ENVIRONMENT_IBL_HLSLI

#include "pbr_lighting.hlsli"

float3 RotateEnvironmentDirectionY(float3 direction, float rotationRadians) {
    float sineValue = sin(rotationRadians);
    float cosineValue = cos(rotationRadians);
    return float3(cosineValue * direction.x + sineValue * direction.z,
                  direction.y,
                 -sineValue * direction.x + cosineValue * direction.z);
}

// Single-source diffuse ownership shared by split-sum and SHE.
// Traced diffuse is incoming radiance (rather than irradiance) and replaces
// environment diffuse only where its validity alpha is nonzero.
float3 ResolveDiffuseIndirectIrradiance(float3 environmentIrradiance,
                                        float4 tracedRadiance) {
    return lerp(environmentIrradiance, max(tracedRadiance.rgb,0.0F.xxx)*kPi,
                saturate(tracedRadiance.a));
}

// Traced GI already includes occlusion; apply AO only to environment fallback.
float DiffuseIndirectOcclusion(float ambientOcclusion, float traceValidity) {
    return lerp(saturate(ambientOcclusion),1.0F,saturate(traceValidity));
}

float3 EvaluateImageBasedLightingWithIndirect(
    TextureCube<float4> diffuseIrradiance,
    TextureCube<float4> specularPrefilter,
    Texture2D<float2> brdfLut,
    SamplerState diffuseSampler, SamplerState specularSampler, SamplerState brdfSampler,
    float3 normal,
    float3 viewDirection,
    float3 baseColor,
    float metallic,
    float roughness,
    float specular,
    float ambientOcclusion,
    float maximumSpecularLod,
    float environmentRotationRadians,
    float4 indirectDiffuse) {
    float3 N = normalize(normal);
    float3 V = normalize(viewDirection);
    float NdotV = saturate(dot(N, V));
    float3 R = reflect(-V, N);
    float3 dielectricF0 = (0.08F * saturate(specular)).xxx;
    float3 F0 = lerp(dielectricF0, max(baseColor, 0.0F.xxx), saturate(metallic));
    float3 irradiance = diffuseIrradiance.SampleLevel(diffuseSampler,
        RotateEnvironmentDirectionY(N, environmentRotationRadians), 0.0F).rgb;
    // Traced indirect light (.rgb radiance, .a 1 where valid) replaces the environment's diffuse
    // integral; it already sees occluders, so diffuse AO is not applied on top of it.
    irradiance = ResolveDiffuseIndirectIrradiance(irradiance, indirectDiffuse);
    float3 diffuse = irradiance * max(baseColor, 0.0F.xxx) * (1.0F - saturate(metallic)) / kPi;
    float3 prefiltered = specularPrefilter.SampleLevel(specularSampler,
        RotateEnvironmentDirectionY(R, environmentRotationRadians),
        saturate(roughness) * maximumSpecularLod).rgb;
    float2 brdf = brdfLut.SampleLevel(brdfSampler,
        float2(NdotV, saturate(roughness)), 0.0F);
    // Directional albedo of the specular lobe (with FresnelSchlick's grazing fade below F0 0.02).
    float3 specularAlbedo = F0 * brdf.x + brdf.y * saturate(50.0F * F0.g);
    float3 specularLighting = prefiltered * specularAlbedo;
    // Energy conservation: light the specular lobe reflects is not also scattered diffusely.
    diffuse *= saturate(1.0F.xxx - specularAlbedo);
    float ao = saturate(ambientOcclusion);
    // Diffuse AO is not a valid specular visibility term. Lagarde's inexpensive approximation
    // preserves narrow reflections while still occluding rough reflections in cavities.
    float specularOcclusion = saturate(pow(NdotV + ao, exp2(-16.0F * saturate(roughness) - 1.0F))
                                       - 1.0F + ao);
    return diffuse * DiffuseIndirectOcclusion(ao,indirectDiffuse.a) + specularLighting * specularOcclusion;
}

float3 EvaluateImageBasedLightingSeparateSamplers(
    TextureCube<float4> diffuseIrradiance,
    TextureCube<float4> specularPrefilter,
    Texture2D<float2> brdfLut,
    SamplerState diffuseSampler, SamplerState specularSampler, SamplerState brdfSampler,
    float3 normal, float3 viewDirection, float3 baseColor, float metallic, float roughness,
    float specular, float ambientOcclusion, float maximumSpecularLod, float environmentRotationRadians) {
    return EvaluateImageBasedLightingWithIndirect(diffuseIrradiance, specularPrefilter, brdfLut,
        diffuseSampler, specularSampler, brdfSampler, normal, viewDirection, baseColor, metallic,
        roughness, specular, ambientOcclusion, maximumSpecularLod, environmentRotationRadians,
        float4(0.0F, 0.0F, 0.0F, 0.0F));
}

float3 EvaluateImageBasedLighting(TextureCube<float4> diffuseIrradiance,TextureCube<float4> specularPrefilter,
    Texture2D<float2> brdfLut,SamplerState environmentSampler,float3 normal,float3 viewDirection,
    float3 baseColor,float metallic,float roughness,float specular,float ambientOcclusion,
    float maximumSpecularLod,float environmentRotationRadians){
    return EvaluateImageBasedLightingSeparateSamplers(diffuseIrradiance,specularPrefilter,brdfLut,
        environmentSampler,environmentSampler,environmentSampler,normal,viewDirection,baseColor,
        metallic,roughness,specular,ambientOcclusion,maximumSpecularLod,environmentRotationRadians);
}
#endif
