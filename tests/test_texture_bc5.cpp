#include "dve/render/normal_bc5_codec.hpp"
#include "dve/rhi/null_device.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::render;
namespace {
void require(bool ok,const char* message) {
    if(!ok) throw std::runtime_error(message);
}
PolygonImage normal_image(std::uint32_t w,std::uint32_t h,unsigned seed) {
    PolygonImage image;
    image.name="normal"; image.mimeType="image/raw";
    image.width=w;image.height=h;
    image.rgba8.resize(static_cast<std::size_t>(w)*h*4U);
    for(std::uint32_t y=0;y<h;++y)for(std::uint32_t x=0;x<w;++x) {
        const float nx=0.45F*std::sin(0.13F*static_cast<float>(x+seed));
        const float ny=0.40F*std::cos(0.11F*static_cast<float>(y+seed));
        const float nz=std::sqrt(std::max(0.0F,1.0F-nx*nx-ny*ny));
        const auto i=(static_cast<std::size_t>(y)*w+x)*4U;
        image.rgba8[i]=static_cast<std::uint8_t>((nx*0.5F+0.5F)*255.0F+0.5F);
        image.rgba8[i+1U]=static_cast<std::uint8_t>((ny*0.5F+0.5F)*255.0F+0.5F);
        image.rgba8[i+2U]=static_cast<std::uint8_t>((nz*0.5F+0.5F)*255.0F+0.5F);
        image.rgba8[i+3U]=255U;
    }
    return image;
}
void test_codec_and_decoding() {
    const auto image=normal_image(13U,9U,3U);
    const auto chain=generate_semantic_texture_mips(image,TextureSemantic::Normal);
    std::vector<std::vector<std::byte>> encoded,again;
    std::string error;
    require(encode_bc5_normal_mips(chain,encoded,&error),"BC5 encode failed");
    require(encode_bc5_normal_mips(chain,again,&error) && again==encoded,
            "BC5 encode is not deterministic");
    require(encoded.size()==chain.levels.size(),"BC5 missing mips");
    for(std::size_t i=0;i<encoded.size();++i) {
        const auto& mip=chain.levels[i];
        const auto bytes=rhi::texture_row_bytes(rhi::TextureFormat::BC5RGUnorm,mip.width)*
                         rhi::texture_rows(rhi::TextureFormat::BC5RGUnorm,mip.height);
        require(encoded[i].size()==bytes,"BC5 mip byte size or edge blocks");
        const double angle=bc5_mean_normal_angle_degrees(mip,encoded[i]);
        require(angle>=0.0 && angle<8.0,"BC5 angular reconstruction error too high");
    }
    require(bc5_mean_normal_angle_degrees(chain.levels.front(),{})<0.0,
            "BC5 metric accepted empty compressed payload");
    auto bad=chain;
    bad.levels.front().rgba8[2]=0U;
    require(!encode_bc5_normal_mips(bad,encoded,&error),
            "negative-Z tangent normal must be rejected");
    require(encoded.empty(),"failed BC5 encode retained stale bytes");
}
void test_residency_and_fallback() {
    rhi::NullDevice device;
    TextureResidencyManager manager(device,131072U,{},make_bc5_normal_encoder());
    std::string error;
    const auto image=normal_image(7U,5U,7U);
    const auto request=[&](const PolygonImage& img) {
        return TextureUploadRequest{111U,img,TextureSemantic::Normal,
              TextureCompressionPreference::PreferBc5ForNormals,true,
              TextureUploadMode::ReplaceIfChanged};
    };
    require(manager.enqueue(request(image),&error),"BC5 residency enqueue");
    const auto bytes=manager.pump(131072U,&error);
    require(bytes>0U,"BC5 residency pump failed");
    const auto* record=manager.find(111U);
    require(record && record->format==rhi::TextureFormat::BC5RGUnorm,"BC5 GPU format");
    require(record->residentBytes==80U,"7x5 BC5 3-level total should be 80 bytes");
    const auto old=manager.reference(111U);
    require(old.has_value(),"missing BC5 reference");
    require(manager.enqueue(request(image),&error),"BC5 unchanged request");
    require(manager.stats().unchangedRequests==1U,"BC5 duplicate bypass");
    require(manager.enqueue(request(normal_image(7U,5U,9U)),&error),"BC5 reload");
    require(manager.pump(131072U,&error)>0U,"BC5 replacement upload");
    require(!manager.reference_valid(*old),"BC5 stale generation not invalidated");
    require(manager.stats().residentTexturesReplaced==1U,"BC5 replacement count");

    TextureResidencyManager fallback(device,131072U);
    require(fallback.enqueue({222U,image,TextureSemantic::Normal,
        TextureCompressionPreference::PreferBc5ForNormals,false},&error),
        "BC5 no-codec fallback enqueue");
    require(fallback.pump(131072U,&error)>0U,"BC5 no-codec fallback upload");
    require(fallback.find(222U)->format==rhi::TextureFormat::RGBA8Unorm,
            "BC5 without codec must retain RGBA8 fallback");
    require(fallback.stats().compressionFallbacks==1U,"BC5 fallback telemetry");

    auto invalid=normal_image(4U,4U,2U);invalid.rgba8[2]=0U;
    require(manager.enqueue({333U,invalid,TextureSemantic::Normal,
        TextureCompressionPreference::PreferBc5ForNormals,false},&error),
        "negative-Z must permit RGBA8 fallback");
    require(manager.pump(131072U,&error)>0U,"negative-Z fallback upload");
    require(manager.find(333U)->format==rhi::TextureFormat::RGBA8Unorm,"negative-Z BC5 erroneously selected");
    require(manager.stats().compressionEncodeFailures>=1U,"BC5 codec refusal uncounted");
    manager.clear();fallback.clear();
    require(device.statistics().texturesCreated==device.statistics().texturesDestroyed,
            "BC5 GPU resource leak");
}
}
int main() {try{
    test_codec_and_decoding();test_residency_and_fallback();
    std::cout<<"T3 BC5 normals: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
