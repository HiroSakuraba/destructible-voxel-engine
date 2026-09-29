struct DashrAtlasPixelInput {
    float4 position : SV_Position;
    float3 row0 : TEXCOORD0;
    float3 row1 : TEXCOORD1;
    float3 row2 : TEXCOORD2;
    float3 objectAnchor : TEXCOORD3;
    float2 uv : TEXCOORD4;
};

struct DashrAtlasPixelOutput {
    float4 row0AndDistortionU : SV_Target0;
    float4 row1AndDistortionV : SV_Target1;
    float4 row2AndValidity : SV_Target2;
    float4 objectAnchorAndValidity : SV_Target3;
};

DashrAtlasPixelOutput main(DashrAtlasPixelInput input) {
    DashrAtlasPixelOutput output;

    // Because the mesh is rasterized in UV space, screen derivatives of UV and
    // objectAnchor directly estimate the local deformation Jacobian. Dividing
    // by the corresponding UV derivative normalizes an undeformed mapping to 1.
    const float du = ddx(input.uv.x);
    const float dv = ddy(input.uv.y);
    const float distortionU = abs(du) > 1.0e-8F
        ? dot(input.row0, ddx(input.objectAnchor)) / du : 1.0F;
    const float distortionV = abs(dv) > 1.0e-8F
        ? dot(input.row1, ddy(input.objectAnchor)) / dv : 1.0F;

    output.row0AndDistortionU = float4(input.row0, distortionU);
    output.row1AndDistortionV = float4(input.row1, distortionV);
    output.row2AndValidity = float4(input.row2, 1.0F);
    output.objectAnchorAndValidity = float4(input.objectAnchor, 1.0F);
    return output;
}
