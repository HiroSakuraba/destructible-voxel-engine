struct DashrAtlasPixelInput {
    float4 position : SV_Position;
    float3 row0 : TEXCOORD0;
    float3 row1 : TEXCOORD1;
    float3 row2 : TEXCOORD2;
    float3 objectAnchor : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float2 distortion : TEXCOORD5;
};

struct DashrAtlasPixelOutput {
    float4 row0AndDistortionU : SV_Target0;
    float4 row1AndDistortionV : SV_Target1;
    float4 row2AndValidity : SV_Target2;
    float4 objectAnchorAndValidity : SV_Target3;
};

DashrAtlasPixelOutput main(DashrAtlasPixelInput input) {
    DashrAtlasPixelOutput output;
    output.row0AndDistortionU = float4(input.row0, input.distortion.x);
    output.row1AndDistortionV = float4(input.row1, input.distortion.y);
    output.row2AndValidity = float4(input.row2, 1.0F);
    output.objectAnchorAndValidity = float4(input.objectAnchor, 1.0F);
    return output;
}
