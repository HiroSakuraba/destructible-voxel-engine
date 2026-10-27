#include "dve/rhi/pipeline_cache_file.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace dve::rhi::pso;
namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
std::vector<std::byte> sample(const Identity& id) {
    std::vector<std::byte> blob(256U);
    write32(blob,0U,32U); // VkPipelineCacheHeaderVersionOne.headerSize
    write32(blob,4U,1U);  // headerVersionOne
    write32(blob,8U,id.vendor);
    write32(blob,12U,id.device);
    for(std::size_t i=0;i<id.uuid.size();++i)
        blob[16U+i]=static_cast<std::byte>(id.uuid[i]);
    for(std::size_t i=32U;i<blob.size();++i)
        blob[i]=static_cast<std::byte>((i*39U+7U)&255U);
    return blob;
}
void test() {
    Identity gpu{0x10deU,0x1234U,0x9988U,0x401000U,{}};
    for(std::size_t i=0;i<gpu.uuid.size();++i)
        gpu.uuid[i]=static_cast<std::uint8_t>(i*13U+9U);
    const auto source=sample(gpu);
    std::vector<std::byte> encoded,decoded,loaded;
    check(encode(gpu,source,encoded),"cannot encode valid cache");
    check(encoded.size()==kEnvelopeBytes+source.size(),"envelope length incorrect");
    check(decode(encoded,gpu,decoded)&&decoded==source,"cache roundtrip failed");
    auto fail=[](const std::vector<std::byte>& bytes,const Identity& id) {
        std::vector<std::byte> output{std::byte{1}};
        check(!decode(bytes,id,output)&&output.empty(),"bad cache accepted or data retained");
    };
    auto mismatch=gpu;++mismatch.vendor;fail(encoded,mismatch);
    mismatch=gpu;++mismatch.device;fail(encoded,mismatch);
    mismatch=gpu;++mismatch.driver;fail(encoded,mismatch);
    mismatch=gpu;++mismatch.api;fail(encoded,mismatch);
    mismatch=gpu;mismatch.uuid[4U]^=0x40U;fail(encoded,mismatch);
    for(const std::size_t cut:{0U,1U,12U,31U,63U,64U,65U,80U,90U,319U}) {
        if(cut<encoded.size())fail({encoded.begin(),encoded.begin()+static_cast<std::ptrdiff_t>(cut)},gpu);
    }
    auto bytes=encoded;bytes[120U]^=std::byte{1};fail(bytes,gpu); // FNV
    bytes=encoded;bytes[8U]^=std::byte{1};fail(bytes,gpu); // envelope version
    bytes=encoded;bytes[56U]^=std::byte{1};fail(bytes,gpu); // reserved
    bytes=encoded;bytes[64U+4U]^=std::byte{1};fail(bytes,gpu); // Vulkan header version
    bytes=encoded;bytes[64U+8U]^=std::byte{1};fail(bytes,gpu); // Vulkan vendor
    bytes=encoded;bytes[64U+16U]^=std::byte{1};fail(bytes,gpu); // Vulkan UUID
    bytes=encoded;write32(bytes,44U,999999U);fail(bytes,gpu);
    bytes=encoded;bytes.push_back(std::byte{1});fail(bytes,gpu);
    bytes.assign(kMaximumDriverCacheBytes+kEnvelopeBytes+1U,std::byte{0});
    fail(bytes,gpu); // oversized hostile input
    bytes=source;write32(bytes,0U,8U);
    check(!encode(gpu,bytes,decoded)&&decoded.empty(),"invalid Vulkan header encoded");
    bytes=source;write32(bytes,4U,2U);
    check(!encode(gpu,bytes,decoded)&&decoded.empty(),"unknown Vulkan header version encoded");
    const auto dir=std::filesystem::temp_directory_path()/
       ("dve-psocache-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path=dir/"nested"/"cache.bin";
    check(!load(path,gpu,loaded),"nonexistent cache loaded");
    check(save(path,gpu,source),"cannot atomically save cache");
    check(load(path,gpu,loaded)&&loaded==source,"cannot reload exact cache");
    mismatch=gpu;++mismatch.driver;
    check(!load(path,mismatch,loaded)&&loaded.empty(),"driver mismatch loaded from disk");
    // A second save to an existing destination must never corrupt the old
    // cache if the OS doesn't support atomic rename-overwrite.
    (void)save(path,gpu,source);
    check(load(path,gpu,loaded)&&loaded==source,"second save corrupted the cache");
    std::error_code ec;
    std::filesystem::remove_all(dir,ec);
}
}
int main(){try{test();std::cout<<"PSO-1 cache validation PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
