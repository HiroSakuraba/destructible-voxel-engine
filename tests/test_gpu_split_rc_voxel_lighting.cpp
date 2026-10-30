#include "dve_test_skip.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>
using namespace dve::rhi;
namespace {
std::string error;void check(bool b,const std::string& m){if(!b)throw std::runtime_error(m+": "+error);}
struct V{float x,y,z,w;};struct Ray{V origin,direction,sky;};struct Output{V hit,sky;};
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"open shader");auto n=f.tellg();std::vector<std::byte> v(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(v.data()),n);check(bool(f),"read shader");return v;}
void near(V v,float x,float y,float z,const char* message){check(std::abs(v.x-x)<1e-4F&&std::abs(v.y-y)<1e-4F&&std::abs(v.z-z)<1e-4F,message);}
}
int main(int argc,char** argv){try{
 if(argc!=2)throw std::runtime_error("pass voxel-trace shader");VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("Split RC voxel light: Vulkan unavailable");}
 std::vector<Ray> rays={{{1.5F,1.5F,1.5F,.5F},{1,0,0,20},{.1F,.2F,.3F,0}},{{-1,5.5F,1.5F,.5F},{1,0,0,20},{.1F,.2F,.3F,0}}};
 constexpr float pi=3.141592653589793F;std::array<V,6> info{{{0,0,0,1},{8,8,8,0},{.4F,.6F,.8F,0},{.2F,.2F,.2F,0},{-1,0,0,pi},{1,.5F,.25F,20}}};std::array<V,2> palette{{{.5F,.25F,.75F,.25F},{.1F,.2F,.3F,0}}};std::vector<unsigned> grid(128);const unsigned target=(1*8+1)*8+2,blocker=(1*8+1)*8;grid[target/4]|=1u<<((target%4)*8);
 const auto make=[&](std::size_t n){BufferDesc b;b.bytes=n;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;return d.create_buffer(b,&error);};const auto a=make(rays.size()*sizeof(Ray)),b=make(sizeof(info)),c=make(grid.size()*4),e=make(sizeof(palette)),f=make(rays.size()*sizeof(Output));check(a&&b&&c&&e&&f,"buffers");check(d.write_buffer(a,0,std::as_bytes(std::span(rays)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(info)),&error)&&d.write_buffer(c,0,std::as_bytes(std::span(grid)),&error)&&d.write_buffer(e,0,std::as_bytes(std::span(palette)),&error),"upload");
 BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{3,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{4,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,a,{},0,rays.size()*sizeof(Ray),{}},{1,b,{},0,sizeof(info),{}},{2,c,{},0,grid.size()*4,{}},{3,e,{},0,sizeof(palette),{}},{4,f,{},0,rays.size()*sizeof(Output),{}}};const auto group=d.create_bind_group(gd,&error);ComputePipelineDesc pd;pd.bytecode=load(argv[1]);pd.bindGroupLayouts={layout};const auto pipeline=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipeline,"pipeline");
 const auto run=[&](){const auto cmd=d.begin_commands(QueueKind::Compute,"split RC voxel lighting",&error);check(d.bind_compute_bind_group(cmd,0,group,&error)&&d.dispatch(cmd,pipeline,1,1,1,&error),"dispatch");const auto fence=d.submit(cmd,&error);check(fence&&d.fence_complete(fence),"fence");std::vector<Output> output(rays.size());check(d.read_buffer(f,0,std::as_writable_bytes(std::span(output)),&error),"readback");return output;};
 auto visible=run();near(visible[0].hit,.5875F,.36875F,.721875F,"sunlit diffuse plus emission");check(std::abs(visible[0].hit.w-.5F)<1e-5F&&visible[1].hit.w==-1,"hit and miss distances");near(visible[1].sky,.1F,.2F,.3F,"sky miss");
 grid[blocker/4]|=1u<<((blocker%4)*8);check(d.write_buffer(c,0,std::as_bytes(std::span(grid)),&error),"add blocker");auto shadowed=run();near(shadowed[0].hit,.2125F,.275F,.58125F,"occluded sun retains sky and emission");
 grid[blocker/4]&=~(255u<<((blocker%4)*8));info[4].w=0;check(d.write_buffer(c,0,std::as_bytes(std::span(grid)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(info)),&error),"disable sun");auto night=run();near(night[0].hit,.2125F,.275F,.58125F,"sun off");
 palette[0].w=1;check(d.write_buffer(e,0,std::as_bytes(std::span(palette)),&error),"metal material");auto metal=run();near(metal[0].hit,.1F,.2F,.3F,"metallic suppresses diffuse bounce");
 check(d.destroy_compute_pipeline(pipeline,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error),"cleanup");for(auto h:{a,b,c,e,f})check(d.destroy_buffer(h,&error),"buffer cleanup");if(std::getenv("DVE_VULKAN_VALIDATION")){check(d.validation_enabled(),"validation enabled");check(d.validation_error_count()==0,"validation errors");}std::cout<<"Voxel sky, sun, shadow, emission and metallic lighting passed on "<<d.capabilities().adapterName<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
