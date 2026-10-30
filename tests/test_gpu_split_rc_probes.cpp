#include "dve_test_skip.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using namespace dve::rhi;
namespace {
std::string error;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message+": "+error);}
struct Sample { float x,y,z,base; int level,sheet,pad0,pad1; };
static_assert(sizeof(Sample)==32);
std::vector<std::byte> load(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary|std::ios::ate);check(bool(in),"open shader");const auto size=in.tellg();std::vector<std::byte> out(static_cast<std::size_t>(size));in.seekg(0);in.read(reinterpret_cast<char*>(out.data()),size);check(bool(in),"read shader");return out;}
unsigned key(const Sample& s){const auto spacing=s.base*float(1u<<s.level);const auto x=unsigned(int(std::floor(s.x/spacing))+128),y=unsigned(int(std::floor(s.y/spacing))+128),z=unsigned(int(std::floor(s.z/spacing))+128);return (x|(y<<8)|(z<<16)|(unsigned(s.level)<<24)|(unsigned(s.sheet)<<26))+1;}
void run(VulkanDevice& d,const std::vector<std::byte>& shader,const std::vector<Sample>& samples,unsigned capacity,unsigned invalid,unsigned full){
    const auto make=[&](std::size_t bytes){BufferDesc b;b.bytes=bytes;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
    const auto a=make(samples.size()*sizeof(Sample)),b=make(capacity*4),c=make(samples.size()*4),e=make(8);check(a&&b&&c&&e,"buffers");
    std::vector<unsigned> slots(capacity),out(samples.size());std::array<unsigned,2> counters{};
    check(d.write_buffer(a,0,std::as_bytes(std::span(samples)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(slots)),&error)&&d.write_buffer(e,0,std::as_bytes(std::span(counters)),&error),"initialize");
    BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{2,BindingType::StorageBufferReadWrite,ShaderStage::Compute},{3,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
    const auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,a,{},0,samples.size()*sizeof(Sample),{}},{1,b,{},0,capacity*4,{}},{2,c,{},0,samples.size()*4,{}},{3,e,{},0,8,{}}};
    const auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=shader;pd.bindGroupLayouts={layout};const auto pipeline=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipeline,"pipeline");
    const auto cmd=d.begin_commands(QueueKind::Compute,"split RC sparse probe conformance",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipeline,static_cast<unsigned>((samples.size()+63)/64),1,1,&error),"dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");
    check(d.read_buffer(b,0,std::as_writable_bytes(std::span(slots)),&error)&&d.read_buffer(c,0,std::as_writable_bytes(std::span(out)),&error)&&d.read_buffer(e,0,std::as_writable_bytes(std::span(counters)),&error),"readback");
    check(counters[0]==invalid&&counters[1]==full,"overflow counters");
    unsigned failures=0;std::unordered_map<unsigned,unsigned> keyToSlot;std::unordered_map<unsigned,unsigned> slotToKey;
    for(std::size_t i=0;i<samples.size();++i){if(out[i]==0xffffffffu){++failures;continue;}check(out[i]<capacity,"slot range");const auto k=key(samples[i]);check(slots[out[i]]==k,"stored key mismatch");
        const auto [a1,insert1]=keyToSlot.emplace(k,out[i]);check(insert1||a1->second==out[i],"duplicate key received different slot");
        const auto [b1,insert2]=slotToKey.emplace(out[i],k);check(insert2||b1->second==k,"different keys collided");}
    check(failures==invalid+full,"failure count");
    check(d.destroy_compute_pipeline(pipeline,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error)&&d.destroy_buffer(a,&error)&&d.destroy_buffer(b,&error)&&d.destroy_buffer(c,&error)&&d.destroy_buffer(e,&error),"cleanup");
}
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("pass path to split RC probe shader");
    VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC sparse test: Vulkan unavailable");}
    std::vector<Sample> samples;for(int i=0;i<192;++i){const int n=i%24;samples.push_back({float(n-12)+.25F,float((n%3)-1)+.25F,-.25F,1.0F,n%4,n%6,0,0});}
    for(int level=0;level<4;++level) for(int sheet=0;sheet<6;++sheet)
        samples.push_back({.25F,.25F,.25F,1,level,sheet,0,0});
    samples.push_back({-128,0,0,1,0,0,0,0});samples.push_back({127.99F,0,0,1,0,0,0,0});
    samples.push_back({128,0,0,1,0,0,0,0});samples.push_back({-128.01F,0,0,1,0,0,0,0});samples.push_back({0,0,0,0,0,0,0,0});samples.push_back({0,0,0,1,0,6,0,0});samples.push_back({0,0,0,1,4,0,0,0});
    samples.push_back({std::numeric_limits<float>::quiet_NaN(),0,0,1,0,0,0,0});
    samples.push_back({0,0,0,std::numeric_limits<float>::max(),3,0,0,0});
    const auto shader=load(argv[1]);run(d,shader,samples,127,7,0);
    std::vector<Sample> crowded;for(int i=0;i<32;++i)crowded.push_back({float(i)+.5F,0,0,1,0,0,0,0});run(d,shader,crowded,8,0,24);
    if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
    std::cout<<"Sparse probe insertion, duplicate keys, range and overflow passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
