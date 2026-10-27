// T6: apples-to-apples CPU REFERENCE rendered-image difference suite.
// Bakes authentic DVE images, encodes BC5 normals / optional Basis bc7f base
// color, decodes native blocks back to pixels, and feeds precisely the same
// rasterizer/camera/materials for reference and encoded variants.
//
// This DOES NOT exercise GPU BC5/BC7 sampling, trilinear compressed mips, or
// production shader pipelines. Those require a separate Vulkan capture suite.
#include "dve/editor_polygon.hpp"
#include "dve/render/material_texture_bake.hpp"
#include "dve/render/normal_bc5_codec.hpp"
#include "dve/render/normal_texture_binding.hpp"
#include "dve/render/texture_residency.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/rgba8_png.hpp"
#ifdef DVE_T6_WITH_BC7
#include "dve/render/bc7f_texture_encoder.hpp"
#include "basisu_transcoder_internal.h"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::render;
namespace {
void check(bool ok,const std::string& reason) {
    if(!ok)throw std::runtime_error(reason);
}
std::uint8_t byte_normal(double v) {
    return static_cast<std::uint8_t>(std::clamp((v*0.5+0.5)*255.0+0.5,0.0,255.0));
}
CookedPolygonAsset authored_scene(unsigned pattern) {
    auto asset=editor::make_editor_polygon_plane(1,1);
    asset.contentHash=0U;
    for(auto& v:asset.vertices)v.texcoord1=v.texcoord;
    PolygonImage norm,color;
    norm.name="normal-authored";color.name="color-authored";
    norm.mimeType=color.mimeType="image/raw";
    norm.width=norm.height=color.width=color.height=64U;
    norm.rgba8.resize(64U*64U*4U); color.rgba8.resize(norm.rgba8.size());
    for(unsigned y=0U;y<64U;++y) for(unsigned x=0U;x<64U;++x) {
        const double fx=static_cast<double>(x),fy=static_cast<double>(y);
        const double nx=pattern==0U?0.0:pattern==1U?
            0.24*std::sin(fx*.19):0.32*std::sin(fx*.46+fy*.13);
        const double ny=pattern==0U?0.0:pattern==1U?
            0.23*std::cos(fy*.18):0.28*std::cos(fx*.12+fy*.41);
        const double nz=std::sqrt(std::max(0.0,1.0-nx*nx-ny*ny));
        const auto i=(y*64U+x)*4U;
        norm.rgba8[i]=byte_normal(nx);
        norm.rgba8[i+1U]=byte_normal(ny);
        norm.rgba8[i+2U]=byte_normal(nz);
        norm.rgba8[i+3U]=255U;
        const double checker=((x/8U+y/8U)&1U)?0.78:1.0;
        color.rgba8[i]=static_cast<std::uint8_t>(std::clamp(
            (55.0+static_cast<double>(x)*2.5)*checker,0.0,255.0));
        color.rgba8[i+1U]=static_cast<std::uint8_t>(std::clamp(
            (65.0+static_cast<double>(y)*2.3)*checker,0.0,255.0));
        color.rgba8[i+2U]=static_cast<std::uint8_t>(std::clamp(
            (150.0+25.0*std::sin(fx*.14))*checker,0.0,255.0));
        color.rgba8[i+3U]=255U;
    }
    asset.images={norm,color};
    asset.samplers={{}};
    asset.textures={{"normal",0U,0U},{"base",1U,0U}};
    asset.materialBindings[0].normal={0U,0U,PolygonTextureColorSpace::Linear};
    asset.materialBindings[0].baseColor={1U,0U,PolygonTextureColorSpace::Srgb};
    asset.materials[0].baseColor={1.0F,1.0F,1.0F,1.0F};
    asset.materials[0].roughness=.29F;
    asset.materials[0].metallic=.15F;
    return asset;
}
CookedPolygonAsset baked_scene(unsigned pattern) {
    const auto authored=authored_scene(pattern);
    MaterialTextureBakeSettings options{};
    options.resolution=64U;
    options.samplesPerAxis=2U;
    options.paddingPixels=3U;
    options.maps=static_cast<std::uint32_t>(MaterialBakeMap::Normal)|
                 static_cast<std::uint32_t>(MaterialBakeMap::BaseColor);
    const auto baked=bake_material_textures(authored,options);
    check(baked.success,"T6 real material bake failed: "+baked.error);
    check(baked.coveredPixels>0U && baked.surfaceSamples>0U,"T6 bake sampled no geometry");
    auto scene=authored;
    scene.images[0]=baked.images.at(MaterialBakeMap::Normal);
    scene.images[1]=baked.images.at(MaterialBakeMap::BaseColor);
    scene.contentHash=0U;
    return scene;
}
PolygonImage unpack_bc5_image(const PolygonImage& source,
                             std::span<const std::byte> blocks) {
    const auto expected=rhi::texture_row_bytes(rhi::TextureFormat::BC5RGUnorm,source.width)*
                        rhi::texture_rows(rhi::TextureFormat::BC5RGUnorm,source.height);
    check(blocks.size()==expected,"T6 BC5 decompression payload size");
    PolygonImage result=source;
    const auto tiles=(source.width+3U)/4U;
    for(unsigned y=0;y<source.height;++y)for(unsigned x=0;x<source.width;++x) {
        const auto off=(static_cast<std::size_t>(y/4U)*tiles+x/4U)*16U;
        const auto texels=decode_bc5_rg_block(
            std::span<const std::byte,16>(blocks.data()+off,16U));
        const auto rg=texels[(y%4U)*4U+x%4U];
        const auto n=decode_bc5_normal_sample(
            static_cast<float>(rg[0])/255.0F,static_cast<float>(rg[1])/255.0F);
        const auto pixel=(static_cast<std::size_t>(y)*source.width+x)*4U;
        for(std::size_t c=0;c<3U;++c)result.rgba8[pixel+c]=
            static_cast<std::uint8_t>(std::clamp(n[c]*255.0F+0.5F,0.0F,255.0F));
    }
    return result;
}
PolygonImage compressed_normal(const PolygonImage& source) {
    const auto mips=generate_semantic_texture_mips(source,TextureSemantic::Normal);
    std::vector<std::vector<std::byte>> blocks;
    std::string error;
    check(encode_bc5_normal_mips(mips,blocks,&error),"T6 BC5 encode: "+error);
    check(blocks.size()==mips.levels.size(),"T6 BC5 lost mip levels");
    return unpack_bc5_image(source,blocks.front());
}
#ifdef DVE_T6_WITH_BC7
PolygonImage compressed_color(const PolygonImage& source) {
    const auto mips=generate_semantic_texture_mips(source,TextureSemantic::BaseColor);
    std::vector<std::vector<std::byte>> blocks;
    std::string error;
    auto encoder=make_basis_bc7f_encoder(Bc7fQuality::Analytical);
    check(encoder(mips,blocks,&error),"T6 bc7f encoding: "+error);
    check(blocks.size()==mips.levels.size(),"T6 BC7 lost mip levels");
    const auto expected=rhi::texture_row_bytes(rhi::TextureFormat::BC7RGBASrgb,source.width)*
                        rhi::texture_rows(rhi::TextureFormat::BC7RGBASrgb,source.height);
    check(blocks.front().size()==expected,"T6 BC7 first mip size mismatch");
    PolygonImage result=source;
    const auto tilesX=(source.width+3U)/4U;
    std::array<basist::color_rgba,16> rgba{};
    for(unsigned by=0U;by<(source.height+3U)/4U;++by)
        for(unsigned bx=0U;bx<tilesX;++bx) {
            const auto offset=(static_cast<std::size_t>(by)*tilesX+bx)*16U;
            check(basist::bc7u::unpack_bc7(blocks.front().data()+offset,rgba.data()),
                  "T6 Basis BC7 decoder refused bc7f block");
            for(unsigned py=0;py<4U;++py)for(unsigned px=0;px<4U;++px) {
                const auto x=bx*4U+px,y=by*4U+py;
                if(x>=source.width||y>=source.height)continue;
                const auto i=(static_cast<std::size_t>(y)*source.width+x)*4U;
                const auto& p=rgba[py*4U+px];
                result.rgba8[i]=p.r;result.rgba8[i+1U]=p.g;
                result.rgba8[i+2U]=p.b;result.rgba8[i+3U]=p.a;
            }
        }
    return result;
}
#endif

struct Frame {
    PolygonRenderTarget image;
    std::vector<std::uint8_t> rgba;
    PolygonRenderStats stats{};
};
Frame render(const CookedPolygonAsset& scene,bool grazing,
             MaterialMappingMode mapping) {
    auto copy=scene;
    copy.contentHash=0U;
    copy.materialBindings[0].mapping.mappingMode=mapping;
    copy.materialBindings[0].mapping.triplanarScale=1.0F;
    copy.materialBindings[0].mapping.triplanarBlendSharpness=4.0F;
    PolygonCamera camera;
    camera.position=grazing?Float3{0.0F,.36F,2.25F}:Float3{0.0F,1.45F,1.45F};
    camera.target={0.0F,0.0F,0.0F};
    camera.up={1.0F,0.0F,0.0F};
    RenderEnvironment env;
    env.sunDirection={.5F,.9F,.4F};
    env.sunIntensity=3.2F;
    env.globalIlluminationMode=GlobalIlluminationMode::Off;
    env.shadowMode=ShadowMode::Off;
    env.ambientOcclusionSamples=0U;
    Frame frame;
    frame.image.resize(160U,160U);
    frame.image.enable_passes();
    frame.image.clear();
    PolygonRenderInstance instance{1U,&copy,{}, {}, {1,1,1,1},true};
    PolygonRenderOptions opts;
    opts.preserveExistingDepth=false;
    opts.enableFrustumCulling=false;
    opts.metersPerUnit=1.0F;
    ReferencePolygonRenderer renderer;
    frame.stats=renderer.render(std::span(&instance,1U),camera,env,frame.image,opts);
    check(frame.stats.shadedFragments>100U,"T6 renderer drew too few material pixels");
    std::string error;
    check(resolve_polygon_render_rgba8(frame.image,1.0F,frame.rgba,&error),
          "T6 color resolve failed: "+error);
    return frame;
}
struct Metrics {
    unsigned coverage{},changed{};
    double mse{},psnr{},mae{},p95{},maxAbs{};
    double normalMeanDeg{},normalP95Deg{};
    double borderMae{},interiorMae{};
};
double angle_deg(Float3 a,Float3 b) {
    const double la=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);
    const double lb=std::sqrt(b.x*b.x+b.y*b.y+b.z*b.z);
    if(la<1.e-10||lb<1.e-10)return 0.0;
    const double cosValue=std::clamp(
        static_cast<double>(a.x*b.x+a.y*b.y+a.z*b.z)/(la*lb),-1.0,1.0);
    return std::acos(cosValue)*180.0/3.14159265358979323846;
}
Metrics compare(const Frame& ref,const Frame& target) {
    check(ref.image.width==target.image.width&&ref.image.height==target.image.height,
          "T6 mismatched frame dimensions");
    check(ref.rgba.size()==target.rgba.size(),"T6 mismatched frame pixels");
    const auto w=ref.image.width,h=ref.image.height;
    unsigned loX=w,loY=h,hiX=0U,hiY=0U;
    for(unsigned y=0U;y<h;++y)for(unsigned x=0U;x<w;++x) {
        const auto at=static_cast<std::size_t>(y)*w+x;
        check((ref.image.objectId[at]==1U)==(target.image.objectId[at]==1U),
              "T6 compression modified geometry/coverage");
        if(ref.image.objectId[at]==1U) {
            loX=std::min(loX,x);hiX=std::max(hiX,x);
            loY=std::min(loY,y);hiY=std::max(hiY,y);
        }
    }
    Metrics m{};
    std::vector<double> perPixel,angles;
    double sumSquared=0.0,sumAbsolute=0.0;
    double borderSum=0.0,interiorSum=0.0;
    unsigned borderCount=0U,interiorCount=0U;
    for(unsigned y=0U;y<h;++y)for(unsigned x=0U;x<w;++x) {
        const auto i=static_cast<std::size_t>(y)*w+x;
        if(ref.image.objectId[i]!=1U)continue;
        ++m.coverage;
        double pixelErr=0.0;
        for(unsigned c=0;c<3U;++c) {
            const double d=std::abs(static_cast<double>(ref.rgba[i*4U+c])-
                                    static_cast<double>(target.rgba[i*4U+c]))/255.0;
            pixelErr=std::max(pixelErr,d);
            sumAbsolute+=d;
            sumSquared+=d*d;
        }
        if(pixelErr>0.0)++m.changed;
        perPixel.push_back(pixelErr);
        m.maxAbs=std::max(m.maxAbs,pixelErr);
        angles.push_back(angle_deg(ref.image.passes.normal[i],target.image.passes.normal[i]));
        const bool border=x<=loX+7U||x+7U>=hiX||y<=loY+7U||y+7U>=hiY;
        if(border) {borderSum+=pixelErr;++borderCount;}
        else {interiorSum+=pixelErr;++interiorCount;}
    }
    check(m.coverage>100U,"T6 comparison has no visible scene");
    check(ref.image.passes.normal.size()==w*h&&target.image.passes.normal.size()==w*h,
          "T6 missing normal AOV");
    m.mae=sumAbsolute/(static_cast<double>(m.coverage)*3.0);
    m.mse=sumSquared/(static_cast<double>(m.coverage)*3.0);
    m.psnr=m.mse>0.0?-10.0*std::log10(m.mse):99.0;
    std::sort(perPixel.begin(),perPixel.end());
    std::sort(angles.begin(),angles.end());
    auto at95=[](const auto& vec){return vec[std::min(vec.size()-1U,static_cast<std::size_t>(
        std::ceil(vec.size()*0.95)-1.0))];};
    m.p95=at95(perPixel);
    m.normalMeanDeg=std::accumulate(angles.begin(),angles.end(),0.0)/
                    static_cast<double>(angles.size());
    m.normalP95Deg=at95(angles);
    m.borderMae=borderCount?borderSum/borderCount:0.0;
    m.interiorMae=interiorCount?interiorSum/interiorCount:0.0;
    return m;
}
void write_png(const std::filesystem::path& dest,const Frame& frame) {
    std::string error;
    check(write_rgba8_png(dest,frame.image.width,frame.image.height,frame.rgba,&error),
          "T6 cannot write reference/candidate PNG: "+error);
}
void write_diff_png(const std::filesystem::path& path,const Frame& a,const Frame& b) {
    std::vector<std::uint8_t> pixels(a.rgba.size(),0U);
    for(std::size_t i=0;i<a.image.objectId.size();++i) {
        pixels[i*4U+3U]=255U;
        if(a.image.objectId[i]==1U)for(std::size_t c=0;c<3U;++c) {
            const int diff=std::abs(static_cast<int>(a.rgba[i*4U+c])-
                                    static_cast<int>(b.rgba[i*4U+c]));
            pixels[i*4U+c]=static_cast<std::uint8_t>(std::min(255,diff*6));
        }
    }
    std::string error;
    check(write_rgba8_png(path,a.image.width,a.image.height,pixels,&error),
          "T6 cannot write difference PNG: "+error);
}
void log_metrics(std::ostream& out,const std::string& name,const Metrics& m) {
    out<<name<<","<<m.coverage<<","<<m.changed<<","
       <<std::fixed<<std::setprecision(7)<<m.mae<<","<<std::sqrt(m.mse)<<","
       <<std::setprecision(3)<<m.psnr<<","<<std::setprecision(7)<<m.p95<<","
       <<m.maxAbs<<","<<std::setprecision(4)<<m.normalMeanDeg<<","
       <<m.normalP95Deg<<","<<std::setprecision(7)<<m.borderMae<<","
       <<m.interiorMae<<"\n";
}
void compare_case(const std::string& id,const CookedPolygonAsset& baseline,
                  const CookedPolygonAsset& candidate,
                  bool negativeControl,
                  const std::filesystem::path& artifactDir,
                  std::ostream& report) {
    for(const auto mode:{MaterialMappingMode::UV0,MaterialMappingMode::ObjectTriplanar})
        for(const bool grazing:{false,true}) {
            const std::string suffix=mode==MaterialMappingMode::UV0?"uv":"triplanar";
            const std::string tag=id+"_"+suffix+(grazing?"_grazing":"_overhead");
            const auto source=render(baseline,grazing,mode);
            const auto changed=render(candidate,grazing,mode);
            const auto metrics=compare(source,changed);
            // Assert test is sensitive to the normal-map path in an intentionally
            // corrupted control. Normal BC5 quantization itself is allowed to be
            // indistinguishable at some screen pixels.
            if(negativeControl) {
                check(metrics.mae>0.002,"T6 negative control did not change rendered image: "+tag);
            } else {
                check(metrics.mae<0.15 && metrics.p95<0.40,
                      "T6 encoded texture causes excessive beauty regression: "+tag);
                check(metrics.normalMeanDeg<9.0 && metrics.normalP95Deg<30.0,
                      "T6 encoded texture causes excessive normal regression: "+tag);
            }
            log_metrics(report,tag,metrics);
            std::cout<<"T6 "<<tag<<" coverage="<<metrics.coverage
                     <<" MAE="<<metrics.mae<<" PSNR="<<metrics.psnr
                     <<" normal_mean_deg="<<metrics.normalMeanDeg<<"\n";
            if(!artifactDir.empty()) {
                write_png(artifactDir/(tag+"_rgba8.png"),source);
                write_png(artifactDir/(tag+"_encoded.png"),changed);
                write_diff_png(artifactDir/(tag+"_difference_x6.png"),source,changed);
            }
        }
}
} // namespace
int main() {try {
    std::filesystem::path folder;
    if(const char* path=std::getenv("DVE_T6_CAPTURE_DIR");path&&*path) {
        folder=path;
        std::filesystem::create_directories(folder);
    }
    std::ofstream csv;
    if(!folder.empty()){
        csv.open(folder/"metrics.csv",std::ios::binary);
        check(bool(csv),"T6 cannot create metrics.csv");
    }
    auto header="case,covered,changed,mae_rgb,rmse_rgb,psnr_db,p95_max_channel,"
                "max_channel,normal_mean_deg,normal_p95_deg,border_mae,interior_mae\n";
    if(csv.is_open())csv<<header;
    std::cout<<header;
    for(unsigned pattern=0U;pattern<3U;++pattern) {
        auto baseline=baked_scene(pattern);
        auto bc5=baseline;
        bc5.images[0]=compressed_normal(baseline.images[0]);
        static constexpr const char* labels[]={"flat","ripple","woven"};
        compare_case(std::string(labels[pattern])+"_bc5",baseline,bc5,false,folder,
                     csv.is_open()?static_cast<std::ostream&>(csv):static_cast<std::ostream&>(std::cout));
#ifdef DVE_T6_WITH_BC7
        auto bc7=baseline;
        bc7.images[1]=compressed_color(baseline.images[1]);
        compare_case(std::string(labels[pattern])+"_bc7",baseline,bc7,false,folder,
                     csv.is_open()?static_cast<std::ostream&>(csv):static_cast<std::ostream&>(std::cout));
#endif
        if(pattern==1U){
            auto sabotaged=baseline;
            for(auto& c:sabotaged.images[0].rgba8)c=0U;
            for(std::size_t i=0;i<sabotaged.images[0].rgba8.size();i+=4U){
                sabotaged.images[0].rgba8[i]=255U;
                sabotaged.images[0].rgba8[i+1U]=0U;
                sabotaged.images[0].rgba8[i+2U]=128U;
                sabotaged.images[0].rgba8[i+3U]=255U;
            }
            compare_case("negative_control",baseline,sabotaged,true,folder,
                         csv.is_open()?static_cast<std::ostream&>(csv):static_cast<std::ostream&>(std::cout));
        }
    }
    check(!csv.is_open() || bool(csv),"T6 metrics report write failed");
    std::cout<<"T6 CPU reference render comparisons: PASS\n";
    return 0;
}catch(const std::exception& ex){
    std::cerr<<"T6 failure: "<<ex.what()<<"\n";return 1;
}}
