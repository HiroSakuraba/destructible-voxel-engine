#include "dve/material_surface_detail.hpp"
#include "dve/transform.hpp"
#include "dve/asset_cooker.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
namespace dve {
std::array<float,kSurfaceDetailFloatCount> surface_detail_values(const MaterialSurfaceDetail& d) noexcept {
    return {d.periodMeters,d.rotationRadians.x,d.rotationRadians.y,d.rotationRadians.z,d.grainStretch,d.colorVariation,d.roughnessVariation,d.bumpHeightMeters,d.uvTileMeters};
}
void set_surface_detail_values(MaterialSurfaceDetail& d,const std::array<float,kSurfaceDetailFloatCount>& v) noexcept {
    d.periodMeters=v[0];d.rotationRadians={v[1],v[2],v[3]};d.grainStretch=v[4];d.colorVariation=v[5];d.roughnessVariation=v[6];d.bumpHeightMeters=v[7];d.uvTileMeters=v[8];
}
bool material_surface_detail_valid(const MaterialSurfaceDetail& d) noexcept {
    if(unsigned(d.pattern)>unsigned(kLastSurfacePattern)||unsigned(d.space)>unsigned(SurfaceDetailSpace::UV1))return false;
    for(float v:surface_detail_values(d))if(!std::isfinite(v))return false;
    return d.periodMeters>=.00001F&&d.periodMeters<=10000 && d.grainStretch>=1&&d.grainStretch<=1000 &&
        std::abs(d.rotationRadians.x)<=10000&&std::abs(d.rotationRadians.y)<=10000&&std::abs(d.rotationRadians.z)<=10000 &&
        d.colorVariation>=0&&d.colorVariation<=1&&d.roughnessVariation>=0&&d.roughnessVariation<=1 &&
        d.bumpHeightMeters>=0&&d.bumpHeightMeters<=d.periodMeters*.25F && d.uvTileMeters>=.00001F&&d.uvTileMeters<=10000;
}
std::uint64_t material_surface_detail_hash(const MaterialSurfaceDetail& d) noexcept {
    if(d.pattern==SurfacePattern::NoPattern)return 0; // legacy hashes/bytes stay unchanged
    std::uint64_t h=1469598103934665603ULL;
    const auto add=[&](std::uint32_t value){for(unsigned i=0;i<32;i+=8)h=(h^((value>>i)&255U))*1099511628211ULL;};
    add(unsigned(d.pattern));add(unsigned(d.space));add(d.seed);for(float v:surface_detail_values(d))add(std::bit_cast<std::uint32_t>(v));return h;
}
namespace {
// Smooth lattice noise with analytic derivatives. Independent hashed corners avoid
// the directional striping of a small sum of sine waves on mineral and paint surfaces.
SurfacePatternSample lattice_noise(Float3 p,std::uint32_t seed) noexcept {
    const int ix=int(std::floor(p.x)),iy=int(std::floor(p.y)),iz=int(std::floor(p.z));
    const Float3 f{p.x-float(ix),p.y-float(iy),p.z-float(iz)};
    const auto smooth=[](float t){return t*t*t*(t*(t*6-15)+10);};
    const auto derivative=[](float t){return 30*t*t*(t-1)*(t-1);};
    const Float3 u{smooth(f.x),smooth(f.y),smooth(f.z)},du{derivative(f.x),derivative(f.y),derivative(f.z)};
    SurfacePatternSample out;
    for(unsigned corner=0;corner<8;++corner){
        const unsigned x=corner&1,y=(corner>>1)&1,z=corner>>2;
        std::uint32_t h=(std::uint32_t(ix)+x)*0x8da6b343U^(std::uint32_t(iy)+y)*0xd8163841U^(std::uint32_t(iz)+z)*0xcb1ab31fU^seed;
        h^=h>>16;h*=0x7feb352dU;h^=h>>15;h*=0x846ca68bU;h^=h>>16;
        const float value=float(h>>8)*(2.F/16777215.F)-1;
        const float wx=x?u.x:1-u.x,wy=y?u.y:1-u.y,wz=z?u.z:1-u.z;
        out.value+=value*wx*wy*wz;
        out.gradientPerMeter.x+=value*(x?du.x:-du.x)*wy*wz;
        out.gradientPerMeter.y+=value*wx*(y?du.y:-du.y)*wz;
        out.gradientPerMeter.z+=value*wx*wy*(z?du.z:-du.z);
    }
    return out;
}
std::uint32_t mix_hash(std::uint32_t h) noexcept {
    h^=h>>16;h*=0x7feb352dU;h^=h>>15;h*=0x846ca68bU;h^=h>>16;return h;
}
std::uint32_t cell_hash(int x,int y,int z,std::uint32_t seed) noexcept {
    return mix_hash(std::uint32_t(x)*0x8da6b343U^std::uint32_t(y)*0xd8163841U^std::uint32_t(z)*0xcb1ab31fU^seed);
}
float unit_float(std::uint32_t h) noexcept { return float(h>>8)*(1.F/16777215.F); }
// Three octaves of lattice noise, about [-1, 1].
float fbm(Float3 q,std::uint32_t seed) noexcept {
    float value=0,amplitude=.55F,frequency=1;
    for(unsigned i=0;i<3;++i){
        value+=amplitude*lattice_noise(multiply(q,frequency),seed+i*0x9e3779b9U).value;
        amplitude*=.55F;frequency*=2.03F;
    }
    return value;
}
float marble_exponent(const MaterialSurfaceDetail& d) noexcept { return std::clamp(d.grainStretch*.25F,1.F,64.F); }
// Veins: dark, recessed thin sheets along planes of constant x, bent by turbulence.
float marble_value(const MaterialSurfaceDetail& d,Float3 q) noexcept {
    const float t=q.x+1.1F*fbm(multiply(q,.7F),d.seed);
    const float vein=std::pow(1-std::abs(std::sin(3.14159265F*t)),marble_exponent(d));
    return .85F-1.7F*vein+.15F*fbm(multiply(q,3.F),d.seed+7U);
}
// Mean of the vein term over a period, (1 - |sin pi t|)^k, by midpoint quadrature.
float marble_mean(const MaterialSurfaceDetail& d) noexcept {
    const float k=marble_exponent(d);float sum=0;
    for(int i=0;i<64;++i)sum+=std::pow(1-std::abs(std::sin(3.14159265F*(float(i)+.5F)/64.F)),k);
    return .85F-1.7F*sum/64.F;
}
// Layers of constant y with per-layer hardness (hard layers stand proud), undulating slowly.
float strata_value(const MaterialSurfaceDetail& d,Float3 q) noexcept {
    const float run=(std::max)(d.grainStretch*.25F,.25F);
    const float y=q.y+.35F*fbm({q.x/run,q.y*.2F,q.z/run},d.seed);
    const float layer=std::floor(y),f=y-layer;
    const int i=int(layer);
    const auto hardness=[&](int n){return 2*unit_float(cell_hash(n,0,0,d.seed^0x51ed270bU))-1;};
    const float s=std::clamp(f/.12F,0.F,1.F),blend=s*s*(3-2*s);
    const float value=hardness(i-1)+(hardness(i)-hardness(i-1))*blend;
    return .85F*value+.15F*fbm(multiply(q,4.F),d.seed+11U);
}
float cellular_gap(const MaterialSurfaceDetail& d) noexcept { return std::clamp(1.6F/d.grainStretch,.005F,.5F); }
// Stones: jittered cells, domed, each its own height; cracks (value -1) between them.
float cellular_value(const MaterialSurfaceDetail& d,Float3 q) noexcept {
    const int ix=int(std::floor(q.x)),iy=int(std::floor(q.y)),iz=int(std::floor(q.z));
    float f1=1e9F,f2=1e9F;std::uint32_t nearest=0;
    for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
        const std::uint32_t h=cell_hash(ix+x,iy+y,iz+z,d.seed);
        const Float3 point{float(ix+x)+.1F+.8F*unit_float(h),float(iy+y)+.1F+.8F*unit_float(mix_hash(h+1U)),float(iz+z)+.1F+.8F*unit_float(mix_hash(h+2U))};
        const float distance=length(subtract(q,point));
        if(distance<f1){f2=f1;f1=distance;nearest=h;}else if(distance<f2)f2=distance;
    }
    const float s=std::clamp((f2-f1)/cellular_gap(d),0.F,1.F),stone=s*s*(3-2*s);
    const float top=.55F+.4F*unit_float(mix_hash(nearest+3U))-.6F*f1*f1;
    return -1+(top+1)*stone;
}
// Mean value over the volume as a function of the crack width (least-squares fit to 200 000
// samples, within 0.002 for gaps 0.005 .. 0.5; checked in tests/test_solid_textures.cpp).
float cellular_mean(float gap) noexcept { return .5768F+gap*(-2.6954F+1.5355F*gap); }
bool solid_pattern_sample(const MaterialSurfaceDetail& d,Float3 q,float width,SurfacePatternSample& result) noexcept {
    float (*value)(const MaterialSurfaceDetail&,Float3)=nullptr;float mean=0,band=1;
    switch(d.pattern){
        case SurfacePattern::Marble:value=marble_value;mean=marble_mean(d);band=1+.5F*marble_exponent(d);break;
        case SurfacePattern::Strata:value=strata_value;mean=0;band=4;break;
        case SurfacePattern::Cellular:value=cellular_value;mean=cellular_mean(cellular_gap(d));band=1/cellular_gap(d);break;
        default:return false;
    }
    // Centred on the pattern's mean, so it is neutral on average like the other patterns (colour
    // unchanged at a distance), and filtered: it fades to that neutral 0 as the pixel outgrows its
    // finest band (veins, layer edges, cracks).
    const float w=6.2831853F*width*band;
    const float attenuation=std::exp(-.5F*w*w);
    const float v=value(d,q);
    result.value=attenuation*(v-mean);
    // Gradient by central differences in pattern space (per period).
    constexpr float e=1e-3F;
    const auto at=[&](Float3 o){return value(d,add(q,o));};
    result.gradientPerMeter={(at({e,0,0})-at({-e,0,0}))/(2*e)*attenuation,(at({0,e,0})-at({0,-e,0}))/(2*e)*attenuation,
                             (at({0,0,e})-at({0,0,-e}))/(2*e)*attenuation};
    return true;
}
}
SurfacePatternSample sample_surface_pattern(const MaterialSurfaceDetail& d,Float3 p,float footprint) noexcept {
    if(d.pattern==SurfacePattern::NoPattern||!material_surface_detail_valid(d)||!std::isfinite(footprint))return {};
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))return {};
    const auto rotation=quaternion_from_euler_xyz(d.rotationRadians);
    const auto q=multiply(rotate(conjugate(rotation),p),1/d.periodMeters);
    if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z)||(std::max)({std::abs(q.x),std::abs(q.y),std::abs(q.z)})>1e8F)return {};
    const double phase=double(d.seed%65521)*.61803398875;
    const float width=(std::max)(0.F,footprint)/d.periodMeters;
    SurfacePatternSample result;
    // Each band is attenuated with its physical footprint, including its derivatives.
    const auto wave=[&](double argument,Float3 derivative,float frequencyBound,float weight){
        const float w=6.2831853F*width*frequencyBound;
        const float attenuation=std::exp(-.5F*w*w);
        const float a=weight*attenuation;
        result.value+=a*float(std::sin(argument));
        result.gradientPerMeter=add(result.gradientPerMeter,multiply(derivative,a*float(std::cos(argument))));
    };
    constexpr float tau=6.2831853F;
    if(solid_pattern_sample(d,q,width,result)){
    }else if(d.pattern==SurfacePattern::Wood){
        // Growth rings around the grain axis: side grain and end grain share one 3D field.
        const float warp=.2F*std::sin(q.x/d.grainStretch+float(phase));
        const float y=q.y+warp,z=q.z+.17F*std::sin(q.x/d.grainStretch*.7F);
        const float radius=std::sqrt(y*y+z*z+.04F);
        const Float3 dr{(y*.2F*std::cos(q.x/d.grainStretch+float(phase))+z*.119F*std::cos(q.x/d.grainStretch*.7F))/(radius*d.grainStretch),y/radius,z/radius};
        wave(tau*radius+phase,multiply(dr,tau),1.4F,.85F);
        const Float3 f{1/d.grainStretch,3.1F,1.7F};
        wave(tau*dot(q,f)+phase*1.7,multiply(f,tau),3.6F,.15F);
    }else{
        const bool fiber=d.pattern==SurfacePattern::Paper;
        const Float3 stretch=fiber?Float3{1,1/d.grainStretch,1}:Float3{1,1,1};
        const std::array<float,3> frequencies{1,2.03F,4.17F},weights{.55F,.3F,.15F};
        for(unsigned i=0;i<3;++i){
            const float frequency=frequencies[i];
            const auto n=lattice_noise({q.x*stretch.x*frequency,q.y*stretch.y*frequency,q.z*stretch.z*frequency},d.seed+i*0x9e3779b9U);
            const float w=tau*width*frequency;
            const float weight=weights[i]*std::exp(-.5F*w*w);
            result.value+=n.value*weight;
            result.gradientPerMeter=add(result.gradientPerMeter,{n.gradientPerMeter.x*stretch.x*frequency*weight,n.gradientPerMeter.y*stretch.y*frequency*weight,n.gradientPerMeter.z*stretch.z*frequency*weight});
        }
    }
    result.gradientPerMeter=rotate(rotation,multiply(result.gradientPerMeter,1/d.periodMeters));
    return result;
}
bool surface_pattern_is_height_source(const MaterialSurfaceDetail& d) noexcept {
    return d.pattern!=SurfacePattern::NoPattern&&(d.space==SurfaceDetailSpace::Object||d.space==SurfaceDetailSpace::World)&&
        material_surface_detail_valid(d);
}
float surface_pattern_height(const MaterialSurfaceDetail& d,Float3 p,float footprint) noexcept {
    if(!surface_pattern_is_height_source(d))return 1;
    return std::clamp(.5F+.5F*sample_surface_pattern(d,p,footprint).value,0.F,1.F);
}
void apply_material_surface_detail(VoxelMaterialDefinition& m,Float3& normal,const MaterialSurfaceContext& c) noexcept {
    const auto& d=m.surfaceDetail;if(d.pattern==SurfacePattern::NoPattern||!material_surface_detail_valid(d))return;
    const bool uv=d.space==SurfaceDetailSpace::UV0||d.space==SurfaceDetailSpace::UV1;
    if(uv&&!c.hasUv)return; // authoring rejects unsupported voxel UV recipes
    Float3 p=d.space==SurfaceDetailSpace::World?c.worldPositionMeters:c.objectPositionMeters;
    if(uv){const auto t=d.space==SurfaceDetailSpace::UV1?c.uv1:c.uv0;p={t.x*d.uvTileMeters,t.y*d.uvTileMeters,0};}
    const auto sample=sample_surface_pattern(d,p,c.footprintMeters);
    const float modulation=1+d.colorVariation*sample.value;
    m.baseColor.x=std::clamp(m.baseColor.x*modulation,0.F,1.F);m.baseColor.y=std::clamp(m.baseColor.y*modulation,0.F,1.F);m.baseColor.z=std::clamp(m.baseColor.z*modulation,0.F,1.F);
    m.roughness=std::clamp(m.roughness+d.roughnessVariation*sample.value,0.F,1.F);
    auto gradient=sample.gradientPerMeter;
    if(uv)gradient=add(multiply(c.tangent,gradient.x),multiply(c.bitangent,gradient.y));
    else if(d.space==SurfaceDetailSpace::Object)gradient=transform_vector(c.pose,gradient);
    gradient=subtract(gradient,multiply(normal,dot(gradient,normal)));
    auto slope=multiply(gradient,d.bumpHeightMeters);const float size=length(slope);if(size>1)slope=multiply(slope,1/size);
    normal=normalize(subtract(normal,slope));
}
}
