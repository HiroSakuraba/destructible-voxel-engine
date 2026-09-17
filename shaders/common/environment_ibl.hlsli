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

float3 EvaluateImageBasedLighting(
    TextureCube<float4> diffuseIrradiance,
    TextureCube<float4> specularPrefilter,
    Texture2D<float2> brdfLut,
    SamplerState environmentSampler,
    float3 normal,
    float3 viewDirection,
    float3 baseColor,
    float metallic,
    float roughness,
    float specular,
    float ambientOcclusion,
    float maximumSpecularLod,
    float environmentRotationRadians) {
    float3 N = normalize(normal);
    float3 V = normalize(viewDirection);
    float NdotV = saturate(dot(N, V));
    float3 R = reflect(-V, N);
    float3 dielectricF0 = (0.08F * saturate(specular)).xxx;
    float3 F0 = lerp(dielectricF0, max(baseColor, 0.0F.xxx), saturate(metallic));
    float3 irradiance = diffuseIrradiance.SampleLevel(environmentSampler,
        RotateEnvironmentDirectionY(N, environmentRotationRadians), 0.0F).rgb;
    float3 diffuse = irradiance * max(baseColor, 0.0F.xxx) * (1.0F - saturate(metallic)) / kPi;
    float3 prefiltered = specularPrefilter.SampleLevel(environmentSampler,
        RotateEnvironmentDirectionY(R, environmentRotationRadians),
        saturate(roughness) * maximumSpecularLod).rgb;
    float2 brdf = brdfLut.SampleLevel(environmentSampler,
        float2(NdotV, saturate(roughness)), 0.0F);
    float3 specularLighting = prefiltered * (F0 * brdf.x + brdf.y);
    float ao = saturate(ambientOcclusion);
    // Diffuse AO is not a valid specular visibility term. Lagarde's inexpensive approximation
    // preserves narrow reflections while still occluding rough reflections in cavities.
    float specularOcclusion = saturate(pow(NdotV + ao, exp2(-16.0F * saturate(roughness) - 1.0F))
                                       - 1.0F + ao);
    return diffuse * ao + specularLighting * specularOcclusion;
}

#endif
