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
void upload_budget_uses_selected_format_size() {
    // Codex 4385582816: pump() admission must use the compressed size of the format that will
    // actually be uploaded, matching the `uploaded` accounting.
    rhi::NullDevice device;
    TextureResidencyManager manager(device,1U<<20,make_basis_bc7f_encoder());
    std::string error;
    const auto enqueue=[&](std::uint64_t id,std::uint32_t w,std::uint32_t h,std::uint32_t seed) {
        return manager.enqueue({id,image("budget",w,h,seed),TextureSemantic::BaseColor,
            TextureCompressionPreference::PreferBc7,false,TextureUploadMode::ReplaceIfChanged},&error);
    };
    // Two 1x1 BC7 textures are 16 B each (4 B as RGBA8): pump(20) must admit exactly one.
    check(enqueue(801U,1,1,1U) && enqueue(802U,1,1,2U),"1x1 enqueue");
    check(manager.pump(20U,&error)==16U,"pump(20) should upload only one 16-byte BC7 texture");
    check(manager.stats().pendingCount==1U,"second 1x1 BC7 texture should stay queued");
    check(manager.pump(20U,&error)==16U && manager.stats().pendingCount==0U,
          "second 1x1 BC7 texture was not uploaded by the next pump");
    // 16x16 is 1024 B as RGBA8 but 256 B as BC7: 512 fits in a 600 B budget, a third does not.
    check(enqueue(811U,16,16,3U) && enqueue(812U,16,16,4U) && enqueue(813U,16,16,5U),
          "16x16 enqueue");
    check(manager.pump(600U,&error)==512U,"compressed textures that fit the budget were deferred");
    check(manager.stats().pendingCount==1U,"third 16x16 BC7 texture should stay queued");
    // The first upload is always admitted even when it alone exceeds the per-pump budget.
    check(manager.pump(1U,&error)==256U,"oversized first upload must still be admitted");
}
void unchanged_requests_skip_the_encoder() {
    // Codex 4385582822: duplicate/unchanged requests must not run the (expensive) encoder.
    rhi::NullDevice device;
    std::atomic<std::uint32_t> calls{0U};
    const auto inner=make_basis_bc7f_encoder();
    const TextureBc7Encoder counting=[&](const PolygonTextureMipChain& chain,
        std::vector<std::vector<std::byte>>& out,std::string* error) {
        calls.fetch_add(1U); return inner(chain,out,error);
    };
    TextureResidencyManager manager(device,1U<<20,counting);
    std::string error;
    const auto img=image("hot",8,8,21U);
    const auto request=[&](TextureUploadMode mode) {
        return TextureUploadRequest{901U,img,TextureSemantic::BaseColor,
            TextureCompressionPreference::PreferBc7,true,mode};
    };
    check(manager.enqueue(request(TextureUploadMode::ReplaceIfChanged),&error),"first enqueue");
    check(calls.load()==1U,"first request must encode once");
    // Unchanged while still queued.
    check(manager.enqueue(request(TextureUploadMode::ReplaceIfChanged),&error),"queued unchanged");
    check(calls.load()==1U && manager.stats().unchangedRequests==1U,"queued unchanged re-encoded");
    // Duplicate rejected while queued.
    check(!manager.enqueue(request(TextureUploadMode::RejectDuplicate),&error),"queued duplicate accepted");
    check(calls.load()==1U && manager.stats().duplicateRequests==1U,"queued duplicate re-encoded");
    check(manager.pump(1U<<20,&error)>0U,"upload");
    // Unchanged while resident.
    check(manager.enqueue(request(TextureUploadMode::ReplaceIfChanged),&error),"resident unchanged");
    check(calls.load()==1U && manager.stats().unchangedRequests==2U,"resident unchanged re-encoded");
    check(!manager.enqueue(request(TextureUploadMode::RejectDuplicate),&error),"resident duplicate accepted");
    check(calls.load()==1U && manager.stats().duplicateRequests==2U,"resident duplicate re-encoded");
    check(manager.stats().compressionEncoded==1U,"encode telemetry should count one encode");
    // A genuine change still encodes.
    auto changed=image("hot",8,8,22U);
    check(manager.enqueue({901U,changed,TextureSemantic::BaseColor,
        TextureCompressionPreference::PreferBc7,true,TextureUploadMode::ReplaceIfChanged},&error),
        "changed enqueue");
    check(calls.load()==2U,"changed request must encode");
}
void encoder_is_deterministic_and_thread_safe() {
    const auto encoder=make_basis_bc7f_encoder();
    const auto chain=generate_semantic_texture_mips(image("det",13,9,31U),TextureSemantic::BaseColor);
    std::vector<std::vector<std::byte>> reference;
    std::string error;
    check(encoder(chain,reference,&error),"reference encode");
    std::vector<std::vector<std::byte>> again;
    check(encoder(chain,again,&error) && again==reference,"bc7f encoder is not deterministic");
    std::atomic<bool> same{true};
    std::vector<std::thread> threads;
    for (int t=0;t<4;++t) threads.emplace_back([&] {
        for (int i=0;i<4;++i) {
            std::vector<std::vector<std::byte>> out; std::string e;
            if (!encoder(chain,out,&e) || out!=reference) same.store(false);
        }
    });
    for (auto& thread:threads) thread.join();
    check(same.load(),"concurrent bc7f encodes diverged");
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
        encoder_is_deterministic_and_thread_safe();
        upload_budget_uses_selected_format_size();
        unchanged_requests_skip_the_encoder();
        std::cout<<"T2 BC7 residency tests: PASS\n";
    } catch (const std::exception& e) {
        std::cerr<<"T2 failure: "<<e.what()<<"\n";
        return 1;
    }
}
