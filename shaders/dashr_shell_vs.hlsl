cbuffer DashrShellConstants : register(b0, space0) {
    float4x4 gObjectToClip;
    float4x4 gObjectToWorld;
    float4 gCameraObjectAndHeightScale;
    float4 gCameraWorldAndDebug;
    float4 gEnvironmentParameters;
    float4 gHeightAndStep;
    float4 gDistortion;
    float4 gMinimumStepAndReserved;
    float4 gHeightUvScaleOffset;
    float4 gHeightUvRotation;
    uint4 gLimits;
};

struct DashrShellVertexInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct DashrShellVertexOutput {
    float4 position : SV_Position;
    float3 objectPosition : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

DashrShellVertexOutput main(DashrShellVertexInput input) {
    DashrShellVertexOutput output;
    output.position = mul(gObjectToClip, float4(input.position, 1.0F));
    // Shadow recording sets gLimits.w so near-extruding conservative shell
    // vertices use the same depth-pancaking convention as live_csm_caster_vs.
    if (gLimits.w != 0U) output.position.z = max(output.position.z, 0.0F);
    output.objectPosition = input.position;
    output.uv = input.uv;
    return output;
}
