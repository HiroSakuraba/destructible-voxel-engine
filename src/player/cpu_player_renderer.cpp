#include "dve/player/player_renderer.hpp"
#include "dve/render/scene_lighting.hpp"

#include "dve/polygon_asset.hpp"
#include "dve/render/caustic_map.hpp"
#include "dve/render/voxel_reference_renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dve::player {
namespace {

using Clock = std::chrono::steady_clock;

// Local cross product: game_world.cpp's cross_product is not declared in any header.
[[nodiscard]] Float3 cross_racing(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Palette for voxel objects without a cooked material table (create_object/spawn_box
// objects): stable, readable colors per material id instead of the magenta diagnostic.
const std::array<VoxelMaterialDefinition, 256>& default_palette() {
    static const std::array<VoxelMaterialDefinition, 256> palette = [] {
        std::array<VoxelMaterialDefinition, 256> result{};
        for (std::size_t id = 0; id < result.size(); ++id) {
            const float hue = static_cast<float>((id * 47U) % 360U) / 60.0F;
            const float x = 1.0F - std::abs(std::fmod(hue, 2.0F) - 1.0F);
            Float3 rgb{};
            if (hue < 1) rgb = {1, x, 0};
            else if (hue < 2) rgb = {x, 1, 0};
            else if (hue < 3) rgb = {0, 1, x};
            else if (hue < 4) rgb = {0, x, 1};
            else if (hue < 5) rgb = {x, 0, 1};
            else rgb = {1, 0, x};
            result[id].name = "default";
            result[id].baseColor = {0.25F + 0.5F * rgb.x, 0.25F + 0.5F * rgb.y, 0.25F + 0.5F * rgb.z, 1.0F};
            result[id].roughness = 0.8F;
        }
        return result;
    }();
    return palette;
}


struct RacingProjected {
    float x{},y{},depth{},pixelsPerMetre{};
};
struct RacingCamera {
    Float3 eye{},right{},up{},forward{};
    float tanHalf{},aspect{},nearPlane{},farPlane{};
    std::uint32_t width{},height{};
};
RacingCamera racing_camera(const render::PolygonCamera& camera,std::uint32_t width,std::uint32_t height) {
    RacingCamera c{};
    c.eye=camera.position;c.forward=normalize(subtract(camera.target,camera.position));
    c.right=normalize(cross_racing(c.forward,camera.up));
    if(length_squared(c.right)<1.e-8F)c.right={1,0,0};
    c.up=normalize(cross_racing(c.right,c.forward));
    c.tanHalf=std::tan(camera.verticalFieldOfViewRadians*0.5F);
    c.aspect=float(width)/float(height);c.nearPlane=camera.nearPlane;c.farPlane=camera.farPlane;
    c.width=width;c.height=height;
    return c;
}
bool project_racing(const RacingCamera& c,Float3 point,RacingProjected& p){
    const Float3 rel=subtract(point,c.eye);
    const float z=dot(rel,c.forward);
    if(!(z>c.nearPlane&&z<c.farPlane)||!std::isfinite(z)||c.tanHalf<=0)return false;
    p.x=(0.5F+0.5F*dot(rel,c.right)/(z*c.tanHalf*c.aspect))*float(c.width);
    p.y=(0.5F-0.5F*dot(rel,c.up)/(z*c.tanHalf))*float(c.height);
    p.depth=(z-c.nearPlane)/(c.farPlane-c.nearPlane);
    p.pixelsPerMetre=float(c.height)/(2*c.tanHalf*z);
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.depth);
}
void draw_racing_effects(std::span<const RacingVisualEffect> effects,
                         const render::PolygonCamera& camera,
                         const render::PolygonRenderTarget& depth,
                         std::span<std::uint8_t> rgba){
    if(effects.empty()||!depth.valid()||rgba.size()!=std::size_t(depth.width)*depth.height*4)return;
    const auto c=racing_camera(camera,depth.width,depth.height);
    auto paint=[&](int x,int y,float z,float alpha,std::array<float,3> rgb){
        if(x<0||y<0||x>=int(depth.width)||y>=int(depth.height)||alpha<=0)return;
        const std::size_t at=std::size_t(y)*depth.width+std::size_t(x);
        // The depth buffer uses the same normalized linear camera-space z.
        if(z>depth.depth[at]+0.00008F)return;
        alpha=std::clamp(alpha,0.0F,0.85F);
        for(std::size_t channel=0;channel<3;++channel){
            const float mixed=float(rgba[at*4+channel])*(1-alpha)+rgb[channel]*alpha;
            rgba[at*4+channel]=std::uint8_t(std::clamp(mixed,0.0F,255.0F));
        }
        rgba[at*4+3]=255;
    };
    for(const RacingVisualEffect& fx:effects){
        if(fx.lifetime<=0||fx.age>=fx.lifetime||fx.opacity<=0)continue;
        RacingProjected a{},b{};
        if(!project_racing(c,fx.a,a))continue;
        const float life=std::clamp(1.0F-fx.age/fx.lifetime,0.0F,1.0F);
        if(fx.kind==RacingVisualKind::SkidMark){
            if(!project_racing(c,fx.b,b))continue;
            const float ra=std::clamp(fx.radius*a.pixelsPerMetre*0.5F,0.65F,20.0F);
            const float rb=std::clamp(fx.radius*b.pixelsPerMetre*0.5F,0.65F,20.0F);
            const float radius=(std::max)(ra,rb);
            const float dx=b.x-a.x,dy=b.y-a.y,length2=dx*dx+dy*dy;
            if(length2>1000000.0F)continue;
            const int minX=(std::max)(0,int(std::floor(std::min(a.x,b.x)-radius-1)));
            const int maxX=(std::min)(int(depth.width)-1,int(std::ceil(std::max(a.x,b.x)+radius+1)));
            const int minY=(std::max)(0,int(std::floor(std::min(a.y,b.y)-radius-1)));
            const int maxY=(std::min)(int(depth.height)-1,int(std::ceil(std::max(a.y,b.y)+radius+1)));
            for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x){
                const float t=length2>1.e-5F?std::clamp(((x+0.5F-a.x)*dx+(y+0.5F-a.y)*dy)/length2,0.0F,1.0F):0.0F;
                const float px=a.x+t*dx,py=a.y+t*dy;
                const float r=ra+t*(rb-ra);
                const float d2=(x+0.5F-px)*(x+0.5F-px)+(y+0.5F-py)*(y+0.5F-py);
                if(d2>r*r)continue;
                paint(x,y,a.depth+t*(b.depth-a.depth),fx.opacity*life,{24,26,28});
            }
        }else{
            const float growing=fx.radius*(1.0F+fx.age*1.4F);
            const float radius=std::clamp(growing*a.pixelsPerMetre,1.0F,35.0F);
            const int minX=(std::max)(0,int(std::floor(a.x-radius)));
            const int maxX=(std::min)(int(depth.width)-1,int(std::ceil(a.x+radius)));
            const int minY=(std::max)(0,int(std::floor(a.y-radius)));
            const int maxY=(std::min)(int(depth.height)-1,int(std::ceil(a.y+radius)));
            const std::array<float,3> color=fx.kind==RacingVisualKind::Dust?
               std::array<float,3>{163,144,113}:std::array<float,3>{175,179,182};
            for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x){
                const float u=(x+0.5F-a.x)/radius,v=(y+0.5F-a.y)/radius;
                const float rr=u*u+v*v;
                if(rr>=1.0F)continue;
                const float falloff=(1-rr)*(1-rr);
                paint(x,y,a.depth,fx.opacity*life*falloff,color);
            }
        }
    }
}


// HDR participating-medium pass. All distances below are world metres;
// PolygonRenderTarget depth is normalized linear camera-forward depth, not
// Euclidean distance and not voxel units.
void apply_underwater_volume_hdr(const PlayerRenderView& view,
                                 const RenderEnvironment& environment,
                                 render::PolygonRenderTarget& target) {
    if(!view.ocean||!view.waterVolume||!target.valid()||
       !valid_water_volume_settings(*view.waterVolume))return;
    const auto camera=racing_camera(view.camera,target.width,target.height);
    if(!(camera.tanHalf>0)||!std::isfinite(camera.tanHalf))return;
    const auto eye=sample_water_volume(*view.ocean,*view.waterVolume,camera.eye,view.oceanTime);
    const Float3 sky={
       std::max(0.F,environment.skyColor.x*1.3F+environment.sunIntensity*.09F),
       std::max(0.F,environment.skyColor.y*1.3F+environment.sunIntensity*.09F),
       std::max(0.F,environment.skyColor.z*1.3F+environment.sunIntensity*.09F)};
    for(std::uint32_t py=0;py<target.height;++py){
      for(std::uint32_t px=0;px<target.width;++px){
        const std::size_t index=std::size_t(py)*target.width+px;
        const float x=(2.F*(px+.5F)/target.width-1.F)*camera.tanHalf*camera.aspect;
        const float y=(1.F-2.F*(py+.5F)/target.height)*camera.tanHalf;
        const Float3 dir=normalize(add(camera.forward,
                add(multiply(camera.right,x),multiply(camera.up,y))));
        const float cosine=dot(dir,camera.forward);
        if(cosine<=.02F)continue;
        const float normalizedDepth=target.depth[index];
        const bool hit=normalizedDepth<.999999F;
        const float forwardDepth=hit?
           camera.nearPlane+std::clamp(normalizedDepth,0.F,1.F)*
                   (camera.farPlane-camera.nearPlane):
           std::min(85.F,camera.farPlane);
        const float rayLength=std::min(160.F,forwardDepth/cosine);
        const auto medium=water_ray_segment(*view.ocean,camera.eye,dir,rayLength,view.oceanTime);
        if(medium.lengthMeters<=.001F)continue;
        const Float3 start=add(camera.eye,multiply(dir,medium.startMeters));
        const Float3 end=add(camera.eye,multiply(dir,rayLength));
        const auto entry=sample_water_volume(*view.ocean,*view.waterVolume,start,view.oceanTime);
        const auto atEnd=sample_water_volume(*view.ocean,*view.waterVolume,end,view.oceanTime);
        const bool objectUnderwater=hit&&atEnd.submerged;
        const auto& old=target.hdrColor[index];
        UnderwaterOpticsInput input{};
        input.objectRadiance=hit?Float3{old.x,old.y,old.z}:
            (medium.cameraSubmerged&&medium.hitsSurface?
                Float3{old.x,old.y,old.z}:Float3{});
        input.objectSubmerged=objectUnderwater;
        input.objectDepthMeters=atEnd.depthBelowSurface;
        input.eyeDepthMeters=entry.depthBelowSurface;
        input.viewPathMeters=medium.lengthMeters;
        input.rayDirectionZ=dir.z;
        input.ambientSurfaceLight=sky;
        if(objectUnderwater)for(const auto& lamp:view.underwaterLamps){
          const auto illumination=evaluate_underwater_lamp(lamp,entry,end);
          input.lampIrradiance=add(input.lampIrradiance,illumination);
        }
        Float3 shaded=shade_underwater_radiance(entry,input);
        // Approximate in-scattering from visible headlamp cones. The same
        // lamp spectrum is extinguished on light-to-particle and particle-to-eye.
        // Four stratified analytic samples: no stochastic frame flicker.
        const float step=std::min(medium.lengthMeters,24.F)/4.F;
        for(int i=0;i<4&&step>0;++i){
          const float t=medium.startMeters+(float(i)+.5F)*step;
          const Float3 p=add(camera.eye,multiply(dir,t));
          const auto media=sample_water_volume(*view.ocean,*view.waterVolume,p,view.oceanTime);
          Float3 lampLight{};
          for(const auto& lamp:view.underwaterLamps)
             lampLight=add(lampLight,evaluate_underwater_lamp(lamp,media,p));
          const Float3 tr=water_transmittance(media,std::max(0.F,t-medium.startMeters));
          constexpr float phase=.12F;
          shaded.x+=lampLight.x*media.scatteringRGB.x*tr.x*phase*step;
          shaded.y+=lampLight.y*media.scatteringRGB.y*tr.y*phase*step;
          shaded.z+=lampLight.z*media.scatteringRGB.z*tr.z*phase*step;
        }
        // Water-to-air interface: Fresnel transmission, including total
        // internal reflection. Screen-space ray bending is not attempted:
        // geometry refractive reray is a separate S2 follow-up.
        if(medium.cameraSubmerged&&medium.hitsSurface){
          const auto p=add(camera.eye,multiply(dir,medium.lengthMeters));
          const auto surface=view.ocean->sample(p.x,p.y,view.oceanTime);
          const float reflectance=water_to_air_fresnel(dot(dir,surface.normal));
          shaded=add(multiply(shaded,1.F-reflectance),
                     multiply(Float3{.012F,.045F,.075F},reflectance));
        }
        target.hdrColor[index]={
          std::clamp(shaded.x,0.F,64.F),std::clamp(shaded.y,0.F,64.F),
          std::clamp(shaded.z,0.F,64.F),old.w};
      }
    }
}

void draw_ocean_surface(const OceanField* ocean,double seconds,
                       const render::PolygonCamera& camera,
                       const render::PolygonRenderTarget& depth,
                       std::span<std::uint8_t> rgba) {
    if(!ocean||!depth.valid()||rgba.size()!=std::size_t(depth.width)*depth.height*4U)return;
    const Float3 eye=camera.position;
    const Float3 forward=normalize(subtract(camera.target,camera.position));
    const Float3 right=normalize(cross_racing(forward,camera.up));
    const Float3 up=normalize(cross_racing(right,forward));
    const float tanHalf=std::tan(camera.verticalFieldOfViewRadians*.5F);
    if(!std::isfinite(tanHalf)||tanHalf<=0)return;
    const float aspect=float(depth.width)/float(depth.height);
    const auto sun=normalize(Float3{-.32F,-.27F,.91F});
    // Half-resolution shade, but scene-depth test every individual output pixel.
    // This keeps the reference CPU player responsive without a GPU dependency.
    for(unsigned py=0;py<depth.height;py+=2U){
        for(unsigned px=0;px<depth.width;px+=2U){
            const float x=2.F*(float(px)+1.F)/float(depth.width)-1.F;
            const float y=1.F-2.F*(float(py)+1.F)/float(depth.height);
            const auto dir=normalize(add(forward,add(multiply(right,x*tanHalf*aspect),multiply(up,y*tanHalf))));
            if(dir.z>=-.005F)continue;
            float t=-eye.z/dir.z;
            if(!(t>0)||t>camera.farPlane+150.F)continue;
            OceanSample sample{};
            // Fixed-point waterline iteration; no separate shader wave field.
            for(unsigned iteration=0;iteration<5;++iteration){
                const Float3 point=add(eye,multiply(dir,t));
                sample=ocean->sample(point.x,point.y,seconds);
                const float candidate=(sample.height-eye.z)/dir.z;
                if(!std::isfinite(candidate)||candidate<=0)break;
                t=std::clamp(.5F*t+.5F*candidate,0.F,camera.farPlane+150.F);
            }
            if(t<=camera.nearPlane||t>camera.farPlane)continue;
            const auto point=add(eye,multiply(dir,t));
            sample=ocean->sample(point.x,point.y,seconds);
            if(std::abs(point.z-sample.height)>2.F)continue;
            const float z=dot(subtract(point,eye),forward);
            if(z<=camera.nearPlane||z>=camera.farPlane)continue;
            const float normalizedDepth=(z-camera.nearPlane)/(camera.farPlane-camera.nearPlane);
            const float diffuse=std::clamp(dot(sample.normal,sun),0.F,1.F);
            const float glint=std::pow(std::clamp(dot(sample.normal,normalize(subtract(sun,dir))),0.F,1.F),36.F);
            const float fresnel=std::pow(1.F-std::clamp(-dot(dir,sample.normal),0.F,1.F),5.F);
            const float foam=sample.foam;
            const float color[3]={
                std::clamp(12.F+48.F*diffuse+180.F*glint+115.F*foam,0.F,255.F),
                std::clamp(65.F+55.F*diffuse+150.F*glint+120.F*foam,0.F,255.F),
                std::clamp(100.F+55.F*diffuse+130.F*glint+112.F*foam,0.F,255.F)};
            const float opacity=std::clamp(.90F-.20F*fresnel,0.F,1.F);
            for(unsigned oy=0;oy<2U&&py+oy<depth.height;++oy)
              for(unsigned ox=0;ox<2U&&px+ox<depth.width;++ox){
                const std::size_t at=std::size_t(py+oy)*depth.width+(px+ox);
                if(normalizedDepth>depth.depth[at]+.0001F)continue;
                for(unsigned channel=0;channel<3;++channel){
                    const float mixed=float(rgba[4*at+channel])*(1.F-opacity)+color[channel]*opacity;
                    rgba[4*at+channel]=std::uint8_t(std::clamp(mixed,0.F,255.F));
                }
                rgba[4*at+3]=255;
              }
        }
    }
}

class CpuPlayerRenderer final : public IPlayerRenderer {
public:
    CpuPlayerRenderer(IFrameBlitter* blitter, CpuPlayerRendererOptions options)
        : blitter_(blitter), options_(options) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "cpu"; }

    bool resize(std::uint32_t width, std::uint32_t height, std::string* error) override {
        if (width == 0U || height == 0U || width > 8192U || height > 8192U) {
            if (error) *error = "CPU render size must be between 1x1 and 8192x8192";
            return false;
        }
        target_.resize(width, height);
        frame_.width = width;
        frame_.height = height;
        frame_.pixels.assign(static_cast<std::size_t>(width) * height * 4U, 0U);
        return true;
    }

    bool render(const PlayerRenderView& view, std::string* error) override {
        if (!target_.valid()) {
            if (error) *error = "renderer has no size; call resize() first";
            return false;
        }
        stats_ = {};
        const auto start = Clock::now();

        // The reference tracer works in voxel-index units (1 unit = 1 voxel, rigid
        // transforms). Pick the scene's voxel size from the first voxel object and convert
        // every metre-space transform and the camera with it.
        float voxelSize = 0.0F;
        for (const GameRenderObject& object : view.objects) {
            if (object.enabled && object.kind == GameGeometryKind::Voxel && object.voxels != nullptr &&
                object.voxelSizeMeters > 0.0F) {
                voxelSize = object.voxelSizeMeters;
                break;
            }
        }
        if (!(voxelSize > 0.0F)) voxelSize = kDefaultMetersPerVoxel;
        const float toUnits = 1.0F / voxelSize;
        ++polygonFrame_;
        if(polygonScale_ != toUnits){polygonCache_.clear();polygonRenderCache_.clear();polygonScale_=toUnits;}

        voxels_.clear();
        polygons_.clear();
        for (const GameRenderObject& object : view.objects) {
            if (!object.enabled) continue;
            RigidTransform transform = object.transform;
            transform.position = multiply(transform.position, toUnits);
            if (object.kind == GameGeometryKind::Voxel && object.voxels != nullptr) {
                std::span<const VoxelMaterialDefinition> materials = object.materials;
                if (materials.empty()) materials = default_palette();
                voxels_.push_back({object.id, object.voxels, transform, materials, true,
                    multiply(object.voxelScale,object.voxelSizeMeters*toUnits)});
            } else if (object.kind == GameGeometryKind::Polygon && object.polygon != nullptr) {
                const CookedPolygonAsset* scaled = scaled_polygon(*object.polygon, toUnits);
                std::span<const render::PolygonLodLevel> lods;
                if(!object.polygonLods.empty()){
                    auto& binding=polygonLods_[object.id]; binding.lastFrame=polygonFrame_; binding.levels.clear();
                    for(const auto& level:object.polygonLods)
                        binding.levels.push_back({level.asset ? scaled_polygon(*level.asset,toUnits) : nullptr,level.minimumProjectedDiameterPixels});
                    lods=binding.levels;
                }
                polygons_.push_back({object.id, scaled, transform, lods, {1, 1, 1, 1}, true});
            }
        }
        prune_polygon_cache();
        stats_.voxelInstances = voxels_.size();
        stats_.polygonInstances = polygons_.size();

        render::PolygonCamera camera = view.camera;
        camera.position = multiply(camera.position, toUnits);
        camera.target = multiply(camera.target, toUnits);
        camera.nearPlane *= toUnits;
        camera.farPlane *= toUnits;

        const RenderEnvironment environment = apply_cpu_render_quality(view.environment, options_.quality);
        if (!environment.validate(error))
          return false;
        clear_background(environment);
        std::unique_ptr<render::SceneLighting> sceneLighting;
        if (!environment.lights.empty() || environment.pathTraceBounces > 0 || render::scene_uses_camera_transport(voxels_,polygons_))
          sceneLighting = std::make_unique<render::SceneLighting>(
              voxels_, polygons_, environment, voxelSize);
        render::ReferenceVoxelRenderer voxelRenderer;
        voxelRenderer.sceneLighting = sceneLighting.get();
        voxelRenderer.metersPerVoxel = voxelSize;
        voxelRenderer.threadCount = options_.threadCount;
        voxelRenderer.radianceCascades.threadCount = options_.threadCount;
        // Caustics: only at Reference quality and only when something lets light through.
        std::optional<render::CausticMap> caustics;
        std::vector<render::OpticalPolygonInstance> optical;
        if (options_.quality == CpuRenderQuality::Reference && options_.causticResolution > 0U) {
            bool refractive = false;
            for (const auto& v : voxels_)
                for (const auto& m : v.materials) refractive = refractive || material_is_refractive(m);
            for (const auto& p : polygons_)
                if (p.asset) for (const auto& m : p.asset->materials) refractive = refractive || material_is_refractive(m);
            if (refractive) {
                for (const auto& p : polygons_) optical.push_back({p.objectId, p.asset, p.transform, p.visible});
                const render::OpticalScene opticalScene(voxels_, optical, voxelSize);
                render::CausticMapSettings causticSettings;
                causticSettings.resolution = options_.causticResolution;
                caustics = render::build_caustic_map(
                    opticalScene, {normalize(environment.sunDirection), environment.shadowSoftnessRadians}, causticSettings);
                voxelRenderer.caustics = &*caustics;
                voxelRenderer.sunOccluders = polygons_;
            }
        }
        // Cross-geometry sun visibility is part of the new lighting path; legacy
        // scenes (no lights, no path budget) keep their exact previous frames.
        if (sceneLighting) voxelRenderer.sunOccluders = polygons_;
        voxelRenderer.occlusionPolygons = polygons_;   // AO rays see voxels and polygons alike
        const auto voxelStats = voxelRenderer.render(voxels_, camera, environment, target_, true);
        stats_.tracedRays = voxelStats.tracedRays;
        stats_.hitRays = voxelStats.hitRays;
        if (!polygons_.empty()) {
            render::PolygonRenderOptions polygonOptions;
            polygonOptions.sceneLighting = sceneLighting.get();
            polygonOptions.preserveExistingDepth = true;
            polygonOptions.enableFrustumCulling = view.polygonFrustumCulling;
            polygonOptions.lodBias = view.polygonLodBias;
            polygonOptions.cache = &polygonRenderCache_;
            polygonOptions.surfaceDetail=view.surfaceDetail;
            polygonOptions.metersPerUnit = voxelSize;
            std::optional<render::HybridOcclusionScene> occlusion;
            if (environment.ambientOcclusionSamples > 0U && environment.globalIlluminationMode != GlobalIlluminationMode::Off) {
                occlusion.emplace(voxels_, polygons_);
                polygonOptions.occlusionRay = occlusion->ray();
            }
            stats_.polygons = render::ReferencePolygonRenderer{}.render(polygons_, camera, environment, target_, polygonOptions);
        } else {
            polygonRenderCache_.clear();
        }
        if(view.fogVolumes.empty()) {
            // Underwater extinction supersedes ordinary distance fog. Running
            // both would double-attenuate every pixel of the water column.
            if(!(view.ocean&&view.waterVolume))
              (void)render::apply_distance_fog(target_, camera, environment, voxelSize);
        } else {
            // Assets stay in metre space. Camera/depth are converted back from
            // the voxel-unit surface pass for this shared HDR integration pass.
            std::vector<render::CpuFogVolume> fogVolumes(view.fogVolumes.begin(),view.fogVolumes.end());
            for(auto& v:fogVolumes){
                v.transform.position=multiply(v.transform.position,toUnits);
                v.scale=multiply(v.scale,toUnits);
            }
            auto fogSettings=view.fogSettings;
            fogSettings.sceneLighting = sceneLighting.get();
            if(!fogSettings.densityCache)fogSettings.densityCache=&fogDensityCache_;
            if(!fogSettings.lightingCache)fogSettings.lightingCache=&fogLightingCache_;
            std::vector<render::CpuFogLight> lights(fogSettings.lights.begin(),fogSettings.lights.end());
            for(auto& light:lights)light.position=multiply(light.position,toUnits);
            fogSettings.lights=lights;
            std::optional<render::HybridOcclusionScene> fogOcclusion;
            if(fogSettings.scattering&&!fogSettings.sceneVisibility){
                fogOcclusion.emplace(voxels_,polygons_);
                fogSettings.sceneVisibility=[&](Float3 p,Float3 d,float maximum){
                    return fogOcclusion->nearest(add(p,multiply(d,0.001F*toUnits)),d,maximum)?0.0F:1.0F;
                };
            }
            if(!render::apply_cpu_fog(target_,camera,environment,voxelSize,fogVolumes,
                                     fogSettings,&stats_.fog,error))return false;
        }
        apply_underwater_volume_hdr(view,environment,target_);
        if (sceneLighting) (void)render::apply_cpu_bloom(target_, environment);
        stats_.renderMilliseconds = milliseconds_since(start);

        const auto resolveStart = Clock::now();
        const float exposure = options_.exposure * (environment.exposure > 0.0F ? environment.exposure : 1.0F);
        if (!render::resolve_polygon_render_rgba8(target_, exposure, environment.tonemapOperator,
                                                  frame_.pixels, error)) return false;
        draw_ocean_surface(view.ocean,view.oceanTime,view.camera,target_,frame_.pixels);
        draw_racing_effects(view.racingEffects,view.camera,target_,frame_.pixels);
        if(view.prototypeHud)draw_prototype_hud(frame_.pixels,frame_.width,frame_.height,*view.prototypeHud);
        stats_.resolveMilliseconds = milliseconds_since(resolveStart);
        rendered_ = true;
        return true;
    }

    bool present(std::string* error) override {
        if (!rendered_) {
            if (error) *error = "nothing rendered yet";
            return false;
        }
        const auto start = Clock::now();
        const bool ok = blitter_ == nullptr || blitter_->blit(frame_, error);
        stats_.presentMilliseconds = milliseconds_since(start);
        return ok;
    }

    [[nodiscard]] const Rgba8Image* readback() const noexcept override { return rendered_ ? &frame_ : nullptr; }
    [[nodiscard]] PlayerRenderStats last_stats() const noexcept override { return stats_; }

private:
    render::CpuFogDensityCache fogDensityCache_;
    render::CpuFogLightingCache fogLightingCache_;
    struct ScaledPolygon {
        CookedPolygonAsset asset;
        std::uint64_t lastFrame{};
    };

    const CookedPolygonAsset* scaled_polygon(const CookedPolygonAsset& source, float scale) {
        // GameWorld supplies immutable cooked assets. A content change publishes
        // a new hash; unhashed caller assets are fingerprinted here each frame.
        const auto hash=source.contentHash ? source.contentHash : polygon_asset_content_hash(source);
        auto [it,inserted]=polygonCache_.try_emplace(hash);
        auto& entry=it->second;
        if (inserted) {
            if(!validate_polygon_asset(source)){polygonCache_.erase(it);return nullptr;}
            ++stats_.scaledPolygonCopies;
            entry.asset = source;
            for (PolygonVertex& vertex : entry.asset.vertices) vertex.position = multiply(vertex.position, scale);
            entry.asset.bounds.minimum = multiply(entry.asset.bounds.minimum, scale);
            entry.asset.bounds.maximum = multiply(entry.asset.bounds.maximum, scale);
            // Preserve hash validation after converting vertices into voxel units.
            entry.asset.contentHash = polygon_asset_content_hash(entry.asset);
        }
        entry.lastFrame=polygonFrame_;
        return &entry.asset;
    }

    void prune_polygon_cache() {
        for (auto it = polygonCache_.begin(); it != polygonCache_.end();) {
            it = it->second.lastFrame==polygonFrame_ ? std::next(it) : polygonCache_.erase(it);
        }
        stats_.scaledPolygonAssets=polygonCache_.size();
        for(auto it=polygonLods_.begin();it!=polygonLods_.end();)
            it=it->second.lastFrame==polygonFrame_ ? std::next(it) : polygonLods_.erase(it);
    }

    void clear_background(const RenderEnvironment& environment) {
        target_.clear({environment.skyColor.x, environment.skyColor.y, environment.skyColor.z, 1.0F}, 1.0F);
        if (!options_.skyGradient) return;
        for (std::uint32_t y = 0; y < target_.height; ++y) {
            const float t = static_cast<float>(y) / static_cast<float>((std::max)(1U, target_.height - 1U));
            const float zenith = 1.6F - 0.9F * t; // brighter toward the top
            const Float4 color{environment.skyColor.x * zenith, environment.skyColor.y * zenith,
                               environment.skyColor.z * (zenith + 0.15F), 1.0F};
            std::fill_n(target_.hdrColor.begin() + static_cast<std::ptrdiff_t>(y) * target_.width,
                        target_.width, color);
        }
        target_.sync_background_pass();
    }

    IFrameBlitter* blitter_{};
    CpuPlayerRendererOptions options_{};
    render::PolygonRenderTarget target_;
    Rgba8Image frame_;
    bool rendered_{};
    PlayerRenderStats stats_{};
    std::vector<render::VoxelReferenceInstance> voxels_;
    std::vector<render::PolygonRenderInstance> polygons_;
    std::unordered_map<std::uint64_t, ScaledPolygon> polygonCache_;
    struct LodBinding { std::vector<render::PolygonLodLevel> levels; std::uint64_t lastFrame{}; };
    std::unordered_map<GameObjectId,LodBinding> polygonLods_;
    render::PolygonRenderCache polygonRenderCache_;
    std::uint64_t polygonFrame_{};
    float polygonScale_{};
};

} // namespace

std::string_view cpu_render_quality_name(CpuRenderQuality quality) noexcept {
    switch (quality) {
    case CpuRenderQuality::Fast: return "fast";
    case CpuRenderQuality::Balanced: return "balanced";
    case CpuRenderQuality::Reference: return "reference";
    }
    return "unknown";
}

bool parse_cpu_render_quality(std::string_view text, CpuRenderQuality& quality) noexcept {
    for (CpuRenderQuality candidate : {CpuRenderQuality::Fast, CpuRenderQuality::Balanced,
                                       CpuRenderQuality::Reference}) {
        if (text == cpu_render_quality_name(candidate)) {
            quality = candidate;
            return true;
        }
    }
    return false;
}

RenderEnvironment apply_cpu_render_quality(RenderEnvironment environment, CpuRenderQuality quality) noexcept {
    if (quality == CpuRenderQuality::Reference) return environment;
    if (environment.shadowMode != ShadowMode::Off) environment.shadowMode = ShadowMode::Hard;
    if (quality == CpuRenderQuality::Fast) {
      environment.pathTraceBounces = 0;
      environment.localLightSamples =
          (std::min)(environment.localLightSamples, 2U);
      if (environment.globalIlluminationMode != GlobalIlluminationMode::Off)
            environment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
    } else {
        environment.globalIlluminationSamples = (std::min)(environment.globalIlluminationSamples, 2U);
        environment.pathTraceBounces =
            (std::min)(environment.pathTraceBounces, 2U);
        environment.pathTraceSamples =
            (std::min)(environment.pathTraceSamples, 2U);
        environment.localLightSamples =
            (std::min)(environment.localLightSamples, 2U);
    }
    return environment;
}

std::unique_ptr<IPlayerRenderer> make_cpu_player_renderer(IFrameBlitter* blitter, CpuPlayerRendererOptions options) {
    return std::make_unique<CpuPlayerRenderer>(blitter, options);
}

bool MemoryFrameBlitter::blit(const Rgba8Image& frame, std::string* error) {
    if (!frame.valid()) {
        if (error) *error = "invalid frame";
        return false;
    }
    last_ = frame;
    ++frames_;
    return true;
}

} // namespace dve::player
