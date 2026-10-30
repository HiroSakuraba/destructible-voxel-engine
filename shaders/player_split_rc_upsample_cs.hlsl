struct Surface { float4 position; float4 normal; };
[[vk::binding(0,0)]] StructuredBuffer<Surface> surfaces : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<float4> light : register(t1);
[[vk::binding(2,0)]] StructuredBuffer<float4> settings : register(t2);
[[vk::binding(3,0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint width,height;output.GetDimensions(width,height);if(id.x>=width||id.y>=height)return;
    const uint columns=(width+7u)/8u,rows=(height+7u)/8u;
    const uint nearest=(id.y/8u)*columns+id.x/8u;const Surface reference=surfaces[nearest];
    float4 p=0;
    if(reference.position.w>.5F){
        const float2 g=(float2(id.xy)+.5F)/8.0F-.5F;const int2 base=(int2)floor(g);const float2 f=g-float2(base);
        float weightSum=0;
        [unroll]for(int oy=0;oy<2;++oy)[unroll]for(int ox=0;ox<2;++ox){
            const int2 q=base+int2(ox,oy);if(any(q<0)||q.x>=int(columns)||q.y>=int(rows))continue;
            const uint index=uint(q.y)*columns+uint(q.x);const Surface other=surfaces[index];const float4 value=light[index];
            if(value.w<.5F||other.position.w<.5F)continue;
            const float normal=dot(reference.normal.xyz,other.normal.xyz);
            const float plane=abs(dot(reference.normal.xyz,other.position.xyz-reference.position.xyz));
            if(normal<.85F||plane>settings[0].x)continue;
            const float weight=(ox?f.x:1-f.x)*(oy?f.y:1-f.y);p.xyz+=value.xyz*weight;weightSum+=weight;
        }
        if(weightSum>1e-6F){p.xyz/=weightSum;p.w=1;}
    }
    output[id.xy]=p;
}
