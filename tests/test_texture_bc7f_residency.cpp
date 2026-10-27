#include "dve/render/bc7f_texture_encoder.hpp"
#include "dve/render/material_bake_residency.hpp"
#include "dve/rhi/null_device.hpp"
#include <algorithm>
#include <atomic>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::render;
namespace {
void check(bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}
PolygonImage image(const char* name, std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    PolygonImage result;
    result.name = name; result.mimeType = "image/raw"; result.width = w; result.height = h;
    result.rgba8.resize(static_cast<std::size_t>(w)*h*4U);
    for (std::uint32_t y=0; y<h; ++y) for (std::uint32_t x=0; x<w; ++x) {
        const auto i=(static_cast<std::size_t>(y)*w+x)*4U;
        result.rgba8[i]   = static_cast<std::uint8_t>((x*57U+y*19U+seed)&255U);
        result.rgba8[i+1] = static_cast<std::uint8_t>((x*13U+y*73U+seed*3U)&255U);
        result.rgba8[i+2] = static_cast<std::uint8_t>((x*23U+y*31U+seed*5U)&255U);
        result.rgba8[i+3] = static_cast<std::uint8_t>((x*29U+y*7U+seed*17U)&255U);
    }
    return result;
}
std::size_t expected_bc7_bytes(std::uint32_t w, std::uint32_t h, bool mipmapped) {
    std::size_t n=0U;
    do {
        n+=rhi::texture_row_bytes(rhi::TextureFormat::BC7RGBAUnorm,w)*
           rhi::texture_rows(rhi::TextureFormat::BC7RGBAUnorm,h);
        if(!mipmapped || (w==1U && h==1U))break;
        w=std::max(1U,w/2U); h=std::max(1U,h/2U);
    } while(true);
    return n;
}
void native_upload_and_hot_reload() {
    rhi::NullDevice device;
    TextureResidencyManager manager(device,8192U,make_basis_bc7f_encoder());
    std::string error;
    auto img=image("coat",7,5,5U);
    check(manager.enqueue({101U,img,TextureSemantic::BaseColor,
        TextureCompressionPreference::PreferBc7,true,TextureUploadMode::ReplaceIfChanged},&error),
        "could not enqueue BC7");
    check(manager.pump(8192U,&error)>0U,"BC7 upload failed");
    const auto* t=manager.find(101U);
    check(t && t->format==rhi::TextureFormat::BC7RGBASrgb,"color must be BC7 sRGB");
    check(t->mipLevels==3U,"expected three mips for 7x5");
    check(t->residentBytes==expected_bc7_bytes(7U,5U,true),"incorrect resident compressed bytes");
    const auto old=manager.reference(101U);
    check(old && manager.reference_valid(*old),"BC7 original reference invalid");

    check(manager.enqueue({101U,img,TextureSemantic::BaseColor,
        TextureCompressionPreference::PreferBc7,true,TextureUploadMode::ReplaceIfChanged},&error),
        "identical BC7 update failed");
    check(manager.stats().unchangedRequests==1U,"identical BC7 update not deduplicated");

    auto changed=image("coat",7,5,6U);
    check(manager.enqueue({101U,changed,TextureSemantic::BaseColor,
        TextureCompressionPreference::PreferBc7,true,TextureUploadMode::ReplaceIfChanged},&error),
        "changed BC7 update failed");
    check(manager.pump(8192U,&error)>0U,"BC7 hot reload failed");
    check(manager.reference(101U)->generation==old->generation+1U,"BC7 hot reload generation");
    check(!manager.reference_valid(*old),"BC7 stale binding remains valid");
    check(manager.find(101U)->format==rhi::TextureFormat::BC7RGBASrgb,"hot reload switched format");
    check(manager.stats().residentTexturesReplaced==1U,"BC7 replacement count");

    // Source data textures use linear sampling, even with the same BC7 physical block layout.
    check(manager.enqueue({102U,image("roughness",4,4,9U),TextureSemantic::MetallicRoughness,
        TextureCompressionPreference::PreferBc7,false,TextureUploadMode::RejectDuplicate},&error),
        "data texture enqueue");
    check(manager.pump(8192U,&error)>0U,"data texture upload");
    check(manager.find(102U)->format==rhi::TextureFormat::BC7RGBAUnorm,"data texture wrongly sRGB");
    check(manager.find(102U)->residentBytes==16U,"4x4 BC7 resident byte count");
    check(manager.stats().compressionEncoded>=3U,"expected encoder invocations");
    manager.clear();
    check(device.statistics().texturesCreated==device.statistics().texturesDestroyed,
          "BC7 hot reload leaked GPU textures");
}
void fallback_and_transaction() {
    rhi::NullDevice device;
    const TextureBc7Encoder refuse=[](const PolygonTextureMipChain&,
        std::vector<std::vector<std::byte>>&,std::string*) {return false;};
    TextureResidencyManager manager(device,4096U,refuse);
    std::string error;
    check(manager.enqueue({201U,image("fallback",7,5,1U),TextureSemantic::BaseColor,
        TextureCompressionPreference::PreferBc7,false,TextureUploadMode::ReplaceIfChanged},&error),
        "enqueue encoder failure fallback");
    check(manager.pump(4096U,&error)>0U,"encode failure did not fall back to rgba");
    const auto* t=manager.find(201U);
    check(t && t->format==rhi::TextureFormat::RGBA8Unorm && t->residentBytes==140U,
          "incorrect fallback format/bytes");
    check(manager.stats().compressionFallbacks==1U &&
          manager.stats().compressionEncodeFailures==1U,"missing encode failure telemetry");
    const auto reference=manager.reference(201U);
    manager.set_budget(4U);
    check(manager.enqueue({201U,image("fallback",7,5,2U),TextureSemantic::BaseColor,
        TextureCompressionPreference::PreferBc7,false,TextureUploadMode::ReplaceIfChanged},&error),
        "enqueue over-budget replacement");
    check(manager.pump(4096U,&error)==0U,"over-budget replacement should fail");
    check(reference && manager.reference_valid(*reference),"failed replacement lost original texture");
    check(manager.stats().failedReplacementsPreserved==1U,"failed replacement not tracked");

    // The encoder success contract is checked: a wrong-sized BC7 payload falls back.
    const TextureBc7Encoder malformed=[](const PolygonTextureMipChain&,
        std::vector<std::vector<std::byte>>& data,std::string*) {
        data={{std::byte{0x12}}}; return true;
    };
    TextureResidencyManager malformedManager(device,4096U,malformed);
    check(malformedManager.enqueue({301U,image("bad",4,4,2U),TextureSemantic::BaseColor,
        TextureCompressionPreference::PreferBc7,false},&error),"malformed encoder enqueue");
    check(malformedManager.pump(4096U,&error)>0U,"malformed encoder fallback upload");
    check(malformedManager.find(301U)->format==rhi::TextureFormat::RGBA8Unorm,
          "accepted malformed BC7 output");
    check(malformedManager.stats().compressionEncodeFailures==1U,"malformed encoder not counted");
}
void baked_material_map_adapter() {
    rhi::NullDevice device;
    TextureResidencyManager manager(device,8192U,make_basis_bc7f_encoder());
    MaterialTextureBakeResult bake;
    bake.success=true;
    bake.images[MaterialBakeMap::BaseColor]=image("baked_coat",7,5,3U);
    bake.images[MaterialBakeMap::Normal]=image("baked_normal",7,5,7U);
    const std::vector<MaterialBakeTextureBinding> bindings{
        {MaterialBakeMap::BaseColor,71U,TextureCompressionPreference::PreferBc7},
        {MaterialBakeMap::Normal,72U,TextureCompressionPreference::PreferBc5ForNormals},
    };
    MaterialBakeResidencyReport report;
    std::string error;
    check(enqueue_material_bake_textures(manager,bake,bindings,report,
          TextureUploadMode::ReplaceIfChanged,&error),"baked material map adapter rejected valid input");
    check(report.accepted==2U && report.rejected==0U,"baked map adapter report");
    check(manager.pump(8192U,&error)>0U,"baked material pump");
    check(manager.find(71U)->format==rhi::TextureFormat::BC7RGBASrgb,
          "baked color did not use BC7 sRGB");
    check(manager.find(72U)->format==rhi::TextureFormat::RGBA8Unorm,
          "BC5 normal preference should conservatively use RGBA8");
    check(manager.stats().compressionFallbacks==1U,"normal fallback telemetry absent");

    // Invalid or missing mappings fail before enqueueing a partial set.
    const auto queuedBefore=manager.stats().requestsQueued;
    const std::vector<MaterialBakeTextureBinding> invalid{
        {MaterialBakeMap::BaseColor,73U,TextureCompressionPreference::PreferBc7},
        {MaterialBakeMap::Emission,74U,TextureCompressionPreference::PreferBc7}
    };
    check(!enqueue_material_bake_textures(manager,bake,invalid,report,
        TextureUploadMode::ReplaceIfChanged,&error),"missing baked map accepted");
    check(manager.stats().requestsQueued==queuedBefore,
        "binding validation partially queued maps");
}
void concurrent_worker_enqueue_and_gpu_pump() {
    rhi::NullDevice device;
    TextureResidencyManager manager(device,65536U,make_basis_bc7f_encoder());
    std::atomic<bool> done{false};
    std::atomic<bool> succeeded{true};
    std::thread worker([&] {
        for (std::uint32_t i=0;i<12U;++i) {
            std::string error;
            if (!manager.enqueue({500U+i,image("parallel",8U,8U,i+13U),
                  TextureSemantic::BaseColor,TextureCompressionPreference::PreferBc7,true,
                  TextureUploadMode::ReplaceIfChanged},&error)) {
                succeeded.store(false);
                break;
            }
        }
        done.store(true);
    });
    while(!done.load() || manager.stats().pendingCount!=0U) {
        std::string error;
        (void)manager.pump(8192U,&error);
        std::this_thread::yield();
    }
    worker.join();
    check(succeeded.load(),"parallel worker enqueue failed");
    check(manager.stats().uploadsCompleted==12U,"parallel enqueue/pump lost an upload");
    for (std::uint32_t i=0;i<12U;++i) {
        const auto* texture=manager.find(500U+i);
        check(texture && texture->format==rhi::TextureFormat::BC7RGBASrgb,
              "parallel BC7 upload missing or wrong format");
    }
}
void unchanged_source_different_preference() {
    rhi::NullDevice device;
    TextureResidencyManager manager(device,4096U,make_basis_bc7f_encoder());
    std::string error;
    const auto img=image("toggle",4,4,3U);
    check(manager.enqueue({7U,img,TextureSemantic::BaseColor,TextureCompressionPreference::Uncompressed,
        false,TextureUploadMode::ReplaceIfChanged},&error),"rgba enqueue");
    check(manager.pump(4096U,&error)>0U,"rgba upload");
    const auto old=manager.reference(7U);
    check(manager.enqueue({7U,img,TextureSemantic::BaseColor,TextureCompressionPreference::PreferBc7,
        false,TextureUploadMode::ReplaceIfChanged},&error),"compression preference toggle");
    check(manager.pump(4096U,&error)>0U,"preference conversion upload");
    check(manager.find(7U)->format==rhi::TextureFormat::BC7RGBASrgb,
          "identical pixels but new compression preference was skipped");
    check(old && !manager.reference_valid(*old),"preference change did not invalidate descriptor generation");
}
}
int main() {
    try {
        native_upload_and_hot_reload();
        fallback_and_transaction();
        unchanged_source_different_preference();
        baked_material_map_adapter();
        concurrent_worker_enqueue_and_gpu_pump();
        std::cout<<"T2 BC7 residency tests: PASS\n";
    } catch (const std::exception& e) {
        std::cerr<<"T2 failure: "<<e.what()<<"\n";
        return 1;
    }
}
