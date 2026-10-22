#include "dve/render/cpu_fog.hpp"

#include <algorithm>
#include <chrono>
#include <bit>
#include <atomic>
#include <thread>
#include <cmath>
#include <limits>

namespace dve::render {
namespace {
Float3 cross(Float3 a, Float3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
Float3 divide(Float3 a, Float3 b) { return {a.x/b.x,a.y/b.y,a.z/b.z}; }
bool finite(Float3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
bool interval(const CpuFogVolume& volume, Float3 o, Float3 d, float& lo, float& hi) {
    o=divide(inverse_transform_point(volume.transform,o),volume.scale);
    d=divide(inverse_transform_vector(volume.transform,d),volume.scale);
    const auto& a=*volume.asset;
    const float origins[]{o.x,o.y,o.z}, dirs[]{d.x,d.y,d.z};
    const float mins[]{a.boundsMinimum.x,a.boundsMinimum.y,a.boundsMinimum.z};
    const float maxs[]{a.boundsMaximum.x,a.boundsMaximum.y,a.boundsMaximum.z};
    for(int axis=0;axis<3;++axis){
        if(std::abs(dirs[axis])<1e-20F){if(origins[axis]<mins[axis]||origins[axis]>maxs[axis])return false;continue;}
        float x=(mins[axis]-origins[axis])/dirs[axis],y=(maxs[axis]-origins[axis])/dirs[axis];
        if(x>y)std::swap(x,y);
        lo=std::max(lo,x);hi=std::min(hi,y);if(hi<=lo)return false;
    }
    return hi>lo;
}
void composite(PolygonRenderTarget& target,std::size_t i,const CpuFogSample& fog){
    auto& c=target.hdrColor[i];const float t=fog.transmittance;
    c={c.x*t+fog.radiance.x,c.y*t+fog.radiance.y,c.z*t+fog.radiance.z,c.w};
    auto& p=target.passes;
    if(p.enabled){
        for(auto* pass:{&p.directDiffuse,&p.directSpecular,&p.indirectDiffuse,&p.indirectSpecular,
                        &p.emission,&p.transmission,&p.background,&p.fog})
            (*pass)[i]=multiply((*pass)[i],t);
        p.fog[i]=add(p.fog[i],fog.radiance);
    }
}
void add_stats(CpuFogStats& a,const CpuFogStats& b){
    a.rays+=b.rays;a.steps+=b.steps;a.densityEvaluations+=b.densityEvaluations;a.volumeTests+=b.volumeTests;
    a.terminatedRays+=b.terminatedRays;a.budgetLimitedRays+=b.budgetLimitedRays;
    a.lightingCacheHits+=b.lightingCacheHits;a.shadowRays+=b.shadowRays;a.shadowDensityEvaluations+=b.shadowDensityEvaluations;
    a.edgeRetraces+=b.edgeRetraces;a.historyAccepted+=b.historyAccepted;a.historyRejected+=b.historyRejected;
    a.primitiveEvaluations+=b.primitiveEvaluations;a.densityNodes+=b.densityNodes;
}
bool valid_settings(const CpuFogSettings& s){
    return s.maximumSteps>0&&s.maximumSteps<=2048&&std::isfinite(s.stepMeters)&&s.stepMeters>0&&
        std::isfinite(s.minimumTransmittance)&&s.minimumTransmittance>=0&&s.minimumTransmittance<1&&
        (s.resolutionDivisor==1||s.resolutionDivisor==2||s.resolutionDivisor==4)&&
        std::isfinite(s.depthRejectionMeters)&&s.depthRejectionMeters>0&&
        std::isfinite(s.historyWeight)&&s.historyWeight>=0&&s.historyWeight<=0.95F&&s.shadowSteps>0&&s.shadowSteps<=64&&
        std::isfinite(s.shadowDistanceMeters)&&s.shadowDistanceMeters>0&&s.lights.size()<=32&&s.threadCount<=64&&std::isfinite(s.lightCacheCellMeters)&&s.lightCacheCellMeters>=0;
}
Float3 product(Float3 a,Float3 b){return {a.x*b.x,a.y*b.y,a.z*b.z};}
float phase(float cosine,float g){
    g=std::clamp(g,-0.95F,0.95F);
    const float denominator=std::max(1e-6F,1+g*g-2*g*cosine);
    return (1-g*g)/(12.56637061436F*denominator*std::sqrt(denominator));
}
float light_visibility(Float3 p,Float3 towardLight,float distance,float metersPerUnit,
    std::span<const CpuFogVolume> volumes,const CpuFogSettings& settings,CpuFogStats* stats,std::uint32_t lightId){
    std::uint64_t cell=1469598103934665603ULL;
    bool cached=settings.lightingCache&&settings.lightCacheCellMeters>0;
    const auto originalP=p;
    if(cached){
        const float size=settings.lightCacheCellMeters/metersPerUnit;
        const auto axis=[&](float x){const float q=std::floor(x/size);cell=(cell^std::bit_cast<std::uint32_t>(q))*1099511628211ULL;return (q+0.5F)*size;};
        p={axis(p.x),axis(p.y),axis(p.z)};cell=(cell^lightId)*1099511628211ULL;
        if(!finite(p)){cached=false;p=originalP;}
        float value;
        if(cached&&settings.lightingCache->find(cell,value)){if(stats)++stats->lightingCacheHits;return value;}
        if(lightId>0){const auto delta=subtract(settings.lights[lightId-1].position,p);distance=length(delta);towardLight=normalize(delta);}
    }
    if(stats)++stats->shadowRays;
    float scene=1;
    if(settings.sceneVisibility)scene=std::clamp(settings.sceneVisibility(p,towardLight,distance),0.0F,1.0F);
    if(!settings.volumeShadows||scene<=0){if(cached)settings.lightingCache->store(cell,scene);return scene;}
    float tau=0;
    for(const auto& v:volumes){
        if(!v.visible||!v.asset)continue;
        float lo=0,hi=distance;
        if(!interval(v,p,towardLight,lo,hi))continue;
        const float ds=(hi-lo)/static_cast<float>(settings.shadowSteps);
        for(std::uint32_t j=0;j<settings.shadowSteps;++j){
            const auto world=add(p,multiply(towardLight,lo+(static_cast<float>(j)+0.5F)*ds));
            const auto local=divide(inverse_transform_point(v.transform,world),v.scale);
            tau+=(settings.densityCache?settings.densityCache->density(*v.asset,local,stats):evaluate_gabor_density(*v.asset,local))*ds*metersPerUnit;
            if(stats)++stats->shadowDensityEvaluations;
        }
    }
    const float result=scene*std::exp(-tau);if(cached)settings.lightingCache->store(cell,result);return result;
}
Float3 illumination(Float3 p,Float3 ray,float g,float metersPerUnit,
    std::span<const CpuFogVolume> volumes,const RenderEnvironment& e,const CpuFogSettings& settings,CpuFogStats* stats){
    Float3 light=e.skyColor;
    const Float3 sun=normalize(e.sunDirection);
    const float visibility=light_visibility(p,sun,settings.shadowDistanceMeters/metersPerUnit,metersPerUnit,volumes,settings,stats,0);
    light=add(light,multiply(e.sunColor,e.sunIntensity*phase(dot(ray,sun),g)*visibility));
    std::uint32_t lightId=0;
    for(const auto& l:settings.lights){
        ++lightId;
        const auto delta=subtract(l.position,p);const float distance=length(delta),metres=distance*metersPerUnit;
        if(metres>=l.rangeMeters||distance<1e-6F)continue;
        const auto toward=multiply(delta,1/distance);
        const float falloff=std::pow(1-metres/l.rangeMeters,2.0F)/std::max(metres*metres,0.01F);
        const float v=light_visibility(p,toward,distance,metersPerUnit,volumes,settings,stats,lightId);
        light=add(light,multiply(l.color,l.intensity*falloff*phase(dot(ray,toward),g)*v));
    }
    return light;
}
} // namespace

CpuFogSettings cpu_fog_quality_settings(GaborVolumeQuality quality){
    CpuFogSettings s;
    switch(quality){
        case GaborVolumeQuality::Low:s.maximumSteps=32;s.resolutionDivisor=4;s.stepMeters=0.25F;break;
        case GaborVolumeQuality::Medium:s.maximumSteps=64;s.resolutionDivisor=2;s.stepMeters=0.15F;break;
        case GaborVolumeQuality::High:s.maximumSteps=128;s.stepMeters=0.075F;s.scattering=true;break;
        case GaborVolumeQuality::Cinematic:s.maximumSteps=512;s.stepMeters=0.025F;s.scattering=true;s.volumeShadows=true;s.shadowSteps=16;break;
    }
    return s;
}

CpuFogSample trace_cpu_fog(Float3 origin,Float3 direction,float nearDistance,float farDistance,
    float metersPerUnit,std::span<const CpuFogVolume> volumes,const RenderEnvironment& environment,
    const CpuFogSettings& settings,CpuFogStats* stats,bool includeDistanceFog){
    CpuFogSample result;
    if(!settings.enabled||!valid_settings(settings)||!finite(origin)||!finite(direction)||
       !std::isfinite(nearDistance)||!std::isfinite(farDistance)||farDistance<=nearDistance||
       !(metersPerUnit>0)||!std::isfinite(metersPerUnit))return result;
    direction=normalize(direction);
    if(length_squared(direction)<0.5F)return result;
    if(stats)++stats->rays;
    // Split at every volume boundary: a small volume cannot disappear between
    // samples on a long camera ray, and empty gaps require no density samples.
    struct Range{const CpuFogVolume* volume;float lo,hi;};
    std::vector<Range> ranges,wholeRanges;
    std::vector<float> cuts{nearDistance,farDistance};
    const float fogStart=environment.fogStartMeters/metersPerUnit;
    if(includeDistanceFog&&fogStart>nearDistance&&fogStart<farDistance)cuts.push_back(fogStart);
    for(const auto& v:volumes){
        if(!v.visible||!v.asset)continue;
        if(stats)++stats->volumeTests;
        float lo=nearDistance,hi=farDistance;
        hi=std::min(hi,nearDistance+v.asset->material.maximumRayDistance/metersPerUnit);
        if(interval(v,origin,direction,lo,hi)){
            wholeRanges.push_back({&v,lo,hi});
            const auto occupied=settings.densityCache?settings.densityCache->occupied(*v.asset,
                divide(inverse_transform_point(v.transform,origin),v.scale),
                divide(inverse_transform_vector(v.transform,direction),v.scale),lo,hi):std::vector<Float2>{{lo,hi}};
            for(auto q:occupied){ranges.push_back({&v,q.x,q.y});cuts.push_back(q.x);cuts.push_back(q.y);}
        }
    }
    // Cap event subdivision globally. If sparse BVHs would create too many
    // pieces, retain the original whole-volume ranges without dropping density.
    if(ranges.size()>64){
        ranges=wholeRanges;cuts={nearDistance,farDistance};
        if(includeDistanceFog&&fogStart>nearDistance&&fogStart<farDistance)cuts.push_back(fogStart);
        for(const auto& r:ranges){cuts.push_back(r.lo);cuts.push_back(r.hi);}
    }
    std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
    // Distribute a finite step budget over occupied intervals. Every interval
    // gets at least one sample; the caller can observe when the target is missed.
    float occupied=0;
    for(std::size_t k=1;k<cuts.size();++k){
        const float mid=(cuts[k]+cuts[k-1])*0.5F;
        if(std::any_of(ranges.begin(),ranges.end(),[&](const Range& r){return mid>=r.lo&&mid<=r.hi;}))
            occupied+=cuts[k]-cuts[k-1];
    }
    float requestedStep=settings.stepMeters;
    if(settings.adaptiveSampling)for(const auto& range:ranges)if(range.volume->sampleStepMeters>0)
        requestedStep=std::min(requestedStep,range.volume->sampleStepMeters);
    const float step=std::max(requestedStep/metersPerUnit,occupied/static_cast<float>(settings.maximumSteps));
    if(stats&&occupied*metersPerUnit>requestedStep*static_cast<float>(settings.maximumSteps))++stats->budgetLimitedRays;
    for(std::size_t k=1;k<cuts.size();++k){
        const float start=cuts[k-1],end=cuts[k],mid=(start+end)*0.5F;
        std::vector<const CpuFogVolume*> active;
        for(const auto& r:ranges)if(mid>=r.lo&&mid<=r.hi)active.push_back(r.volume);
        const float global=(includeDistanceFog&&mid>=fogStart)?environment.fogDensityPerMeter:0.0F;
        if(active.empty()&&!(global>0))continue;
        const auto count=active.empty()?1U:std::max(1U,static_cast<std::uint32_t>(std::ceil((end-start)/step)));
        const float ds=(end-start)/static_cast<float>(count);
        for(std::uint32_t j=0;j<count;++j){
            std::uint32_t seed=(j+1U)*747796405U+static_cast<std::uint32_t>(k)*2891336453U+settings.frameIndex*277803737U;
            seed=(seed^(seed>>16U))*2246822519U;seed^=seed>>13U;
            const float offset=settings.jitter?(static_cast<float>(seed&0xFFFFFFU)+0.5F)/16777216.0F:0.5F;
            const Float3 p=add(origin,multiply(direction,start+(static_cast<float>(j)+offset)*ds));
            float extinction=global;Float3 source=multiply(environment.fogColor,global);
            for(const auto* v:active){
                const Float3 local=divide(inverse_transform_point(v->transform,p),v->scale);
                const float density=settings.densityCache?settings.densityCache->density(*v->asset,local,stats):evaluate_gabor_density(*v->asset,local);
                if(stats){++stats->densityEvaluations;if(!settings.densityCache)stats->primitiveEvaluations+=v->asset->primitives.size();}
                const auto& m=v->asset->material;
                extinction+=density;
                const auto albedo=settings.scattering?product(m.albedoTint,illumination(p,direction,m.anisotropy,metersPerUnit,volumes,environment,settings,stats)):m.albedoTint;
                source=add(source,multiply(add(albedo,multiply(m.emissionColor,m.emissionIntensity)),density));
            }
            if(stats)++stats->steps;
            if(extinction>0){
                const float alpha=-std::expm1(-extinction*ds*metersPerUnit);
                result.radiance=add(result.radiance,multiply(source,result.transmittance*alpha/extinction));
                result.transmittance*=1-alpha;
            }
            if(result.transmittance<=settings.minimumTransmittance){if(stats)++stats->terminatedRays;return result;}
        }
    }
    return result;
}

bool apply_cpu_fog(PolygonRenderTarget& target,const PolygonCamera& camera,const RenderEnvironment& environment,
    float metersPerUnit,std::span<const CpuFogVolume> volumes,const CpuFogSettings& settings,CpuFogStats* stats,std::string* error){
    const auto start=std::chrono::steady_clock::now();
    const auto fail=[&](const char* text){if(error)*error=text;return false;};
    if(volumes.size()>32)return fail("CPU fog supports at most 32 simultaneous volumes");
    if(!target.valid()||!valid_settings(settings)||!environment.validate()||!std::isfinite(metersPerUnit)||metersPerUnit<=0||
       !finite(camera.position)||!finite(camera.target)||!finite(camera.up)||!std::isfinite(camera.nearPlane)||camera.nearPlane<0||
       !std::isfinite(camera.farPlane)||camera.farPlane<=camera.nearPlane||
       !std::isfinite(camera.verticalFieldOfViewRadians)||camera.verticalFieldOfViewRadians<=0||camera.verticalFieldOfViewRadians>=3.13F)
        return fail("Invalid CPU fog target, camera, environment or settings");
    for(const auto& v:volumes){
        if(!v.visible)continue;
        if(!std::isfinite(v.sampleStepMeters)||v.sampleStepMeters<0)return fail("Invalid CPU fog sample step");
        if(!v.asset||!v.asset->validate()||!finite(v.scale)||v.scale.x<=0||v.scale.y<=0||v.scale.z<=0||!finite(v.transform.position)||
           !std::isfinite(v.transform.rotation.x)||!std::isfinite(v.transform.rotation.y)||!std::isfinite(v.transform.rotation.z)||
           !std::isfinite(v.transform.rotation.w))return fail("Invalid CPU fog volume");
    }
    for(const auto& l:settings.lights)if(!finite(l.position)||!finite(l.color)||l.color.x<0||l.color.y<0||l.color.z<0||
       !std::isfinite(l.intensity)||l.intensity<0||!std::isfinite(l.rangeMeters)||l.rangeMeters<=0)return fail("Invalid CPU fog light");
    if(!settings.enabled){if(settings.history)settings.history->clear();return true;}
    if(settings.densityCache)for(const auto& v:volumes)if(v.visible)(void)settings.densityCache->prepare(*v.asset);
    std::vector<CpuFogVolume> preparedVolumes(volumes.begin(),volumes.end());
    for(auto& v:preparedVolumes)if(v.visible&&settings.adaptiveSampling&&v.sampleStepMeters==0){
        float feature=std::numeric_limits<float>::max();
        for(const auto& p:v.asset->primitives)
            feature=std::min(feature,0.5F*std::min({p.scale.x,p.scale.y,p.scale.z})/std::max(1.0F,p.frequency));
        v.sampleStepMeters=feature*std::min({v.scale.x,v.scale.y,v.scale.z})*metersPerUnit;
    }
    volumes=preparedVolumes;
    const Float3 f=normalize(subtract(camera.target,camera.position));
    const Float3 r=normalize(cross(f,camera.up)),u=cross(r,f);
    if(length_squared(f)<0.5F||length_squared(r)<0.5F)return fail("Degenerate CPU fog camera");
    const float tanHalf=std::tan(camera.verticalFieldOfViewRadians*0.5F),aspect=static_cast<float>(target.width)/static_cast<float>(target.height);
    const auto rayFor=[&](float x,float y){
        return normalize(add(f,add(multiply(r,(2*(x+0.5F)/static_cast<float>(target.width)-1)*tanHalf*aspect),
            multiply(u,(1-2*(y+0.5F)/static_cast<float>(target.height))*tanHalf))));
    };
    const auto depthDistance=[&](std::size_t i){return camera.nearPlane+std::clamp(target.depth[i],0.0F,1.0F)*(camera.farPlane-camera.nearPlane);};
    const auto trace=[&](std::uint32_t x,std::uint32_t y,CpuFogStats* workerStats){
        const auto ray=rayFor(static_cast<float>(x),static_cast<float>(y));
        const std::size_t i=static_cast<std::size_t>(y)*target.width+x;
        auto pixelSettings=settings;
        pixelSettings.frameIndex^=(x*19349663U)^(y*83492791U);
        return trace_cpu_fog(camera.position,ray,0,depthDistance(i)/dot(ray,f),metersPerUnit,
                             volumes,environment,pixelSettings,workerStats,target.depth[i]<1.0F);
    };
    // Exact inputs define the stationary history key. Camera motion resets rather
    // than reprojecting without motion vectors. Assets are immutable/hash-stamped.
    std::uint64_t key=1469598103934665603ULL;
    const auto hash=[&](std::uint64_t v){key=(key^v)*1099511628211ULL;};
    const auto hf=[&](float v){hash(std::bit_cast<std::uint32_t>(v));};
    const auto hv=[&](Float3 v){hf(v.x);hf(v.y);hf(v.z);};
    hash(target.width);hash(target.height);hash(settings.sceneRevision);
    hv(camera.position);hv(camera.target);hv(camera.up);hf(camera.nearPlane);hf(camera.farPlane);hf(camera.verticalFieldOfViewRadians);hf(metersPerUnit);
    hf(environment.fogDensityPerMeter);hf(environment.fogStartMeters);hv(environment.fogColor);
    hash(settings.maximumSteps);hf(settings.stepMeters);hf(settings.minimumTransmittance);hash(settings.resolutionDivisor);hash(settings.jitter);hash(settings.adaptiveSampling);hash(settings.scattering);hash(settings.volumeShadows);hash(settings.shadowSteps);hf(settings.shadowDistanceMeters);hf(settings.lightCacheCellMeters);hash(static_cast<bool>(settings.sceneVisibility));
    hv(environment.sunDirection);hv(environment.sunColor);hf(environment.sunIntensity);hv(environment.skyColor);
    for(const auto& l:settings.lights){hv(l.position);hv(l.color);hf(l.intensity);hf(l.rangeMeters);}
    for(const auto& v:volumes){hash(v.visible);if(!v.visible)continue;hash(v.asset->contentHash);if(v.asset->contentHash==0)hash(settings.frameIndex);hv(v.transform.position);hv(v.scale);
        hf(v.sampleStepMeters);hf(v.transform.rotation.x);hf(v.transform.rotation.y);hf(v.transform.rotation.z);hf(v.transform.rotation.w);}
    const bool historyEligible=std::all_of(volumes.begin(),volumes.end(),[](const auto& v){return !v.visible||v.asset->contentHash!=0;});
    if(settings.lightingCache)settings.lightingCache->reset(key);
    std::vector<CpuFogSample> samples(target.hdrColor.size());
    const auto divisor=settings.resolutionDivisor;
    const auto lw=(target.width+divisor-1)/divisor,lh=(target.height+divisor-1)/divisor;
    std::vector<CpuFogSample> low(static_cast<std::size_t>(lw)*lh);
    std::vector<float> lowDepth(low.size());
    std::vector<std::uint8_t> lowSky(low.size());
    const auto threadCount=std::clamp(settings.threadCount?settings.threadCount:std::thread::hardware_concurrency(),1U,16U);
    const auto rows=[&](std::uint32_t height,const auto& task){
        const auto workers=std::min(threadCount,std::max(1U,height));
        std::vector<CpuFogStats> workerStats(workers);
        std::atomic<std::uint32_t> next{0};
        const auto work=[&](std::uint32_t worker){
            for(;;){const auto begin=next.fetch_add(8);if(begin>=height)break;
                for(auto y=begin;y<std::min(begin+8,height);++y)task(y,&workerStats[worker]);}
        };
        std::vector<std::jthread> threads;
        for(std::uint32_t worker=1;worker<workers;++worker)threads.emplace_back(work,worker);
        work(0);for(auto& t:threads)t.join();
        if(stats)for(const auto& worker:workerStats)add_stats(*stats,worker);
    };
    rows(lh,[&](std::uint32_t y,CpuFogStats* workerStats){for(std::uint32_t x=0;x<lw;++x){
        const auto px=std::min(x*divisor+divisor/2,target.width-1),py=std::min(y*divisor+divisor/2,target.height-1);
        const std::size_t i=static_cast<std::size_t>(y)*lw+x,full=static_cast<std::size_t>(py)*target.width+px;
        low[i]=trace(px,py,workerStats);lowDepth[i]=depthDistance(full);lowSky[i]=target.depth[full]>=1;
    }});
    rows(target.height,[&](std::uint32_t y,CpuFogStats* workerStats){for(std::uint32_t x=0;x<target.width;++x){
        const std::size_t i=static_cast<std::size_t>(y)*target.width+x;
        const float fx=(static_cast<float>(x)-static_cast<float>(divisor/2))/static_cast<float>(divisor);
        const float fy=(static_cast<float>(y)-static_cast<float>(divisor/2))/static_cast<float>(divisor);
        const auto bx=static_cast<int>(std::floor(fx)),by=static_cast<int>(std::floor(fy));
        const float tx=fx-std::floor(fx),ty=fy-std::floor(fy);
        CpuFogSample sample;sample.transmittance=0;float total=0;
        for(int dy=0;dy<=1;++dy)for(int dx=0;dx<=1;++dx){
            const auto sx=static_cast<std::uint32_t>(std::clamp(bx+dx,0,static_cast<int>(lw)-1));
            const auto sy=static_cast<std::uint32_t>(std::clamp(by+dy,0,static_cast<int>(lh)-1));
            const std::size_t j=static_cast<std::size_t>(sy)*lw+sx;
            const float w=(dx?tx:1-tx)*(dy?ty:1-ty);
            if(lowSky[j]!=(target.depth[i]>=1)||std::abs(lowDepth[j]-depthDistance(i))*metersPerUnit>settings.depthRejectionMeters)continue;
            total+=w;sample.radiance=add(sample.radiance,multiply(low[j].radiance,w));sample.transmittance+=low[j].transmittance*w;
        }
        // Near discontinuities we need the full ray, not a weighted sample from
        // a different depth. This also covers thin walls missed by the low grid.
        if(total<0.999F){sample=trace(x,y,workerStats);++workerStats->edgeRetraces;}
        else{sample.radiance=multiply(sample.radiance,1/total);sample.transmittance/=total;}
        auto* h=settings.history;
        if(h&&settings.historyWeight>0){
            if(historyEligible&&
               h->key==key&&h->samples.size()==samples.size()&&h->depth.size()==target.depth.size()&&h->depth[i]==target.depth[i]){
                const auto& previous=h->samples[i];const float w=settings.historyWeight;
                sample.radiance=add(multiply(sample.radiance,1-w),multiply(previous.radiance,w));
                sample.transmittance=sample.transmittance*(1-w)+previous.transmittance*w;
                ++workerStats->historyAccepted;
            }else ++workerStats->historyRejected;
        }
        samples[i]=sample;composite(target,i,sample);
    }});
    if(settings.history){settings.history->key=key;settings.history->samples=std::move(samples);settings.history->depth=target.depth;}
    if(stats)stats->milliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    return true;
}
} // namespace dve::render
