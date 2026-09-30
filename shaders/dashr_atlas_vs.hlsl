struct DashrAtlasVertexInput {
    float3 position : POSITION;
    float3 dPdu : TEXCOORD0;
    float3 dPdv : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float2 distortion : TEXCOORD3;
};

struct DashrAtlasVertexOutput {
    float4 position : SV_Position;
    float3 row0 : TEXCOORD0;
    float3 row1 : TEXCOORD1;
    float3 row2 : TEXCOORD2;
    float3 objectAnchor : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float2 distortion : TEXCOORD5;
};

void InverseSurfaceBasis(float3 dPdu, float3 dPdv,
                         out float3 row0, out float3 row1, out float3 row2) {
    float3 normal = cross(dPdu, dPdv);
    const float normalLengthSquared = dot(normal, normal);
    if (normalLengthSquared <= 1.0e-12F) {
        dPdu = float3(1.0F, 0.0F, 0.0F);
        dPdv = float3(0.0F, 1.0F, 0.0F);
        normal = float3(0.0F, 0.0F, 1.0F);
    } else {
        normal *= rsqrt(normalLengthSquared);
    }

    // objectFromSurface columns are the scaled UV derivatives plus a unit
    // height normal. The reciprocal rows below preserve UV scale instead of
    // treating the lighting tangent as one object-space unit per UV unit.
    const float determinant = dot(dPdu, cross(dPdv, normal));
    if (abs(determinant) <= 1.0e-8F) {
        row0 = float3(1.0F, 0.0F, 0.0F);
        row1 = float3(0.0F, 1.0F, 0.0F);
        row2 = float3(0.0F, 0.0F, 1.0F);
        return;
    }
    const float inverseDeterminant = rcp(determinant);
    row0 = cross(dPdv, normal) * inverseDeterminant;
    row1 = cross(normal, dPdu) * inverseDeterminant;
    row2 = cross(dPdu, dPdv) * inverseDeterminant;
}

DashrAtlasVertexOutput main(DashrAtlasVertexInput input) {
    DashrAtlasVertexOutput output;
    InverseSurfaceBasis(input.dPdu, input.dPdv, output.row0, output.row1, output.row2);
    output.objectAnchor = input.position;
    output.uv = input.uv;
    output.distortion = input.distortion;

    // Rasterize directly in the surface parameterization.
    output.position = float4(input.uv.x * 2.0F - 1.0F,
                             1.0F - input.uv.y * 2.0F,
                             0.0F, 1.0F);
    return output;
}
