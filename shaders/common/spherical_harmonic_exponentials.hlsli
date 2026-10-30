#ifndef DVE_SPHERICAL_HARMONIC_EXPONENTIALS_HLSLI
#define DVE_SPHERICAL_HARMONIC_EXPONENTIALS_HLSLI

// DVE SHE-1: shader-side reconstruction for the CPU-fitted 208-byte probe format.
// Not bound into the live renderer yet: descriptor and PSO wiring belong to SHE-2.
// Input: StructuredBuffer<uint> containing 52 dwords per probe; RGB half coefficients
// are interleaved (33 * 3), with 5 trailing padding halfwords.
// Source: Silvennoinen, Sloan, Iwanicki, Nowrouzezahrai (HPG 2025), CGF e70219.

static const uint DVE_SHE_PROBE_WORDS = 52u;
static const float DVE_SHE_MIN_ALPHA = 0.25F;

// Same orthonormal basis and order as src/spherical_harmonic_exponentials.cpp.
void DveSheY4(float3 v, out float y[25]) {
    float x=v.x, yy=v.y, z=v.z;
    float xx=x*x, y2=yy*yy, zz=z*z;
    y[0]=0.28209479177387814F;
    y[1]=0.4886025119029199F*yy;
    y[2]=0.4886025119029199F*z;
    y[3]=0.4886025119029199F*x;
    y[4]=1.0925484305920792F*x*yy;
    y[5]=1.0925484305920792F*yy*z;
    y[6]=0.31539156525252005F*(3.0F*zz-1.0F);
    y[7]=1.0925484305920792F*x*z;
    y[8]=0.5462742152960396F*(xx-y2);
    y[9]=0.5900435899266435F*yy*(3.0F*xx-y2);
    y[10]=2.890611442640554F*x*yy*z;
    y[11]=0.4570457994644658F*yy*(5.0F*zz-1.0F);
    y[12]=0.3731763325901154F*z*(5.0F*zz-3.0F);
    y[13]=0.4570457994644658F*x*(5.0F*zz-1.0F);
    y[14]=1.445305721320277F*z*(xx-y2);
    y[15]=0.5900435899266435F*x*(xx-3.0F*y2);
    y[16]=2.5033429417967046F*x*yy*(xx-y2);
    y[17]=1.7701307697799304F*yy*z*(3.0F*xx-y2);
    y[18]=0.9461746957575601F*x*yy*(7.0F*zz-1.0F);
    y[19]=0.6690465435572892F*yy*z*(7.0F*zz-3.0F);
    y[20]=0.10578554691520431F*(35.0F*zz*zz-30.0F*zz+3.0F);
    y[21]=0.6690465435572892F*x*z*(7.0F*zz-3.0F);
    y[22]=0.47308734787878004F*(xx-y2)*(7.0F*zz-1.0F);
    y[23]=1.7701307697799304F*x*z*(xx-3.0F*y2);
    y[24]=0.6258357354491761F*(xx*xx-6.0F*xx*y2+y2*y2);
}

float3 DveSheReadRgb(StructuredBuffer<uint> packed, uint probe, uint coefficient) {
    uint first=coefficient*3u;
    uint offset=probe*DVE_SHE_PROBE_WORDS;
    uint word0=packed[offset+first/2u];
    uint word1=packed[offset+(first+1u)/2u];
    uint word2=packed[offset+(first+2u)/2u];
    uint a=((first&1u)!=0u)?(word0>>16u):(word0&0xffffu);
    uint b=(((first+1u)&1u)!=0u)?(word1>>16u):(word1&0xffffu);
    uint c=(((first+2u)&1u)!=0u)?(word2>>16u):(word2&0xffffu);
    return float3(f16tof32(a),f16tof32(b),f16tof32(c));
}

// Returns Fresnel=1 base integral E0. Multiply the result by the normalized
// split-sum Fresnel tail (F0*A+B)/(A+B), then apply existing specular AO.
// Caller must use split-sum when alpha<0.25 or if no probe has been fitted.
float3 DveSheEvaluateBase(StructuredBuffer<uint> packed, uint probe,
                          float3 normal, float3 viewDirection,
                          float perceptualRoughness) {
    float alpha=perceptualRoughness*perceptualRoughness;
    float3 N=normalize(normal),V=normalize(viewDirection);
    float3 R=reflect(-V,N);
    float3 hr=normalize(N+R);
    float yR[25],yH[25];
    DveSheY4(R,yR);
    DveSheY4(hr,yH);
    float3 logValue=0.0F.xxx;
    [unroll] for(uint i=0u;i<25u;++i){
        uint degree=i<1u?0u:i<4u?1u:i<9u?2u:i<16u?3u:4u;
        float weight=exp(-0.5F*float(degree*(degree+1u))*alpha);
        logValue+=DveSheReadRgb(packed,probe,i)*yR[i]*weight;
    }
    [unroll] for(uint i=1u;i<9u;++i){
        uint degree=i<4u?1u:2u;
        float weight=exp(-0.5F*float(degree*(degree+1u))*alpha);
        logValue+=DveSheReadRgb(packed,probe,24u+i)*yH[i]*weight;
    }
    return exp(clamp(logValue,-20.0F.xxx,16.0F.xxx));
}
#endif
