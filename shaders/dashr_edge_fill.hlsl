Texture2D<float4> gDashrRaw0 : register(t0);
Texture2D<float4> gDashrRaw1 : register(t1);
Texture2D<float4> gDashrRaw2 : register(t2);
Texture2D<float4> gDashrRaw3 : register(t3);

RWTexture2D<float4> gDashrFilled0 : register(u4);
RWTexture2D<float4> gDashrFilled1 : register(u5);
RWTexture2D<float4> gDashrFilled2 : register(u6);
RWTexture2D<float4> gDashrFilled3 : register(u7);

bool ValidDashrTexel(int2 pixel) {
    return gDashrRaw2.Load(int3(pixel, 0)).a > 0.5F;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint width;
    uint height;
    gDashrRaw0.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height) return;

    const int2 pixel = int2(dispatchThreadId.xy);
    int2 source = pixel;
    bool found = ValidDashrTexel(pixel);

    if (!found) {
        float bestDistanceSquared = 1.0e20F;
        [unroll]
        for (int y = -2; y <= 2; ++y) {
            [unroll]
            for (int x = -2; x <= 2; ++x) {
                if (x == 0 && y == 0) continue;
                const int2 candidate = pixel + int2(x, y);
                if (candidate.x < 0 || candidate.y < 0 ||
                    candidate.x >= int(width) || candidate.y >= int(height)) continue;
                if (!ValidDashrTexel(candidate)) continue;
                const float distanceSquared = float(x * x + y * y);
                if (distanceSquared < bestDistanceSquared) {
                    bestDistanceSquared = distanceSquared;
                    source = candidate;
                    found = true;
                }
            }
        }
    }

    if (!found) {
        gDashrFilled0[pixel] = 0.0F.xxxx;
        gDashrFilled1[pixel] = 0.0F.xxxx;
        gDashrFilled2[pixel] = 0.0F.xxxx;
        gDashrFilled3[pixel] = 0.0F.xxxx;
        return;
    }

    gDashrFilled0[pixel] = gDashrRaw0.Load(int3(source, 0));
    gDashrFilled1[pixel] = gDashrRaw1.Load(int3(source, 0));
    gDashrFilled2[pixel] = gDashrRaw2.Load(int3(source, 0));
    gDashrFilled3[pixel] = gDashrRaw3.Load(int3(source, 0));
}
