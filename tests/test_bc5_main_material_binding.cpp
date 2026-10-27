#include "dve/render/main_material_table.hpp"
#include "dve/render/normal_bc5_codec.hpp"
#include "dve/render/texture_residency.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {
void check(bool yes,const char* message) {if(!yes)throw std::runtime_error(message);}
dve::CookedPolygonAsset make_asset() {
    dve::CookedPolygonAsset asset;
    asset.contentHash=0x4BC500ULL;
    asset.materials.resize(1U);
    asset.materialBindings.resize(1U);
    dve::PolygonImage image;
    image.name="normal";image.mimeType="image/raw";image.width=4U;image.height=4U;
    image.rgba8.resize(64U);
    for(std::size_t i=0;i<image.rgba8.size();i+=4U) {
        image.rgba8[i]=128U;image.rgba8[i+1U]=128U;image.rgba8[i+2U]=255U;image.rgba8[i+3U]=255U;
    }
    asset.images.push_back(std::move(image));
    asset.samplers.push_back({});
    asset.textures.push_back({"normal",0U,0U});
    asset.materialBindings[0].normal.texture=0U;
    asset.materialBindings[0].doubleSided=true; // unrelated mapping bits must survive every rebind
    return asset;
}
} // namespace

int main() {try {
    using namespace dve;
    using namespace dve::render;
    rhi::NullDevice device;
    MainMaterialDescriptorTable table(device);
    std::string error;
    rhi::TextureDesc shadowDesc;
    shadowDesc.format=rhi::TextureFormat::D32Float;
    shadowDesc.width=4U;shadowDesc.height=4U;
    shadowDesc.usage=rhi::TextureUsage::Sampled|rhi::TextureUsage::DepthStencil;
    shadowDesc.initialState=rhi::ResourceState::ShaderRead;
    const auto shadowA=device.create_texture(shadowDesc,&error);
    const auto shadowB=device.create_texture(shadowDesc,&error);
    check(shadowA && shadowB,"shadow images");
    rhi::TextureViewDesc sv;sv.texture=shadowA;
    const auto viewA=device.create_texture_view(sv,&error);
    sv.texture=shadowB;
    const auto viewB=device.create_texture_view(sv,&error);
    rhi::SamplerDesc samp;samp.comparison=true;
    const auto compare=device.create_sampler(samp,&error);
    check(viewA && viewB && compare,"shadow resources");
    check(table.initialize(viewA,viewB,compare,&error),"table initialize");
    const auto asset=make_asset();
    check(table.ensure_material(asset,0U,&error),"material setup");
    const auto* initial=table.find(asset.contentHash,0U);
    check(initial && initial->bindGroup,"missing default descriptor");
    GpuPolygonMaterialMappingRecord mapping{};
    check(device.read_buffer(initial->mappingRecordBuffer,initial->mappingRecordOffset,
        std::as_writable_bytes(std::span(&mapping,1U)),&error),"default mapping read");
    check((mapping.mappingFlags & kGpuMaterialMappingNormalBc5)==0U,"BC5 bit set on default RGBA normal");
    check((mapping.mappingFlags & kGpuMaterialMappingDoubleSided)!=0U,"test setup lost double-sided flag");
    const auto authoredView=initial->textureViews[2U];
    const auto authoredGroup=initial->bindGroup;
    check(authoredView && initial->authoredNormalView==authoredView,"authored normal view not recorded");
    // Nothing is overridden yet: restore is a no-op that keeps the descriptor.
    check(table.restore_authored_normal(asset.contentHash,0U,&error),"restore without override failed");
    check(table.find(asset.contentHash,0U)->bindGroup==authoredGroup,"no-op restore rebuilt the group");

    TextureResidencyManager compressed(device,4096U,{},make_bc5_normal_encoder());
    check(compressed.enqueue({500U,asset.images[0],TextureSemantic::Normal,
        TextureCompressionPreference::PreferBc5ForNormals,false},&error),"BC5 request");
    check(compressed.pump(4096U,&error)!=0U,"BC5 resident upload");
    const auto* resident=compressed.find(500U);
    check(resident && resident->format==rhi::TextureFormat::BC5RGUnorm,"BC5 native residency");
    check(table.bind_resident_normal(asset.contentHash,0U,*resident,&error),
          "BC5 live descriptor rebinding");
    const auto* bc5=table.find(asset.contentHash,0U);
    check(bc5 && bc5->textureViews[2U]==resident->view,"BC5 descriptor view not replaced");
    check(device.read_buffer(bc5->mappingRecordBuffer,bc5->mappingRecordOffset,
        std::as_writable_bytes(std::span(&mapping,1U)),&error),"BC5 mapping read");
    check((mapping.mappingFlags & kGpuMaterialMappingNormalBc5)!=0U,"BC5 shader flag not published");
    check((mapping.mappingFlags & kGpuMaterialMappingDoubleSided)!=0U,"BC5 rebind dropped unrelated flags");
    check(bc5->authoredNormalView==authoredView,"authored normal view lost by override");
    const auto oldGroup=bc5->bindGroup;
    check(oldGroup!=authoredGroup,"BC5 rebind did not publish a new group");

    TextureResidencyManager fallback(device,4096U);
    check(fallback.enqueue({501U,asset.images[0],TextureSemantic::Normal,
        TextureCompressionPreference::PreferBc5ForNormals,false},&error),"rgba fallback enqueue");
    check(fallback.pump(4096U,&error)!=0U,"rgba fallback publish");
    const auto* uncompressed=fallback.find(501U);
    check(uncompressed && uncompressed->format==rhi::TextureFormat::RGBA8Unorm,"fallback native format");
    check(table.bind_resident_normal(asset.contentHash,0U,*uncompressed,&error),
          "RGBA8 descriptor rebinding");
    const auto* rgba=table.find(asset.contentHash,0U);
    check(rgba && rgba->bindGroup!=oldGroup,"old BC5 bind group reused");
    check(rgba->textureViews[2U]==uncompressed->view,"RGBA8 view not installed");
    check(device.read_buffer(rgba->mappingRecordBuffer,rgba->mappingRecordOffset,
        std::as_writable_bytes(std::span(&mapping,1U)),&error),"RGBA8 mapping read");
    check((mapping.mappingFlags & kGpuMaterialMappingNormalBc5)==0U,"stale BC5 shader flag after fallback");
    // No authored normal or invalid external residency must not replace valid bound resources.
    ResidentTexture invalid{};
    const auto safeGroup=rgba->bindGroup;
    const auto idleBeforeReject=device.wait_idle_calls();
    check(!table.bind_resident_normal(asset.contentHash,0U,invalid,&error),
          "invalid resident was published");
    check(table.find(asset.contentHash,0U)->bindGroup==safeGroup,
          "invalid resident destroyed previous descriptor");
    check(device.wait_idle_calls()==idleBeforeReject,"rejected rebind stalled the GPU");
    // Wrong semantic, sRGB and unsupported formats are rejected without touching the binding.
    // BC5 requires the BC5 flag, so first re-publish BC5 and verify rejects keep that state.
    check(table.bind_resident_normal(asset.contentHash,0U,*resident,&error),"BC5 re-publish");
    const auto bc5Group=table.find(asset.contentHash,0U)->bindGroup;
    auto badSemantic=*resident;badSemantic.semantic=TextureSemantic::BaseColor;
    auto srgb=*uncompressed;srgb.format=rhi::TextureFormat::RGBA8Srgb;
    auto bc4=*uncompressed;bc4.format=rhi::TextureFormat::BC4RUnorm;
    for(const auto* bad:{&badSemantic,&srgb,&bc4}) {
        check(!table.bind_resident_normal(asset.contentHash,0U,*bad,&error),"bad resident format published");
        check(table.find(asset.contentHash,0U)->bindGroup==bc5Group,"rejected rebind changed bind group");
        check(device.read_buffer(rgba->mappingRecordBuffer,rgba->mappingRecordOffset,
            std::as_writable_bytes(std::span(&mapping,1U)),&error),"post-reject mapping read");
        check((mapping.mappingFlags & kGpuMaterialMappingNormalBc5)!=0U,
              "rejected rebind cleared the live BC5 flag of the still-bound BC5 view");
    }
    check(!table.bind_resident_normal(asset.contentHash,7U,*resident,&error),"unknown material accepted");
    // Linear BC7 normals (PreferBc7) use the legacy decode: accepted, flag clear.
    auto bc7=*uncompressed;bc7.format=rhi::TextureFormat::BC7RGBAUnorm;
    check(table.bind_resident_normal(asset.contentHash,0U,bc7,&error),"linear BC7 normal rejected");
    check(device.read_buffer(rgba->mappingRecordBuffer,rgba->mappingRecordOffset,
        std::as_writable_bytes(std::span(&mapping,1U)),&error),"BC7 mapping read");
    check((mapping.mappingFlags & kGpuMaterialMappingNormalBc5)==0U,"BC7 normal marked as BC5");
    // Every committed rebind drains in-flight GPU work first (shared mapping record + old group).
    const auto idleBeforeCommit=device.wait_idle_calls();
    check(table.bind_resident_normal(asset.contentHash,0U,*resident,&error),"BC5 final publish");
    check(device.wait_idle_calls()==idleBeforeCommit+1U,"rebind did not wait for in-flight frames");
    // Undo: authored view returns, BC5 flag clears, other flags survive, repeated undo is a no-op.
    check(table.restore_authored_normal(asset.contentHash,0U,&error),"restore authored normal");
    const auto* restored=table.find(asset.contentHash,0U);
    check(restored->textureViews[2U]==authoredView,"authored view not restored");
    check(device.read_buffer(restored->mappingRecordBuffer,restored->mappingRecordOffset,
        std::as_writable_bytes(std::span(&mapping,1U)),&error),"restored mapping read");
    check((mapping.mappingFlags & kGpuMaterialMappingNormalBc5)==0U,"BC5 flag survived restore");
    check((mapping.mappingFlags & kGpuMaterialMappingDoubleSided)!=0U,"restore dropped unrelated flags");
    const auto restoredGroup=restored->bindGroup;
    check(table.restore_authored_normal(asset.contentHash,0U,&error) &&
          table.find(asset.contentHash,0U)->bindGroup==restoredGroup,"second restore not a no-op");
    check(!table.restore_authored_normal(asset.contentHash,9U,&error),"restore of unknown material accepted");

    table.clear();
    compressed.clear();
    fallback.clear();
    check(device.destroy_sampler(compare,&error),"destroy shadow compare sampler");
    check(device.destroy_texture_view(viewB,&error),"destroy shadow view B");
    check(device.destroy_texture_view(viewA,&error),"destroy shadow view A");
    check(device.destroy_texture(shadowB,&error),"destroy shadow image B");
    check(device.destroy_texture(shadowA,&error),"destroy shadow image A");
    const auto stats=device.statistics();
    check(stats.bindGroupsCreated==stats.bindGroupsDestroyed,"binding group leak");
    check(stats.texturesCreated==stats.texturesDestroyed,"texture leak");
    check(stats.textureViewsCreated==stats.textureViewsDestroyed,"view leak");
    std::cout<<"T4 main-material BC5 binding: PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
