#include "dve_test_skip.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include "dve/transform.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>
using namespace dve;
using namespace dve::rhi;
namespace {
std::string error;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message+": "+error);}
struct Float4 { float x,y,z,w; };
struct Ray { Float4 hit; Float4 sky; };
static_assert(sizeof(Ray)==32 && sizeof(Float4)==16);
std::vector<std::byte> load(const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary|std::ios::ate);check(bool(input),"open SPIR-V");
    const auto size=input.tellg();std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);input.read(reinterpret_cast<char*>(bytes.data()),size);check(bool(input),"read SPIR-V");return bytes;
}
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("pass path to compiled split_rc_intervals_cs.spv");
    // Boundaries, empty intervals, occluders, sky misses, distant hits, and
    // more than one workgroup exercise both the interval convention and bounds.
    constexpr float t0=.5F;
    const std::array<float,13> distances{-1.F,0.F,.49F,.5F,1.99F,2.F,7.99F,8.F,31.99F,32.F,100.F,.125F,4.F};
    const std::array<unsigned,13> terminal{3,0,0,1,1,2,2,3,3,3,3,0,2};
    const std::array<bool,13> surface{false,true,true,true,true,true,true,true,true,false,false,true,true};
    std::vector<Ray> rays;
    for(unsigned i=0;i<137;++i){const auto n=i%distances.size();rays.push_back({{.25F,.5F,.75F,distances[n]},{.8F,.4F,.2F,t0}});}
    VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC test: Vulkan unavailable");}
    const auto make=[&](std::size_t bytes){BufferDesc b;b.bytes=bytes;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};
    const auto input=make(rays.size()*sizeof(Ray)), output=make(rays.size()*4*sizeof(Float4));check(input&&output,"buffers");
    check(d.write_buffer(input,0,std::as_bytes(std::span(rays)),&error),"upload");
    BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
    const auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,input,{},0,rays.size()*sizeof(Ray),{}},{1,output,{},0,rays.size()*4*sizeof(Float4),{}}};
    const auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={layout};const auto pipeline=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipeline,"compute pipeline");
    const auto commands=d.begin_commands(QueueKind::Compute,"split RC interval conformance",&error);
    check(d.bind_compute_bind_group(commands,0,group,&error)&&d.dispatch(commands,pipeline,static_cast<unsigned>((rays.size()+63)/64),1,1,&error),"dispatch");
    const auto fence=d.submit(commands,&error);check(fence&&d.fence_complete(fence),"fence");
    std::vector<Float4> actual(rays.size()*4);check(d.read_buffer(output,0,std::as_writable_bytes(std::span(actual)),&error),"readback");
    for(unsigned i=0;i<rays.size();++i)for(unsigned level=0;level<4;++level){
        const auto n=i%distances.size();const auto v=actual[i*4+level];
        const float beta=level<terminal[n]?1.F:level==terminal[n]?0.F:-1.F;
        const Float4 expected=level==terminal[n]?(surface[n]?Float4{.25F,.5F,.75F,beta}:Float4{.8F,.4F,.2F,beta}):Float4{0,0,0,beta};
        check(std::abs(v.x-expected.x)<1e-6F&&std::abs(v.y-expected.y)<1e-6F&&std::abs(v.z-expected.z)<1e-6F&&v.w==expected.w,"interval mismatch ray "+std::to_string(i)+" level "+std::to_string(level));
    }
    check(d.destroy_compute_pipeline(pipeline,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error)&&d.destroy_buffer(input,&error)&&d.destroy_buffer(output,&error),"cleanup");
    if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}
    std::cout<<rays.size()<<" rays, "<<rays.size()*4<<" split intervals passed on "<<d.capabilities().adapterName<<"\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
