struct DashrAtlasVertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct DashrAtlasVertexOutput {
    float4 position : SV_Position;
    float3 row0 : TEXCOORD0;
    float3 row1 : TEXCOORD1;
    float3 row2 : TEXCOORD2;
    float3 objectAnchor : TEXCOORD3;
    float2 uv : TEXCOORD4;
};

float3x3 Inverse3x3(float3 a, float3 b, float3 c) {
    const float3 crossBC = cross(b, c);
    const float determinant = dot(a, crossBC);
    if (abs(determinant) <= 1.0e-8F) {
        return float3x3(1,0,0, 0,1,0, 0,0,1);
    }
    const float inverseDeterminant = rcp(determinant);
    // Columns of objectFromSurface are tangent, bitangent, normal.
    // Rows of the inverse are the reciprocal basis.
    return float3x3(
        crossBC * inverseDeterminant,
        cross(c, a) * inverseDeterminant,
        cross(a, b) * inverseDeterminant);
}

DashrAtlasVertexOutput main(DashrAtlasVertexInput input) {
    DashrAtlasVertexOutput output;
    const float3 normal = normalize(input.normal);
    const float3 tangent = normalize(input.tangent.xyz);
    const float3 bitangent = normalize(cross(normal, tangent)) *
                             (input.tangent.w < 0.0F ? -1.0F : 1.0F);
    const float3x3 surfaceFromObject = Inverse3x3(tangent, bitangent, normal);

    output.row0 = surfaceFromObject[0];
    output.row1 = surfaceFromObject[1];
    output.row2 = surfaceFromObject[2];
    output.objectAnchor = input.position;
    output.uv = input.uv;

    // UV atlas coordinates become raster coordinates. Flip Y to match the
    // texture convention used by DVE's Vulkan path.
    output.position = float4(input.uv.x * 2.0F - 1.0F,
                             1.0F - input.uv.y * 2.0F,
                             0.0F, 1.0F);
    return output;
}
