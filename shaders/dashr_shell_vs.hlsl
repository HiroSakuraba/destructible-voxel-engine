cbuffer DashrShellConstants : register(b0, space0) {
    float4x4 gObjectToClip;
    float4 gCameraAndHeightScale;
    float4 gHeightAndStep;
    float4 gDistortion;
    float4 gMinimumStepAndDebug;
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
    output.objectPosition = input.position;
    output.uv = input.uv;
    return output;
}
