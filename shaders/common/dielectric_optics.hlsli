#ifndef DVE_DIELECTRIC_OPTICS_HLSLI
#define DVE_DIELECTRIC_OPTICS_HLSLI
float ClearDielectricFresnel(float cosI,float n1,float n2) {
    if(n1==n2)return 0;
    cosI=saturate(cosI);
    const float eta=n1/n2,sinT2=eta*eta*max(0,1-cosI*cosI);
    if(sinT2>=1)return 1;
    const float cosT=sqrt(max(0,1-sinT2));
    const float rs=(n1*cosI-n2*cosT)/max(1e-20F,n1*cosI+n2*cosT);
    const float rp=(n2*cosI-n1*cosT)/max(1e-20F,n2*cosI+n1*cosT);
    return saturate((rs*rs+rp*rp)*.5F);
}
bool ClearRefract(float3 direction,float3 opposingNormal,float eta,out float3 transmitted) {
    const float cosI=-dot(direction,opposingNormal),k=1-eta*eta*max(0,1-cosI*cosI);
    transmitted=0;
    if(k<0)return false;
    transmitted=normalize(eta*direction+(eta*cosI-sqrt(max(0,k)))*opposingNormal);return true;
}
float3 ClearAbsorption(float3 color,float referenceMeters,float distanceMeters) {
    if(referenceMeters<=0||distanceMeters<=0)return 1.0F.xxx;
    // Exact zero channels remain zero for any positive path length.
    return pow(saturate(color),distanceMeters/referenceMeters);
}
#endif
